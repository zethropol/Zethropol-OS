#include "zethropol_monitoring_client.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>

static QVariant monitoringValueToVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return monitoringValueToVariant(value.value<QDBusVariant>().variant());

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto argument = value.value<QDBusArgument>();
    if (argument.currentType() != QDBusArgument::MapType)
        return {};
    QVariantMap map;

    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        QVariant nestedValue;
        argument >> key >> nestedValue;
        map.insert(key, monitoringValueToVariant(nestedValue));
        argument.endMapEntry();
    }
    argument.endMap();

    return map;
}

ZethropolMonitoringClient::ZethropolMonitoringClient(QObject *parent)
    : QObject(parent)
{
    refreshTimer.setInterval(1000);
    connect(&refreshTimer, &QTimer::timeout,
            this, &ZethropolMonitoringClient::requestState);
    QTimer::singleShot(0, this, &ZethropolMonitoringClient::requestState);
    refreshTimer.start();
}

QVariantMap ZethropolMonitoringClient::state() const
{
    return m_state;
}

void ZethropolMonitoringClient::requestState()
{
    if (m_loading)
        return;

    const auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        const bool changed = m_available || !m_state.isEmpty() || m_error.isEmpty();
        m_available = false;
        m_state.clear();
        m_error = QStringLiteral("Monitoring D-Bus connection is unavailable.");
        if (changed)
            emit stateChanged();
        return;
    }

    m_loading = true;
    const auto msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Monitoring"),
        QStringLiteral("/org/zethropol/Monitoring"),
        QStringLiteral("org.zethropol.Monitoring.v1"),
        QStringLiteral("GetState"));

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &ZethropolMonitoringClient::readState);
}

void ZethropolMonitoringClient::readState()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;

    m_loading = false;
    QDBusPendingReply<QVariantMap> reply = *watcher;
    watcher->deleteLater();

    if (reply.isError()) {
        const QString err = reply.error().message().isEmpty()
            ? QStringLiteral("Monitoring service is unavailable.")
            : reply.error().message();
        const bool changed = m_available || !m_state.isEmpty() || m_error != err;
        m_available = false;
        m_state.clear();
        m_error = err;
        if (changed)
            emit stateChanged();
        return;
    }

    const QVariantMap result = reply.value();
    if (result.value(QStringLiteral("success")).toBool()
        && result.value(QStringLiteral("code")).toString() == QStringLiteral("ok")) {
        const QVariant rawData = result.value(QStringLiteral("data"));
        const QVariant converted = monitoringValueToVariant(rawData);
        const QVariantMap nextState = converted.toMap();

        if (!m_available || !m_error.isEmpty() || nextState != m_state) {
            m_available = true;
            m_error.clear();
            m_state = nextState;
            emit stateChanged();
        }
    } else {
        const QString err = result.value(QStringLiteral("message")).toString().isEmpty()
            ? QStringLiteral("Monitoring service returned no valid data.")
            : result.value(QStringLiteral("message")).toString();
        const bool changed = m_available || !m_state.isEmpty() || m_error != err;
        m_available = false;
        m_state.clear();
        m_error = err;
        if (changed)
            emit stateChanged();
    }
}
