#include "HardwareServiceClient.h"

#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QTimer>
#include <QDateTime>
#include <QDBusMessage>

HardwareServiceClient::HardwareServiceClient(QObject *parent)
    : QObject(parent)
{
    hardwareTimer.setInterval(10000);
    connect(&hardwareTimer, &QTimer::timeout, this, &HardwareServiceClient::refresh);
    hardwareTimer.start();
    QTimer::singleShot(0, this, &HardwareServiceClient::refresh);
}

QVariantMap HardwareServiceClient::state() const
{
    return m_state;
}

static QVariant dbusValueToVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return dbusValueToVariant(value.value<QDBusVariant>().variant());

    if (value.userType() == QMetaType::QVariantMap) {
        const QVariantMap source = value.toMap();
        QVariantMap result;
        for (auto it = source.cbegin(); it != source.cend(); ++it)
            result.insert(it.key(), dbusValueToVariant(it.value()));
        return result;
    }

    if (value.userType() == QMetaType::QVariantList) {
        const QVariantList source = value.toList();
        QVariantList result;
        result.reserve(source.size());
        for (const QVariant &item : source)
            result.append(dbusValueToVariant(item));
        return result;
    }

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto argument = value.value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::ArrayType) {
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd()) {
            QDBusVariant item;
            argument >> item;
            list.append(dbusValueToVariant(item.variant()));
        }
        argument.endArray();
        return list;
    }
    if (argument.currentType() != QDBusArgument::MapType)
        return {};
    QVariantMap map;
    argument.beginMap();

    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        QDBusVariant wrappedValue;
        argument >> key >> wrappedValue;
        map.insert(key, dbusValueToVariant(wrappedValue.variant()));
        argument.endMapEntry();
    }
    argument.endMap();
    return map;
}

void HardwareServiceClient::refresh()
{
    if (m_loading)
        return;
    const auto connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        m_available = false;
        m_state.clear();
        m_error = connection.lastError().message();
        if (m_error.isEmpty())
            m_error = QStringLiteral("Hardware D-Bus connection is unavailable.");
        emit stateChanged();
        return;
    }
    m_loading = true;
    emit stateChanged();
    const auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Hardware"), QStringLiteral("/org/zethropol/Hardware"),
        QStringLiteral("org.zethropol.Hardware.v1"), QStringLiteral("GetState"));
    auto *watcher = new QDBusPendingCallWatcher(
        connection.asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &HardwareServiceClient::readState);
}

static bool isMissingOrUnknown(const QVariant &value)
{
    if (!value.isValid() || value.isNull())
        return true;
    const QString text = value.toString().trimmed();
    return text.isEmpty() || text.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0;
}

static QHash<QString, QString> readKeyValueFile(const QString &path)
{
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('[')))
            continue;
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        QString val = line.mid(eq + 1).trimmed();
        if (val.size() >= 2
            && ((val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"')))
                || (val.startsWith(QLatin1Char('\'')) && val.endsWith(QLatin1Char('\''))))) {
            val = val.mid(1, val.size() - 2).trimmed();
        }
        if (!key.isEmpty() && !val.isEmpty())
            values.insert(key, val);
    }
    return values;
}

static bool desktopNameIndicatesKde(const QString &desktopName)
{
    const QStringList tokens = desktopName.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        if (token.trimmed().compare(QStringLiteral("KDE"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QVariantMap HardwareServiceClient::normalizeSystemInfo(
    QVariantMap system,
    const QProcessEnvironment &env,
    const QString &osReleasePath,
    const QStringList &sessionDesktopCandidates)
{
    if (isMissingOrUnknown(system.value(QStringLiteral("os_version")))) {
        const QHash<QString, QString> osRelease = readKeyValueFile(osReleasePath);
        for (const QString &key : {QStringLiteral("VERSION_ID"), QStringLiteral("VERSION"), QStringLiteral("BUILD_ID")}) {
            const QString candidate = osRelease.value(key).trimmed();
            if (!candidate.isEmpty()) {
                system.insert(QStringLiteral("os_version"), candidate);
                break;
            }
        }
    }

    if (isMissingOrUnknown(system.value(QStringLiteral("desktop_name")))) {
        QString desktop = env.value(QStringLiteral("XDG_CURRENT_DESKTOP")).trimmed();
        if (desktop.isEmpty())
            desktop = env.value(QStringLiteral("XDG_SESSION_DESKTOP")).trimmed();
        if (!desktop.isEmpty())
            system.insert(QStringLiteral("desktop_name"), desktop);
    }

    const QString resolvedDesktop = system.value(QStringLiteral("desktop_name")).toString().trimmed();
    const bool isKde = desktopNameIndicatesKde(resolvedDesktop)
        || env.value(QStringLiteral("KDE_FULL_SESSION")).trimmed().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;

    if (isMissingOrUnknown(system.value(QStringLiteral("desktop_version")))) {
        QString detectedVersion;
        if (isKde) {
            QStringList candidates = sessionDesktopCandidates;
            if (candidates.isEmpty()) {
                const QString desktopSession = env.value(QStringLiteral("DESKTOP_SESSION")).trimmed();
                if (desktopSession.endsWith(QStringLiteral(".desktop")) && desktopSession.startsWith(QLatin1Char('/')))
                    candidates.append(desktopSession);
                candidates.append(QStringLiteral("/usr/share/wayland-sessions/plasma.desktop"));
                candidates.append(QStringLiteral("/usr/share/xsessions/plasma.desktop"));
                candidates.append(QStringLiteral("/usr/share/xsessions/plasmax11.desktop"));
            }
            for (const QString &candidatePath : candidates) {
                const QHash<QString, QString> entries = readKeyValueFile(candidatePath);
                const QString pluginVer = entries.value(QStringLiteral("X-KDE-PluginInfo-Version")).trimmed();
                if (!pluginVer.isEmpty()) {
                    detectedVersion = pluginVer;
                    break;
                }
            }
            if (detectedVersion.isEmpty())
                detectedVersion = env.value(QStringLiteral("KDE_SESSION_VERSION")).trimmed();
        }
        if (!detectedVersion.isEmpty())
            system.insert(QStringLiteral("desktop_version"), detectedVersion);
    }

    if (isMissingOrUnknown(system.value(QStringLiteral("session_type")))) {
        QString session = env.value(QStringLiteral("XDG_SESSION_TYPE")).trimmed();
        if (session.isEmpty() && !env.value(QStringLiteral("WAYLAND_DISPLAY")).trimmed().isEmpty())
            session = QStringLiteral("wayland");
        else if (session.isEmpty() && !env.value(QStringLiteral("DISPLAY")).trimmed().isEmpty())
            session = QStringLiteral("x11");
        if (!session.isEmpty())
            system.insert(QStringLiteral("session_type"), session);
    }

    return system;
}

void HardwareServiceClient::readState()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;
    QDBusPendingReply<QVariantMap> reply = *watcher;
    m_loading = false;
    m_available = false;
    m_error.clear();
    if (reply.isError()) {
        m_error = reply.error().message();
    } else {
        const QVariantMap result = reply.value();
        QVariantMap data = dbusValueToVariant(result.value(QStringLiteral("data"))).toMap();
        if (result.value(QStringLiteral("success")).toBool()
            && result.value(QStringLiteral("code")).toString() == QStringLiteral("ok")
            && !data.isEmpty()) {
            if (data.contains(QStringLiteral("system"))) {
                data.insert(
                    QStringLiteral("system"),
                    normalizeSystemInfo(data.value(QStringLiteral("system")).toMap()));
            }
            m_state = data;
            m_available = true;
            m_lastUpdated = QDateTime::currentDateTime().toString(Qt::ISODate);
        } else {
            m_error = result.value(QStringLiteral("message")).toString();
            if (m_error.isEmpty())
                m_error = QStringLiteral("Hardware service returned no valid data.");
        }
    }
    if (!m_available)
        m_state.clear();
    emit stateChanged();
    watcher->deleteLater();
}
