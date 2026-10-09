#include "zethropol_monitoring_client.h"

#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>

static QVariant monitoringValueToVariant(const QVariant &value)
{
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto argument = value.value<QDBusArgument>();
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
    , monitoringInterface(
          QStringLiteral("org.zethropol.Monitoring"),
          QStringLiteral("/org/zethropol/Monitoring"),
          QStringLiteral("org.zethropol.Monitoring.v1"),
          QDBusConnection::systemBus(),
          this)
{
    refreshTimer.setInterval(1000);
    connect(&refreshTimer, &QTimer::timeout,
            this, &ZethropolMonitoringClient::requestState);
    requestState();
    refreshTimer.start();
}

QVariantMap ZethropolMonitoringClient::state() const
{
    return m_state;
}

void ZethropolMonitoringClient::requestState()
{
    auto *watcher = new QDBusPendingCallWatcher(
        monitoringInterface.asyncCall(QStringLiteral("GetState")), this);

    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &ZethropolMonitoringClient::readState);
}

void ZethropolMonitoringClient::readState()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;

    QDBusPendingReply<QVariantMap> reply = *watcher;

    if (!reply.isError()) {
        const QVariantMap result = reply.value();

        if (result.value(QStringLiteral("success")).toBool()
            && result.value(QStringLiteral("code")).toString() == QStringLiteral("ok")) {
            const QVariant rawData = result.value(QStringLiteral("data"));
            const QVariant converted = monitoringValueToVariant(rawData);
            const QVariantMap nextState = converted.toMap();

            if (nextState != m_state) {
                m_state = nextState;
                emit stateChanged();
            }
        }
    }

    watcher->deleteLater();
}
