#include "ServiceService.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>
#include <functional>

namespace {

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

ServiceService::ServiceService(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QVariantList ServiceService::services() const
{
    return m_services;
}

QVariantList ServiceService::failedServices() const
{
    QVariantList failed;
    for (const QVariant &item : m_services) {
        if (item.toMap().value(QStringLiteral("active")).toString() == QStringLiteral("failed"))
            failed.append(item);
    }
    return failed;
}

int ServiceService::total() const
{
    return m_total;
}

int ServiceService::active() const
{
    return m_active;
}

int ServiceService::failed() const
{
    return m_failed;
}

bool ServiceService::busy() const
{
    return m_busy;
}

QString ServiceService::actionStatus() const
{
    return m_actionStatus;
}

QString ServiceService::failureDetailsName() const { return m_failureDetailsName; }
QString ServiceService::failureDetailsText() const { return m_failureDetailsText; }
bool ServiceService::failureDetailsBusy() const { return m_failureDetailsBusy; }

void ServiceService::loadFailureDetails(const QString &name)
{
    if (m_failureDetailsBusy)
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
    QProcess unitsProcess;
    unitsProcess.start(QStringLiteral("/usr/bin/systemctl"),
                       {QStringLiteral("list-units"),
                        QStringLiteral("--type=service"),
                        QStringLiteral("--all"),
                        QStringLiteral("--no-legend"),
                        QStringLiteral("--plain"),
                        QStringLiteral("--no-pager")});
    unitsProcess.waitForFinished(5000);

    QProcess filesProcess;
    filesProcess.start(QStringLiteral("/usr/bin/systemctl"),
                       {QStringLiteral("list-unit-files"),
                        QStringLiteral("--type=service"),
                        QStringLiteral("--no-legend"),
                        QStringLiteral("--plain"),
                        QStringLiteral("--no-pager")});
    filesProcess.waitForFinished(5000);

    QVariantMap servicesByName;

    const QStringList unitLines =
        QString::fromLocal8Bit(unitsProcess.readAllStandardOutput())
            .split(QRegularExpression(QStringLiteral("[\r\n]+")),
                   Qt::SkipEmptyParts);

    for (const QString &line : unitLines) {
        const QStringList fields =
            line.trimmed().split(QRegularExpression(QStringLiteral("[[:space:]]+")),
                                 Qt::KeepEmptyParts);

        if (fields.size() < 4)
            continue;

        QVariantMap service;
        service.insert(QStringLiteral("name"), fields.at(0));
        service.insert(QStringLiteral("load"), fields.at(1));
        service.insert(QStringLiteral("active"), fields.at(2));
        service.insert(QStringLiteral("sub"), fields.at(3));

        const QString description =
            line.section(QRegularExpression(QStringLiteral("[[:space:]]+")),
                         4)
                .trimmed();

        service.insert(QStringLiteral("description"), description);
        service.insert(QStringLiteral("enabled"), QStringLiteral("unknown"));

        servicesByName.insert(fields.at(0), service);
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

        const bool canEnable =
            unitFileState == QStringLiteral("disabled") ||
            unitFileState == QStringLiteral("indirect");

        const bool canDisable =
            unitFileState == QStringLiteral("enabled") ||
            unitFileState == QStringLiteral("enabled-runtime") ||
            unitFileState == QStringLiteral("linked") ||
            unitFileState == QStringLiteral("linked-runtime");

        service.insert(QStringLiteral("enabled"), unitFileState);
        service.insert(QStringLiteral("masked"), masked);
        service.insert(QStringLiteral("canEnable"), canEnable);
        service.insert(QStringLiteral("canDisable"), canDisable);
        servicesByName.insert(name, service);
    }

    QVariantList services;
    int activeCount = 0;
    int failedCount = 0;

    for (auto it = servicesByName.cbegin(); it != servicesByName.cend(); ++it) {
        const QVariantMap service = it.value().toMap();
        const QString activeState =
            service.value(QStringLiteral("active")).toString();

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


bool ServiceService::serviceExists(const QString &name) const
{
    if (name.isEmpty())
        return false;

    for (const QVariant &item : m_services) {
        const QVariantMap service = item.toMap();
        if (service.value(QStringLiteral("name")).toString() == name)
            return true;
    }

    return false;
}

void ServiceService::runAction(const QString &name, const QString &action)
{
    if (m_busy || !serviceExists(name))
        return;

    m_busy = true;
    m_actionStatus = QStringLiteral("%1 %2...").arg(action, name);
    emit stateChanged();

    auto *process = new QProcess(this);

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            m_actionStatus = QStringLiteral("Could not start service action.");
        else
            m_actionStatus = QStringLiteral("Service action failed.");

        m_busy = false;
        process->deleteLater();
        emit stateChanged();
    });

    connect(process, &QProcess::finished, this,
            [this, process, action](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output =
            QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();

        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            m_actionStatus =
                QStringLiteral("Service %1 completed successfully.").arg(action);
            m_busy = false;
            process->deleteLater();
            refresh();
            emit stateChanged();
            return;
        }

        m_actionStatus = !output.isEmpty()
            ? output
            : QStringLiteral("Service %1 failed.").arg(action);

        m_busy = false;
        process->deleteLater();
        emit stateChanged();
    });

    process->setProgram(QStringLiteral("/usr/bin/pkexec"));
    process->setArguments({
        QStringLiteral("/usr/bin/systemctl"),
        action,
        name
    });
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();
}

void ServiceService::startService(const QString &name)
{
    runAction(name, QStringLiteral("start"));
}

void ServiceService::stopService(const QString &name)
{
    runAction(name, QStringLiteral("stop"));
}

void ServiceService::restartService(const QString &name)
{
    runAction(name, QStringLiteral("restart"));
}

void ServiceService::enableService(const QString &name)
{
    runAction(name, QStringLiteral("enable"));
}

void ServiceService::disableService(const QString &name)
{
    runAction(name, QStringLiteral("disable"));
}
