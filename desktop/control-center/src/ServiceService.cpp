#include "ServiceService.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <functional>

namespace {

const QSet<QString> s_validActions = {
    QStringLiteral("start"),
    QStringLiteral("stop"),
    QStringLiteral("restart"),
    QStringLiteral("enable"),
    QStringLiteral("disable")
};

const QSet<QString> s_quarantinedServices = {
    QStringLiteral("zethropol-performance.service")
};

const QSet<QString> s_protectedCriticalServices = {
    QStringLiteral("dbus.service"),
    QStringLiteral("dbus-broker.service"),
    QStringLiteral("systemd-journald.service"),
    QStringLiteral("systemd-logind.service"),
    QStringLiteral("systemd-udevd.service"),
    QStringLiteral("polkit.service"),
    QStringLiteral("zethropol-services.service"),
    QStringLiteral("zethropol-recovery.service"),
    QStringLiteral("display-manager.service"),
    QStringLiteral("sddm.service"),
    QStringLiteral("plasmalogin.service"),
    QStringLiteral("gdm.service")
};

QVariant decodeServiceDbusValue(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return decodeServiceDbusValue(value.value<QDBusVariant>().variant());

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto arg = value.value<QDBusArgument>();
    if (arg.currentType() == QDBusArgument::StructureType) {
        QVariantMap map;
        bool ok = false;
        QString code, message, opId;
        arg.beginStructure();
        arg >> ok >> code >> message >> opId;
        arg.endStructure();
        map.insert(QStringLiteral("success"), ok);
        map.insert(QStringLiteral("code"), code);
        map.insert(QStringLiteral("message"), message);
        map.insert(QStringLiteral("operation_id"), opId);
        return map;
    }
    if (arg.currentType() == QDBusArgument::MapType) {
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QString k;
            QVariant v;
            arg >> k >> v;
            map.insert(k, decodeServiceDbusValue(v));
            arg.endMapEntry();
        }
        arg.endMap();
        return map;
    }
    return value;
}

void readServiceCommand(QObject *owner, const QString &program, const QStringList &arguments,
                        std::function<void(bool, const QString &)> complete)
{
    auto *process = new QProcess(owner);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SYSTEMD_COLORS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(process, &QProcess::errorOccurred, owner,
                     [process, complete](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        complete(false, process->errorString());
        process->deleteLater();
    });
    QObject::connect(process, &QProcess::finished, owner,
                     [process, complete](int exitCode, QProcess::ExitStatus status) {
        QString output = QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();
        if (output.size() > 65536)
            output = output.left(65536) + QStringLiteral("\n[Çıktı sınırlandırıldı]");
        if (process->property("timedOut").toBool())
            output = QStringLiteral("Sorgu zaman aşımına uğradı.");
        complete(status == QProcess::NormalExit && exitCode == 0
                     && !process->property("timedOut").toBool(), output);
        process->deleteLater();
    });
    QTimer::singleShot(5000, process, [process]() {
        if (process->state() != QProcess::NotRunning) {
            process->setProperty("timedOut", true);
            process->kill();
        }
    });
    process->start(program, arguments);
}

} // namespace

bool ServiceService::isValidServiceName(const QString &name)
{
    if (name.isEmpty() || name.size() > 256)
        return false;
    if (name.startsWith(QLatin1Char('-')) || name.startsWith(QLatin1Char('.')))
        return false;
    if (!name.endsWith(QStringLiteral(".service")) || name.endsWith(QStringLiteral("@.service")))
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QStringLiteral("..")))
        return false;
    static const QRegularExpression allowedRe(QStringLiteral("^[A-Za-z0-9_.@:-]+\\.service$"));
    return allowedRe.match(name).hasMatch();
}

bool ServiceService::isQuarantinedService(const QString &name)
{
    return s_quarantinedServices.contains(name);
}

bool ServiceService::isProtectedAction(const QString &name, const QString &action)
{
    if (s_quarantinedServices.contains(name))
        return true;
    if (s_protectedCriticalServices.contains(name)
        && (action == QStringLiteral("stop")
            || action == QStringLiteral("disable")
            || action == QStringLiteral("restart"))) {
        return true;
    }
    return false;
}

bool ServiceService::validateServiceRequest(const QString &name, const QString &action, QString *reason)
{
    if (!s_validActions.contains(action)) {
        if (reason)
            *reason = QStringLiteral("Desteklenmeyen servis işlemi.");
        return false;
    }
    if (!isValidServiceName(name)) {
        if (reason)
            *reason = QStringLiteral("Geçersiz systemd servis adı.");
        return false;
    }
    if (isQuarantinedService(name)) {
        if (reason)
            *reason = QStringLiteral("Karantinadaki servis (%1) Services üzerinden yönetilemez.").arg(name);
        return false;
    }
    if (isProtectedAction(name, action)) {
        if (reason)
            *reason = QStringLiteral("Kritik sistem servisi (%1) durdurulamaz, devre dışı bırakılamaz veya yeniden başlatılamaz.").arg(name);
        return false;
    }
    return true;
}

ServiceService::ServiceService(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QVariantList ServiceService::services() const { return m_services; }

QVariantList ServiceService::failedServices() const
{
    QVariantList failed;
    for (const QVariant &item : m_services) {
        if (item.toMap().value(QStringLiteral("active")).toString() == QStringLiteral("failed"))
            failed.append(item);
    }
    return failed;
}

int ServiceService::total() const { return m_total; }
int ServiceService::active() const { return m_active; }
int ServiceService::failed() const { return m_failed; }
bool ServiceService::busy() const { return m_busy; }
QString ServiceService::actionStatus() const { return m_actionStatus; }
QString ServiceService::failureDetailsName() const { return m_failureDetailsName; }
QString ServiceService::failureDetailsText() const { return m_failureDetailsText; }
bool ServiceService::failureDetailsBusy() const { return m_failureDetailsBusy; }

void ServiceService::loadFailureDetails(const QString &name)
{
    if (m_failureDetailsBusy || !isValidServiceName(name))
        return;
    bool failed = false;
    for (const QVariant &item : failedServices()) {
        if (item.toMap().value(QStringLiteral("name")).toString() == name) {
            failed = true;
            break;
        }
    }
    if (!failed)
        return;

    m_failureDetailsName = name;
    m_failureDetailsText = QStringLiteral("Hata ayrıntıları okunuyor…");
    m_failureDetailsBusy = true;
    emit stateChanged();
    readServiceCommand(this, QStringLiteral("/usr/bin/systemctl"),
        {QStringLiteral("show"), QStringLiteral("--no-pager"),
         QStringLiteral("--property=ActiveState,SubState,Result,ExecMainCode,ExecMainStatus,StatusText,ExecMainExitTimestamp,InvocationID"),
         QStringLiteral("--"), name},
        [this, name](bool success, const QString &output) {
        if (m_failureDetailsName != name)
            return;
        QVariantMap properties;
        if (success) {
            for (const QString &line : output.split(QLatin1Char('\n'))) {
                const int separator = line.indexOf(QLatin1Char('='));
                if (separator > 0)
                    properties.insert(line.left(separator), line.mid(separator + 1));
            }
        }
        QStringList details{QStringLiteral("Servis: %1").arg(name)};
        if (!success) {
            details << QStringLiteral("Servis sonucu okunamadı: %1").arg(output);
        } else {
            const QString state = properties.value(QStringLiteral("ActiveState")).toString();
            if (!state.isEmpty() && state != QStringLiteral("failed"))
                details << QStringLiteral("Servis şu anda failed durumunda değil: %1 / %2")
                    .arg(state, properties.value(QStringLiteral("SubState")).toString());
            const QString result = properties.value(QStringLiteral("Result")).toString();
            const QMap<QString, QString> reasons{
                {QStringLiteral("exit-code"), QStringLiteral("Süreç hata koduyla sonlandı")},
                {QStringLiteral("signal"), QStringLiteral("Süreç bir sinyalle sonlandırıldı")},
                {QStringLiteral("core-dump"), QStringLiteral("Süreç çöktü")},
                {QStringLiteral("timeout"), QStringLiteral("İşlem zaman aşımına uğradı")},
                {QStringLiteral("oom-kill"), QStringLiteral("Bellek yetersizliği nedeniyle sonlandırıldı")},
                {QStringLiteral("start-limit-hit"), QStringLiteral("Tekrarlanan başlatma sınırına ulaşıldı")},
                {QStringLiteral("success"), QStringLiteral("Son çalıştırma başarılı")}
            };
            details << QStringLiteral("Sonuç: %1%2").arg(result.isEmpty() ? QStringLiteral("bilinmiyor") : result,
                reasons.contains(result) ? QStringLiteral(" — ") + reasons.value(result) : QString());
            const QString code = properties.value(QStringLiteral("ExecMainCode")).toString();
            const QString status = properties.value(QStringLiteral("ExecMainStatus")).toString();
            if (code == QStringLiteral("1"))
                details << QStringLiteral("Çıkış kodu: %1").arg(status);
            else if (code == QStringLiteral("2") || code == QStringLiteral("3"))
                details << QStringLiteral("Sonlandıran sinyal: %1").arg(status);
            const QString statusText = properties.value(QStringLiteral("StatusText")).toString();
            if (!statusText.isEmpty())
                details << QStringLiteral("Durum açıklaması: %1").arg(statusText);
            const QString timestamp = properties.value(QStringLiteral("ExecMainExitTimestamp")).toString();
            if (!timestamp.isEmpty())
                details << QStringLiteral("Sonlanma zamanı: %1").arg(timestamp);
        }
        const QString invocation = properties.value(QStringLiteral("InvocationID")).toString();
        const bool scoped = QRegularExpression(QStringLiteral("^[0-9a-fA-F]{32}$")).match(invocation).hasMatch()
            && invocation != QString(32, QLatin1Char('0'));
        details << QString() << (scoped
            ? QStringLiteral("Son çalıştırmanın günlük kayıtları (en fazla 30):")
            : QStringLiteral("Bu açılışın son günlük kayıtları (en fazla 30):"));
        QStringList arguments{QStringLiteral("--unit=") + name, QStringLiteral("--boot"),
            QStringLiteral("--no-pager"), QStringLiteral("--lines=30"),
            QStringLiteral("--output=short-iso"), QStringLiteral("--quiet")};
        if (scoped)
            arguments << QStringLiteral("_SYSTEMD_INVOCATION_ID=") + invocation;
        const QString summary = details.join(QLatin1Char('\n'));
        m_failureDetailsText = summary + QStringLiteral("\nOkunuyor…");
        emit stateChanged();
        readServiceCommand(this, QStringLiteral("/usr/bin/journalctl"), arguments,
            [this, name, summary](bool journalSuccess, const QString &journal) {
            if (m_failureDetailsName != name)
                return;
            const QString log = journalSuccess
                ? (journal.isEmpty() ? QStringLiteral("Bu çalıştırma için mevcut kullanıcı yetkileriyle görüntülenebilir kayıt bulunamadı.") : journal)
                : QStringLiteral("Günlük okunamadı; erişim izni bulunmayabilir.\n%1").arg(journal);
            m_failureDetailsText = summary + QLatin1Char('\n') + log;
            m_failureDetailsBusy = false;
            emit stateChanged();
        });
    });
}

void ServiceService::refresh()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SYSTEMD_COLORS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));

    QProcess unitsProcess;
    unitsProcess.setProcessEnvironment(environment);
    unitsProcess.start(QStringLiteral("/usr/bin/systemctl"),
                       {QStringLiteral("list-units"),
                        QStringLiteral("--type=service"),
                        QStringLiteral("--all"),
                        QStringLiteral("--no-legend"),
                        QStringLiteral("--plain"),
                        QStringLiteral("--no-pager")});
    const bool unitsOk = unitsProcess.waitForFinished(5000)
        && unitsProcess.exitStatus() == QProcess::NormalExit
        && unitsProcess.exitCode() == 0;

    QProcess filesProcess;
    filesProcess.setProcessEnvironment(environment);
    filesProcess.start(QStringLiteral("/usr/bin/systemctl"),
                       {QStringLiteral("list-unit-files"),
                        QStringLiteral("--type=service"),
                        QStringLiteral("--no-legend"),
                        QStringLiteral("--plain"),
                        QStringLiteral("--no-pager")});
    const bool filesOk = filesProcess.waitForFinished(5000)
        && filesProcess.exitStatus() == QProcess::NormalExit
        && filesProcess.exitCode() == 0;

    if (!unitsOk && !filesOk) {
        m_actionStatus = QStringLiteral("Servis listesi okunamadı (systemd erişilemez).");
        emit stateChanged();
        return;
    }

    QVariantMap servicesByName;

    const QStringList unitLines =
        QString::fromLocal8Bit(unitsProcess.readAllStandardOutput())
            .split(QRegularExpression(QStringLiteral("[\r\n]+")),
                   Qt::SkipEmptyParts);

    for (const QString &line : unitLines) {
        const QString trimmed = line.trimmed();
        const QStringList fields =
            trimmed.split(QRegularExpression(QStringLiteral("[[:space:]]+")),
                          Qt::SkipEmptyParts);

        if (fields.size() < 4)
            continue;

        const QString name = fields.at(0);
        if (!isValidServiceName(name))
            continue;

        QVariantMap service;
        service.insert(QStringLiteral("name"), name);
        service.insert(QStringLiteral("load"), fields.at(1));
        service.insert(QStringLiteral("active"), fields.at(2));
        service.insert(QStringLiteral("sub"), fields.at(3));
        service.insert(QStringLiteral("description"), fields.mid(4).join(QLatin1Char(' ')));
        service.insert(QStringLiteral("enabled"), QStringLiteral("unknown"));

        servicesByName.insert(name, service);
    }

    const QStringList fileLines =
        QString::fromLocal8Bit(filesProcess.readAllStandardOutput())
            .split(QRegularExpression(QStringLiteral("[\r\n]+")),
                   Qt::SkipEmptyParts);

    for (const QString &line : fileLines) {
        const QStringList fields =
            line.trimmed().split(QRegularExpression(QStringLiteral("[[:space:]]+")),
                                 Qt::SkipEmptyParts);

        if (fields.size() < 2)
            continue;

        const QString name = fields.at(0);
        if (!isValidServiceName(name))
            continue;

        QVariantMap service = servicesByName.value(name).toMap();

        if (service.isEmpty()) {
            service.insert(QStringLiteral("name"), name);
            service.insert(QStringLiteral("load"), QStringLiteral("unknown"));
            service.insert(QStringLiteral("active"), QStringLiteral("inactive"));
            service.insert(QStringLiteral("sub"), QStringLiteral("dead"));
            service.insert(QStringLiteral("description"), QString());
        }

        const QString unitFileState = fields.at(1);
        const bool masked =
            unitFileState == QStringLiteral("masked") ||
            unitFileState == QStringLiteral("masked-runtime");
        const bool quarantined = isQuarantinedService(name);
        const bool protectedCritical = s_protectedCriticalServices.contains(name);

        const bool canEnable = !masked && !quarantined &&
            (unitFileState == QStringLiteral("disabled") ||
             unitFileState == QStringLiteral("indirect"));

        const bool canDisable = !masked && !quarantined && !protectedCritical &&
            (unitFileState == QStringLiteral("enabled") ||
             unitFileState == QStringLiteral("enabled-runtime") ||
             unitFileState == QStringLiteral("linked") ||
             unitFileState == QStringLiteral("linked-runtime"));

        service.insert(QStringLiteral("enabled"), unitFileState);
        service.insert(QStringLiteral("masked"), masked);
        service.insert(QStringLiteral("quarantined"), quarantined);
        service.insert(QStringLiteral("protected"), quarantined || protectedCritical);
        service.insert(QStringLiteral("canEnable"), canEnable);
        service.insert(QStringLiteral("canDisable"), canDisable);
        servicesByName.insert(name, service);
    }

    QVariantList services;
    int activeCount = 0;
    int failedCount = 0;

    for (auto it = servicesByName.cbegin(); it != servicesByName.cend(); ++it) {
        QVariantMap service = it.value().toMap();
        const QString name = service.value(QStringLiteral("name")).toString();
        const bool masked = service.value(QStringLiteral("masked")).toBool();
        const bool quarantined = isQuarantinedService(name);
        const bool protectedCritical = s_protectedCriticalServices.contains(name);
        service.insert(QStringLiteral("quarantined"), quarantined);
        service.insert(QStringLiteral("protected"), quarantined || protectedCritical);
        service.insert(QStringLiteral("canStart"), !masked && !quarantined
            && service.value(QStringLiteral("active")).toString() != QStringLiteral("active"));
        service.insert(QStringLiteral("canStop"), !quarantined && !protectedCritical
            && service.value(QStringLiteral("active")).toString() == QStringLiteral("active"));
        service.insert(QStringLiteral("canRestart"), !masked && !quarantined && !protectedCritical
            && service.value(QStringLiteral("active")).toString() == QStringLiteral("active"));

        const QString activeState = service.value(QStringLiteral("active")).toString();
        if (activeState == QStringLiteral("active"))
            ++activeCount;
        if (activeState == QStringLiteral("failed"))
            ++failedCount;

        services.append(service);
    }

    m_services = services;
    m_total = services.size();
    m_active = activeCount;
    m_failed = failedCount;

    bool selectedStillFailed = false;
    for (const QVariant &item : failedServices()) {
        if (item.toMap().value(QStringLiteral("name")).toString() == m_failureDetailsName)
            selectedStillFailed = true;
    }
    if (!selectedStillFailed) {
        m_failureDetailsName.clear();
        m_failureDetailsText.clear();
        m_failureDetailsBusy = false;
    }

    emit stateChanged();
}

bool ServiceService::serviceExists(const QString &name, QVariantMap *outService) const
{
    if (name.isEmpty())
        return false;

    for (const QVariant &item : m_services) {
        const QVariantMap service = item.toMap();
        if (service.value(QStringLiteral("name")).toString() == name) {
            if (outService)
                *outService = service;
            return true;
        }
    }

    return false;
}

void ServiceService::runAction(const QString &name, const QString &action)
{
    if (m_busy)
        return;

    QString validationError;
    if (!validateServiceRequest(name, action, &validationError)) {
        m_actionStatus = validationError;
        emit stateChanged();
        return;
    }

    QVariantMap serviceInfo;
    if (!serviceExists(name, &serviceInfo)) {
        m_actionStatus = QStringLiteral("Servis sistem listesinde bulunamadı: %1").arg(name);
        emit stateChanged();
        return;
    }

    if (serviceInfo.value(QStringLiteral("masked")).toBool()
        && (action == QStringLiteral("start")
            || action == QStringLiteral("enable")
            || action == QStringLiteral("restart"))) {
        m_actionStatus = QStringLiteral("Maskelenmiş servis (%1) başlatılamaz veya etkinleştirilemez.").arg(name);
        emit stateChanged();
        return;
    }

    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_actionStatus = QStringLiteral("Sistem D-Bus bağlantısı kurulamadı; servis işlemi reddedildi.");
        emit stateChanged();
        return;
    }

    m_busy = true;
    m_actionStatus = QStringLiteral("%1 %2…").arg(action, name);
    emit stateChanged();

    const QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Services"),
        QStringLiteral("/org/zethropol/Services"),
        QStringLiteral("org.zethropol.Services.v1"),
        QStringLiteral("ManageService"))
        << name
        << action;

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 125000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, action](QDBusPendingCallWatcher *) {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
            m_actionStatus = QStringLiteral("Servis yönetim arayüzüne (org.zethropol.Services) ulaşılamadı: %1")
                .arg(reply.errorMessage());
            m_busy = false;
            emit stateChanged();
            return;
        }

        const QVariantMap result = decodeServiceDbusValue(reply.arguments().constFirst()).toMap();
        const bool ok = result.value(QStringLiteral("success")).toBool();
        const QString message = result.value(QStringLiteral("message")).toString();

        m_busy = false;
        if (ok) {
            m_actionStatus = message.isEmpty()
                ? QStringLiteral("Servis %1 işlemi yetkilendirildi ve uygulandı.").arg(action)
                : message;
            QTimer::singleShot(500, this, &ServiceService::refresh);
        } else {
            m_actionStatus = message.isEmpty()
                ? QStringLiteral("Servis %1 işlemi reddedildi veya başarısız oldu.").arg(action)
                : message;
        }
        emit stateChanged();
    });
}

void ServiceService::startService(const QString &name) { runAction(name, QStringLiteral("start")); }
void ServiceService::stopService(const QString &name) { runAction(name, QStringLiteral("stop")); }
void ServiceService::restartService(const QString &name) { runAction(name, QStringLiteral("restart")); }
void ServiceService::enableService(const QString &name) { runAction(name, QStringLiteral("enable")); }
void ServiceService::disableService(const QString &name) { runAction(name, QStringLiteral("disable")); }
