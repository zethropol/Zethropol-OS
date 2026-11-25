#include "MonitoringBridge.h"

namespace zethropol::monitoring {

MonitoringBridge::MonitoringBridge(QObject *parent)
    : QObject(parent)
{
    attachClient(MonitoringSharedClient::acquireShared());
}

MonitoringBridge::MonitoringBridge(std::shared_ptr<MonitoringSharedClient> sharedClient,
                                   QObject *parent)
    : QObject(parent)
{
    attachClient(std::move(sharedClient));
}

MonitoringBridge::~MonitoringBridge()
{
    if (m_client) {
        disconnect(m_client.get(), nullptr, this, nullptr);
        m_client.reset();
    }
}

void MonitoringBridge::attachClient(std::shared_ptr<MonitoringSharedClient> client)
{
    m_client = std::move(client);
    if (!m_client) {
        return;
    }

    connect(m_client.get(), &MonitoringSharedClient::metricsUpdated,
            this, &MonitoringBridge::metricsChanged);
    connect(m_client.get(), &MonitoringSharedClient::stateUpdated,
            this, &MonitoringBridge::stateChanged);
}

bool MonitoringBridge::connected() const
{
    return m_client && m_client->snapshot().connected;
}

bool MonitoringBridge::stateAvailable() const
{
    return m_client && m_client->snapshot().stateAvailable;
}

QString MonitoringBridge::readiness() const
{
    return m_client ? m_client->snapshot().readiness : QStringLiteral("dependency-failed");
}

QString MonitoringBridge::errorCode() const
{
    return m_client ? m_client->snapshot().errorCode : QStringLiteral("not-available");
}

QString MonitoringBridge::errorMessage() const
{
    return m_client ? m_client->snapshot().errorMessage : QStringLiteral("Bridge not initialized.");
}

QVariantMap MonitoringBridge::metrics() const
{
    return m_client ? m_client->snapshot().metrics : QVariantMap{};
}

QDateTime MonitoringBridge::lastUpdate() const
{
    return m_client ? m_client->snapshot().lastUpdate : QDateTime{};
}

qint64 MonitoringBridge::lastUpdateMs() const
{
    if (!m_client || !m_client->snapshot().lastUpdate.isValid()) {
        return 0;
    }
    return m_client->snapshot().lastUpdate.toMSecsSinceEpoch();
}

bool MonitoringBridge::hasMetric(const QString &key) const
{
    return m_client && m_client->snapshot().metrics.contains(key);
}

QVariant MonitoringBridge::metricValue(const QString &key) const
{
    if (!m_client) {
        return {};
    }
    const auto &map = m_client->snapshot().metrics;
    auto it = map.constFind(key);
    if (it == map.cend()) {
        return {};
    }
    return it.value();
}

} // namespace zethropol::monitoring
