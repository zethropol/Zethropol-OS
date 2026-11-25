#include "MonitoringSharedClient.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QHash>
#include <QLoggingCategory>
#include <QMetaType>

#include <cmath>
#include <limits>

namespace zethropol::monitoring {

Q_LOGGING_CATEGORY(lcMonitoringBridge, "zethropol.monitoring.bridge")

namespace {

enum class MetricKind {
    Percent,
    TemperatureC,
    NonNegative,
    Positive,
};

struct MetricSpec {
    const char *key;
    MetricKind kind;
};

// Exact 25 normalized metrics exposed by `org.zethropol.Monitoring.v1`.
constexpr MetricSpec kMetricSpecs[] = {
    // CPU (3)
    {"cpu_usage_percent", MetricKind::Percent},
    {"cpu_temperature_c", MetricKind::TemperatureC},
    {"cpu_frequency_ghz", MetricKind::NonNegative},
    // GPU (7)
    {"gpu_usage_percent", MetricKind::Percent},
    {"gpu_memory_usage_percent", MetricKind::Percent},
    {"gpu_core_clock_mhz", MetricKind::NonNegative},
    {"gpu_vram_used_gb", MetricKind::NonNegative},
    {"gpu_vram_total_gb", MetricKind::Positive},
    {"gpu_temperature_c", MetricKind::TemperatureC},
    {"gpu_memory_clock_mhz", MetricKind::NonNegative},
    // Memory (2) — GiB values under legacy `_gb` keys
    {"memory_used_gb", MetricKind::NonNegative},
    {"memory_total_gb", MetricKind::Positive},
    // Storage (10) — decimal GB & %
    {"storage_used_gb", MetricKind::NonNegative},
    {"storage_capacity_gb", MetricKind::Positive},
    {"storage_available_gb", MetricKind::NonNegative},
    {"storage_temperature_c", MetricKind::TemperatureC},
    {"storage_filesystem_total_gb", MetricKind::Positive},
    {"storage_filesystem_used_gb", MetricKind::NonNegative},
    {"storage_filesystem_free_gb", MetricKind::NonNegative},
    {"storage_filesystem_available_gb", MetricKind::NonNegative},
    {"storage_filesystem_usage_percent", MetricKind::Percent},
    {"storage_physical_capacity_gb", MetricKind::Positive},
    // Network (3) — decimal Mbps & ms
    {"network_download_mbps", MetricKind::NonNegative},
    {"network_upload_mbps", MetricKind::NonNegative},
    {"network_ping_ms", MetricKind::NonNegative},
};

bool extractStrictNumericDouble(const QVariant &value, double *out)
{
    if (!value.isValid() || value.isNull()) {
        return false;
    }

    switch (value.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Char:
    case QMetaType::SChar:
    case QMetaType::UChar: {
        bool ok = false;
        const double num = value.toDouble(&ok);
        if (!ok || !std::isfinite(num)) {
            return false;
        }
        *out = (num == 0.0) ? 0.0 : num; // Normalize -0.0 -> 0.0
        return true;
    }
    default:
        // Explicitly reject Bool, QString, QVariantList, QVariantMap, etc.
        return false;
    }
}

bool isInMetricDomain(double v, MetricKind kind)
{
    if (!std::isfinite(v)) {
        return false;
    }
    switch (kind) {
    case MetricKind::Percent:
        return v >= 0.0 && v <= 100.0;
    case MetricKind::TemperatureC:
        return v >= -100.0 && v <= 250.0;
    case MetricKind::NonNegative:
        return v >= 0.0;
    case MetricKind::Positive:
        return v > 0.0;
    }
    return false;
}

QString normalizeReadiness(const QString &raw, bool success, const QString &code)
{
    if (raw == QLatin1String("initializing")
        || raw == QLatin1String("ready")
        || raw == QLatin1String("degraded")
        || raw == QLatin1String("dependency-failed")) {
        return raw;
    }
    if (success && code == QLatin1String("ok")) {
        return QStringLiteral("ready");
    }
    return QStringLiteral("dependency-failed");
}

QString normalizeErrorCode(const QString &raw)
{
    if (raw == QLatin1String("not-available")
        || raw == QLatin1String("timeout")
        || raw == QLatin1String("failed")
        || raw == QLatin1String("unsupported")) {
        return raw;
    }
    return QStringLiteral("failed");
}

bool metricsEqual(const QVariantMap &a, const QVariantMap &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (auto it = a.cbegin(); it != a.cend(); ++it) {
        auto otherIt = b.constFind(it.key());
        if (otherIt == b.cend()) {
            return false;
        }
        bool okA = false;
        bool okB = false;
        const double va = it.value().toDouble(&okA);
        const double vb = otherIt.value().toDouble(&okB);
        if (!okA || !okB || va != vb) {
            return false;
        }
    }
    return true;
}

QHash<QString, std::weak_ptr<MonitoringSharedClient>> &sharedRegistry()
{
    static QHash<QString, std::weak_ptr<MonitoringSharedClient>> registry;
    return registry;
}

QString makeRegistryKey(const QDBusConnection &bus, const BridgeOptions &options)
{
    return bus.name() + QLatin1Char('|') + options.serviceName + QLatin1Char('|')
           + options.objectPath + QLatin1Char('|') + options.interfaceName;
}

} // namespace

bool BridgeSnapshot::hasSameObservableState(const BridgeSnapshot &other) const
{
    return connected == other.connected
           && stateAvailable == other.stateAvailable
           && readiness == other.readiness
           && errorCode == other.errorCode
           && errorMessage == other.errorMessage
           && metricsEqual(metrics, other.metrics);
}

std::shared_ptr<MonitoringSharedClient> MonitoringSharedClient::acquireShared(
    const BridgeOptions &options)
{
    const QDBusConnection bus = QDBusConnection::systemBus();
    const QString key = makeRegistryKey(bus, options);
    auto &registry = sharedRegistry();

    if (auto existing = registry.value(key).lock()) {
        return existing;
    }

    auto created = std::shared_ptr<MonitoringSharedClient>(
        new MonitoringSharedClient(options, bus, key));
    registry.insert(key, created);
    return created;
}

std::shared_ptr<MonitoringSharedClient> MonitoringSharedClient::createIsolated(
    const BridgeOptions &options,
    const QDBusConnection &bus)
{
    return std::shared_ptr<MonitoringSharedClient>(
        new MonitoringSharedClient(options, bus, QString()));
}

MonitoringSharedClient::MonitoringSharedClient(const BridgeOptions &options,
                                               const QDBusConnection &bus,
                                               QString registryKey)
    : QObject(nullptr)
    , m_options(options)
    , m_bus(bus)
    , m_registryKey(std::move(registryKey))
    , m_canReconnectSystemBus(bus.name() == QLatin1String("qt_default_system_bus"))
    , m_currentRetryDelayMs(options.initialRetryDelayMs)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &MonitoringSharedClient::onRetryTimeout);

    m_safetySyncTimer.setSingleShot(false);
    if (m_options.safetySyncIntervalMs > 0) {
        m_safetySyncTimer.setInterval(m_options.safetySyncIntervalMs);
        connect(&m_safetySyncTimer, &QTimer::timeout,
                this, &MonitoringSharedClient::onSafetySyncTimeout);
    }

    initializeConnection();
}

MonitoringSharedClient::~MonitoringSharedClient()
{
    m_retryTimer.stop();
    m_safetySyncTimer.stop();
    detachDbusHandles();

    if (m_ownsBusConnection) {
        QDBusConnection::disconnectFromBus(m_bus.name());
        m_ownsBusConnection = false;
    }

    if (!m_registryKey.isEmpty()) {
        auto &registry = sharedRegistry();
        auto it = registry.find(m_registryKey);
        if (it != registry.end() && it.value().expired()) {
            registry.erase(it);
        }
    }
}

void MonitoringSharedClient::initializeConnection()
{
    if (!m_bus.isConnected()) {
        applyTransportFailure(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("System D-Bus connection is unavailable."),
            QStringLiteral("dependency-failed"));
        scheduleRetry();
        return;
    }

    ensureServiceWatcher();
    ensureSignalSubscription();

    if (!m_disconnectSignalSubscribed) {
        m_disconnectSignalSubscribed = m_bus.connect(
            QString(),
            QStringLiteral("/org/freedesktop/DBus/Local"),
            QStringLiteral("org.freedesktop.DBus.Local"),
            QStringLiteral("Disconnected"),
            this,
            SLOT(onBusDisconnected()));
    }

    if (m_options.safetySyncIntervalMs > 0 && !m_safetySyncTimer.isActive()) {
        m_safetySyncTimer.start();
    }

    // Asynchronous initial GetState synchronization (never blocks QML main thread).
    requestGetState();
}

void MonitoringSharedClient::detachDbusHandles()
{
    for (auto *watcher : findChildren<QDBusPendingCallWatcher *>()) {
        watcher->disconnect(this);
        watcher->deleteLater();
    }
    m_pendingGetState = nullptr;

    if (m_signalSubscribed && m_bus.isConnected()) {
        m_bus.disconnect(
            m_options.serviceName,
            m_options.objectPath,
            m_options.interfaceName,
            QStringLiteral("StateChanged"),
            this,
            SLOT(onStateChangedMessage(QDBusMessage)));
    }
    m_signalSubscribed = false;
    m_signalSubscriptionCount = 0;

    if (m_disconnectSignalSubscribed && m_bus.isConnected()) {
        m_bus.disconnect(
            QString(),
            QStringLiteral("/org/freedesktop/DBus/Local"),
            QStringLiteral("org.freedesktop.DBus.Local"),
            QStringLiteral("Disconnected"),
            this,
            SLOT(onBusDisconnected()));
    }
    m_disconnectSignalSubscribed = false;

    if (m_serviceWatcher) {
        m_serviceWatcher->disconnect(this);
        delete m_serviceWatcher;
        m_serviceWatcher = nullptr;
    }
}

void MonitoringSharedClient::ensureServiceWatcher()
{
    if (m_serviceWatcher) {
        return;
    }
    m_serviceWatcher = new QDBusServiceWatcher(
        m_options.serviceName,
        m_bus,
        QDBusServiceWatcher::WatchForOwnerChange,
        this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged,
            this, &MonitoringSharedClient::onServiceOwnerChanged);
}

void MonitoringSharedClient::ensureSignalSubscription()
{
    if (m_signalSubscribed || !m_bus.isConnected()) {
        return;
    }
    const bool ok = m_bus.connect(
        m_options.serviceName,
        m_options.objectPath,
        m_options.interfaceName,
        QStringLiteral("StateChanged"),
        this,
        SLOT(onStateChangedMessage(QDBusMessage)));
    if (ok) {
        m_signalSubscribed = true;
        m_signalSubscriptionCount = 1;
    }
}

void MonitoringSharedClient::requestGetState(const QString &expectedOwner)
{
    if (!m_bus.isConnected()) {
        applyTransportFailure(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("System D-Bus connection is unavailable."),
            QStringLiteral("dependency-failed"));
        scheduleRetry();
        return;
    }

    // Never keep multiple overlapping in-flight GetState requests; replace any
    // superseded pending watcher cleanly when a new sync is required.
    if (m_pendingGetState) {
        m_pendingGetState->disconnect(this);
        m_pendingGetState->deleteLater();
        m_pendingGetState = nullptr;
    }

    const quint64 requestEpoch = m_ownerEpoch;
    const quint64 sentSignalRevision = m_signalRevision;
    const QString targetOwner = expectedOwner.isEmpty() ? m_currentOwner : expectedOwner;

    const QDBusMessage call = QDBusMessage::createMethodCall(
        m_options.serviceName,
        m_options.objectPath,
        m_options.interfaceName,
        QStringLiteral("GetState"));

    ++m_totalGetStateRequests;
    auto *watcher = new QDBusPendingCallWatcher(
        m_bus.asyncCall(call, m_options.callTimeoutMs),
        this);
    m_pendingGetState = watcher;

    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, requestEpoch, sentSignalRevision, targetOwner]() {
                handleGetStateFinished(watcher, requestEpoch, sentSignalRevision, targetOwner);
            });
}

void MonitoringSharedClient::handleGetStateFinished(QDBusPendingCallWatcher *watcher,
                                                    quint64 requestEpoch,
                                                    quint64 sentSignalRevision,
                                                    const QString &expectedOwner)
{
    if (m_pendingGetState == watcher) {
        m_pendingGetState = nullptr;
    }

    const QDBusMessage reply = watcher->reply();
    const bool isError = watcher->isError();
    const QDBusError dbusError = watcher->error();
    watcher->deleteLater();

    // Guard 1: Service owner changed or service unregistered while GetState was in flight.
    if (requestEpoch != m_ownerEpoch) {
        ++m_discardedStaleReplies;
        return;
    }

    // Guard 2: Reply came from a different unique D-Bus owner than the active owner.
    const QString sender = reply.service();
    if (!m_currentOwner.isEmpty() && !sender.isEmpty() && sender != m_currentOwner) {
        ++m_discardedStaleReplies;
        return;
    }
    if (!expectedOwner.isEmpty() && !sender.isEmpty() && sender != expectedOwner) {
        ++m_discardedStaleReplies;
        return;
    }

    // Guard 3: A newer StateChanged signal already arrived for the same owner while
    // this GetState call was in flight -> do not overwrite fresher signal telemetry.
    if (m_signalRevision != sentSignalRevision) {
        ++m_discardedOutOfOrderReplies;
        return;
    }

    if (isError) {
        QString code = QStringLiteral("not-available");
        bool connected = false;
        const QString errName = dbusError.name();
        if (errName == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
            || errName == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")) {
            code = QStringLiteral("not-available");
            connected = false;
            m_ownerState = OwnerState::Absent;
            m_currentOwner.clear();
        } else {
            switch (dbusError.type()) {
            case QDBusError::NoReply:
            case QDBusError::TimedOut:
                code = QStringLiteral("timeout");
                connected = (m_ownerState == OwnerState::Owned);
                break;
            case QDBusError::ServiceUnknown:
            case QDBusError::Disconnected:
                code = QStringLiteral("not-available");
                connected = false;
                m_ownerState = OwnerState::Absent;
                m_currentOwner.clear();
                break;
            case QDBusError::UnknownMethod:
            case QDBusError::UnknownInterface:
            case QDBusError::UnknownObject:
                code = QStringLiteral("unsupported");
                connected = (m_ownerState == OwnerState::Owned);
                break;
            default:
                code = QStringLiteral("failed");
                connected = (m_ownerState == OwnerState::Owned);
                break;
            }
        }

        const QString msg = dbusError.message().isEmpty()
            ? QStringLiteral("Monitoring D-Bus GetState failed.")
            : dbusError.message();

        applyTransportFailure(connected, code, msg, QStringLiteral("dependency-failed"));
        scheduleRetry(code == QLatin1String("unsupported") ? m_options.incompatibleRetryDelayMs : -1);
        return;
    }

    if (reply.arguments().isEmpty()) {
        applyTransportFailure(
            true,
            QStringLiteral("failed"),
            QStringLiteral("Monitoring service returned an empty reply."),
            QStringLiteral("dependency-failed"));
        scheduleRetry();
        return;
    }

    const QVariant parsed = demarshalDbusVariant(reply.arguments().constFirst());
    if (parsed.typeId() != QMetaType::QVariantMap) {
        applyTransportFailure(
            true,
            QStringLiteral("failed"),
            QStringLiteral("Monitoring service returned malformed state envelope."),
            QStringLiteral("dependency-failed"));
        scheduleRetry();
        return;
    }

    applyEnvelope(parsed.toMap(), sender);
}

void MonitoringSharedClient::onServiceOwnerChanged(const QString &service,
                                                   const QString &oldOwner,
                                                   const QString &newOwner)
{
    if (service != m_options.serviceName) {
        return;
    }
    if (oldOwner == newOwner) {
        return;
    }

    ++m_ownerEpoch;
    m_currentOwner = newOwner;
    m_ownerState = newOwner.isEmpty() ? OwnerState::Absent : OwnerState::Owned;

    // Detach current pointer so the new owner's sync request can proceed immediately;
    // any delayed reply from the superseded owner will be dropped by Guard 1/2
    // (`requestEpoch != m_ownerEpoch` and `sender != m_currentOwner`) and deleted.
    m_pendingGetState = nullptr;

    if (newOwner.isEmpty()) {
        // Service stopped / unregistered -> immediately invalidate stale telemetry.
        applyTransportFailure(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("Monitoring service is unavailable."),
            QStringLiteral("dependency-failed"));
        scheduleRetry();
        return;
    }

    // Service appeared or restarted under a new unique owner:
    // Clear metrics from previous owner, ensure single subscription, and sync via GetState().
    BridgeSnapshot syncing;
    syncing.connected = true;
    syncing.stateAvailable = false;
    syncing.readiness = QStringLiteral("initializing");
    syncing.errorCode = QStringLiteral("not-available");
    syncing.errorMessage = QStringLiteral("Synchronizing monitoring telemetry.");
    syncing.metrics.clear();
    commitSnapshot(std::move(syncing), false);

    m_retryTimer.stop();
    resetBackoff();
    ensureSignalSubscription();
    requestGetState(newOwner);
}

void MonitoringSharedClient::onStateChangedMessage(const QDBusMessage &message)
{
    if (m_ownerState == OwnerState::Absent) {
        // Service currently has no active owner; ignore any stray/delayed signal.
        ++m_discardedStaleReplies;
        return;
    }

    const QString sender = message.service();
    if (!m_currentOwner.isEmpty() && !sender.isEmpty() && sender != m_currentOwner) {
        // Ignore delayed signal emitted by a previous owner after owner change.
        ++m_discardedStaleReplies;
        return;
    }
    if (m_ownerState == OwnerState::Unknown && !sender.isEmpty()) {
        m_ownerState = OwnerState::Owned;
        m_currentOwner = sender;
    }

    if (message.arguments().isEmpty()) {
        return;
    }

    const QVariant parsed = demarshalDbusVariant(message.arguments().constFirst());
    if (parsed.typeId() != QMetaType::QVariantMap) {
        return;
    }

    ++m_signalRevision;
    applyEnvelope(parsed.toMap(), sender);
}

void MonitoringSharedClient::onBusDisconnected()
{
    ++m_ownerEpoch;
    m_ownerState = OwnerState::Absent;
    m_currentOwner.clear();
    m_safetySyncTimer.stop();
    detachDbusHandles();

    applyTransportFailure(
        false,
        QStringLiteral("not-available"),
        QStringLiteral("System D-Bus connection was closed."),
        QStringLiteral("dependency-failed"));
    scheduleRetry();
}

void MonitoringSharedClient::onRetryTimeout()
{
    if (!m_bus.isConnected()) {
        if (m_canReconnectSystemBus) {
            if (m_ownsBusConnection) {
                QDBusConnection::disconnectFromBus(m_bus.name());
                m_ownsBusConnection = false;
            }
            const QString freshName = QStringLiteral("zethropol_monitoring_sys_%1_%2")
                                          .arg(reinterpret_cast<quintptr>(this), 0, 16)
                                          .arg(++m_busReconnectSeq);
            QDBusConnection fresh =
                QDBusConnection::connectToBus(QDBusConnection::SystemBus, freshName);
            if (fresh.isConnected()) {
                m_bus = fresh;
                m_ownsBusConnection = true;
            } else {
                QDBusConnection::disconnectFromBus(freshName);
            }
        }
        if (!m_bus.isConnected()) {
            scheduleRetry();
            return;
        }
        m_ownerState = OwnerState::Unknown;
        initializeConnection();
        return;
    }

    ensureServiceWatcher();
    ensureSignalSubscription();
    requestGetState();
}

void MonitoringSharedClient::onSafetySyncTimeout()
{
    if (!m_bus.isConnected()) {
        return;
    }
    if (m_pendingGetState) {
        return;
    }
    if (m_snapshot.connected) {
        requestGetState();
    }
}

void MonitoringSharedClient::applyEnvelope(const QVariantMap &envelope,
                                           const QString &senderUniqueName)
{
    if (m_ownerState != OwnerState::Owned && !senderUniqueName.isEmpty()) {
        m_ownerState = OwnerState::Owned;
        m_currentOwner = senderUniqueName;
    }

    const bool success = envelope.value(QStringLiteral("success")).toBool();
    const QString code = envelope.value(QStringLiteral("code")).toString();
    const QString message = envelope.value(QStringLiteral("message")).toString();
    const QString readiness = normalizeReadiness(
        envelope.value(QStringLiteral("readiness")).toString(),
        success,
        code);

    if (success && code == QLatin1String("ok")) {
        const QVariant rawDataVar = demarshalDbusVariant(envelope.value(QStringLiteral("data")));
        if (rawDataVar.typeId() != QMetaType::QVariantMap) {
            applyTransportFailure(
                true,
                QStringLiteral("failed"),
                QStringLiteral("Monitoring service returned malformed metrics map."),
                QStringLiteral("dependency-failed"));
            return;
        }

        const QVariantMap validated = validateMetrics(rawDataVar.toMap());
        if (validated.isEmpty()) {
            applyTransportFailure(
                true,
                QStringLiteral("failed"),
                QStringLiteral("Monitoring service returned no valid metrics."),
                QStringLiteral("dependency-failed"));
            return;
        }

        m_retryTimer.stop();
        resetBackoff();
        m_lastLoggedErrorKey.clear();

        BridgeSnapshot next;
        next.connected = true;
        next.stateAvailable = true;
        next.readiness = readiness;
        next.errorCode.clear();
        next.errorMessage.clear();
        next.metrics = validated;
        commitSnapshot(std::move(next), true);
        return;
    }

    const QString normalizedCode = normalizeErrorCode(code);
    const QString normalizedMessage = message.isEmpty()
        ? QStringLiteral("Monitoring telemetry is unavailable.")
        : message;

    BridgeSnapshot next;
    next.connected = true;
    next.stateAvailable = false;
    next.readiness = readiness;
    next.errorCode = normalizedCode;
    next.errorMessage = normalizedMessage;
    next.metrics.clear();

    logErrorDeduplicated(normalizedCode, normalizedMessage);
    commitSnapshot(std::move(next), false);

    // If Monitoring itself is alive and emitting StateChanged, avoid redundant polling;
    // only schedule slow safety retry when signal subscription failed.
    if (!m_signalSubscribed) {
        scheduleRetry(normalizedCode == QLatin1String("unsupported")
                          ? m_options.incompatibleRetryDelayMs
                          : -1);
    }
}

void MonitoringSharedClient::applyTransportFailure(bool connected,
                                                   const QString &errorCode,
                                                   const QString &errorMessage,
                                                   const QString &readiness)
{
    logErrorDeduplicated(errorCode, errorMessage);

    BridgeSnapshot next;
    next.connected = connected;
    next.stateAvailable = false;
    next.readiness = readiness;
    next.errorCode = errorCode;
    next.errorMessage = errorMessage;
    next.metrics.clear();
    commitSnapshot(std::move(next), false);
}

void MonitoringSharedClient::commitSnapshot(BridgeSnapshot next, bool updateTimestamp)
{
    if (m_snapshot.hasSameObservableState(next)) {
        // Avoid emitting redundant QML signals when observable telemetry/state
        // did not change (timestamp-only refresh).
        return;
    }

    const bool metricsChanged = !metricsEqual(m_snapshot.metrics, next.metrics);
    if (updateTimestamp && next.stateAvailable) {
        next.lastUpdate = QDateTime::currentDateTime();
    } else if (!next.stateAvailable) {
        next.lastUpdate = QDateTime();
    } else {
        next.lastUpdate = m_snapshot.lastUpdate;
    }

    m_snapshot = std::move(next);
    if (metricsChanged) {
        emit metricsUpdated();
    }
    emit stateUpdated();
}

void MonitoringSharedClient::scheduleRetry(int overrideDelayMs)
{
    const int delay = overrideDelayMs > 0 ? overrideDelayMs : m_currentRetryDelayMs;
    if (overrideDelayMs <= 0) {
        m_currentRetryDelayMs = qMin(m_currentRetryDelayMs * 2, m_options.maxRetryDelayMs);
    }
    if (!m_retryTimer.isActive() || m_retryTimer.remainingTime() > delay) {
        m_retryTimer.start(delay);
    }
}

void MonitoringSharedClient::resetBackoff()
{
    m_currentRetryDelayMs = m_options.initialRetryDelayMs;
}

void MonitoringSharedClient::logErrorDeduplicated(const QString &code, const QString &message)
{
    const QString key = code + QLatin1Char('|') + message;
    if (m_lastLoggedErrorKey != key) {
        m_lastLoggedErrorKey = key;
        qCWarning(lcMonitoringBridge) << "Monitoring bridge error" << code << ":" << message;
    }
}

QVariant MonitoringSharedClient::demarshalDbusVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>()) {
        return demarshalDbusVariant(value.value<QDBusVariant>().variant());
    }

    if (value.typeId() == QMetaType::QVariantMap) {
        const QVariantMap source = value.toMap();
        QVariantMap result;
        for (auto it = source.cbegin(); it != source.cend(); ++it) {
            result.insert(it.key(), demarshalDbusVariant(it.value()));
        }
        return result;
    }

    if (value.userType() != qMetaTypeId<QDBusArgument>()) {
        return value;
    }

    const auto argument = value.value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::MapType) {
        QVariantMap map;
        argument.beginMap();
        while (!argument.atEnd()) {
            argument.beginMapEntry();
            QString key;
            QVariant nestedValue;
            argument >> key >> nestedValue;
            map.insert(key, demarshalDbusVariant(nestedValue));
            argument.endMapEntry();
        }
        argument.endMap();
        return map;
    }

    if (argument.currentType() == QDBusArgument::ArrayType) {
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd()) {
            QVariant item;
            argument >> item;
            list.append(demarshalDbusVariant(item));
        }
        argument.endArray();
        return list;
    }

    return {};
}

QVariantMap MonitoringSharedClient::validateMetrics(const QVariantMap &rawData)
{
    QVariantMap validated;
    for (const MetricSpec &spec : kMetricSpecs) {
        const QString key = QString::fromLatin1(spec.key);
        auto it = rawData.constFind(key);
        if (it == rawData.cend()) {
            continue;
        }
        double num = 0.0;
        if (extractStrictNumericDouble(it.value(), &num) && isInMetricDomain(num, spec.kind)) {
            validated.insert(key, num);
        }
    }

    auto hasPair = [&validated](const QString &a, const QString &b, double *va, double *vb) {
        auto itA = validated.constFind(a);
        auto itB = validated.constFind(b);
        if (itA == validated.cend() || itB == validated.cend()) {
            return false;
        }
        *va = itA.value().toDouble();
        *vb = itB.value().toDouble();
        return true;
    };

    double used = 0.0;
    double total = 0.0;
    if (hasPair(QStringLiteral("memory_used_gb"), QStringLiteral("memory_total_gb"), &used, &total)
        && used > total) {
        validated.remove(QStringLiteral("memory_used_gb"));
        validated.remove(QStringLiteral("memory_total_gb"));
    }
    if (hasPair(QStringLiteral("gpu_vram_used_gb"), QStringLiteral("gpu_vram_total_gb"), &used, &total)
        && used > total) {
        validated.remove(QStringLiteral("gpu_vram_used_gb"));
        validated.remove(QStringLiteral("gpu_vram_total_gb"));
    }
    if (validated.contains(QStringLiteral("storage_filesystem_total_gb"))) {
        const double fsTotal = validated.value(QStringLiteral("storage_filesystem_total_gb")).toDouble();
        if (validated.contains(QStringLiteral("storage_filesystem_used_gb"))
            && validated.value(QStringLiteral("storage_filesystem_used_gb")).toDouble() > fsTotal) {
            validated.remove(QStringLiteral("storage_filesystem_used_gb"));
            validated.remove(QStringLiteral("storage_filesystem_total_gb"));
            validated.remove(QStringLiteral("storage_filesystem_usage_percent"));
        } else {
            if (validated.contains(QStringLiteral("storage_filesystem_free_gb"))
                && validated.value(QStringLiteral("storage_filesystem_free_gb")).toDouble() > fsTotal) {
                validated.remove(QStringLiteral("storage_filesystem_free_gb"));
            }
            if (validated.contains(QStringLiteral("storage_filesystem_available_gb"))
                && validated.value(QStringLiteral("storage_filesystem_available_gb")).toDouble() > fsTotal) {
                validated.remove(QStringLiteral("storage_filesystem_available_gb"));
            }
        }
    }

    return validated;
}

} // namespace zethropol::monitoring
