#include "HardwareServiceClient.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QTimer>

HardwareServiceClient::HardwareServiceClient(QObject *parent)
    : QObject(parent)
    , hardwareInterface(
        QStringLiteral("org.zethropol.Hardware"),
        QStringLiteral("/org/zethropol/Hardware"),
        QStringLiteral("org.zethropol.Hardware.v1"),
        QDBusConnection::systemBus(),
        this)
    , performanceInterface(
        QStringLiteral("org.zethropol.Performance"),
        QStringLiteral("/org/zethropol/Performance"),
        QStringLiteral("org.zethropol.Performance.v1"),
        QDBusConnection::systemBus(),
        this)
{
    auto *stateWatcher = new QDBusPendingCallWatcher(
        hardwareInterface.asyncCall(QStringLiteral("GetState")),
        this
    );

    connect(
        stateWatcher,
        &QDBusPendingCallWatcher::finished,
        this,
        &HardwareServiceClient::readState
    );

    performanceTimer.setInterval(1000);
    connect(&performanceTimer, &QTimer::timeout, this, &HardwareServiceClient::requestPerformance);
    performanceTimer.start();

    requestPerformance();
}

QVariantMap HardwareServiceClient::state() const
{
    return m_state;
}

QVariantMap HardwareServiceClient::performance() const
{
    return m_performance;
}

static QVariant dbusValueToVariant(const QVariant &value)
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

        map.insert(key, dbusValueToVariant(nestedValue));

        argument.endMapEntry();
    }
    argument.endMap();

    return map;
}

void HardwareServiceClient::readState()
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
            m_state = dbusValueToVariant(rawData).toMap();
            emit stateChanged();
        }
    }

    watcher->deleteLater();
}

void HardwareServiceClient::requestPerformance()
{
    auto *watcher = new QDBusPendingCallWatcher(
        performanceInterface.asyncCall(QStringLiteral("GetState")),
        this
    );

    connect(
        watcher,
        &QDBusPendingCallWatcher::finished,
        this,
        &HardwareServiceClient::readPerformance
    );
}

void HardwareServiceClient::readPerformance()
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
            m_performance = dbusValueToVariant(rawData).toMap();
            emit performanceChanged();
        }
    }

    watcher->deleteLater();
}
