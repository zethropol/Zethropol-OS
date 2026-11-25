#pragma once

#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <memory>

namespace zethropol::monitoring {

struct BridgeOptions {
    QString serviceName = QStringLiteral("org.zethropol.Monitoring");
    QString objectPath = QStringLiteral("/org/zethropol/Monitoring");
    QString interfaceName = QStringLiteral("org.zethropol.Monitoring.v1");
    int callTimeoutMs = 3000;
    int initialRetryDelayMs = 1000;
    int maxRetryDelayMs = 30000;
    int incompatibleRetryDelayMs = 30000;
    // Rare safety sync interval (NOT a 1 Hz polling loop).
    int safetySyncIntervalMs = 30000;
};

struct BridgeSnapshot {
    bool connected = false;
    bool stateAvailable = false;
    QString readiness = QStringLiteral("initializing");
    QString errorCode = QStringLiteral("not-available");
    QString errorMessage = QStringLiteral("Monitoring telemetry is initializing.");
    QVariantMap metrics;
    QDateTime lastUpdate;

    bool hasSameObservableState(const BridgeSnapshot &other) const;
};

/// Process-local shared asynchronous D-Bus backend for `org.zethropol.Monitoring.v1`.
///
/// Multiple `MonitoringBridge` QML instances share a single `MonitoringSharedClient`
/// via `std::shared_ptr` / `std::weak_ptr` registry, guaranteeing:
/// - At most one `QDBusServiceWatcher` and one `StateChanged` signal subscription.
/// - Zero synchronous/blocking D-Bus calls on the Qt/QML main thread.
/// - Epoch + sender unique-name + signal-revision guards against delayed replies
///   and `GetState` / `StateChanged` ordering races.
/// - Automatic cleanup when the last `MonitoringBridge` is destroyed.
class MonitoringSharedClient : public QObject
{
    Q_OBJECT

public:
    static std::shared_ptr<MonitoringSharedClient> acquireShared(
        const BridgeOptions &options = BridgeOptions{});

    static std::shared_ptr<MonitoringSharedClient> createIsolated(
        const BridgeOptions &options,
        const QDBusConnection &bus = QDBusConnection::systemBus());

    ~MonitoringSharedClient() override;

    const BridgeSnapshot &snapshot() const { return m_snapshot; }
    const BridgeOptions &options() const { return m_options; }

    // Read-only diagnostic counters (for automated lifecycle/dedup verification).
    int signalSubscriptionCount() const { return m_signalSubscriptionCount; }
    int activeRequestCount() const { return m_pendingGetState ? 1 : 0; }
    int inFlightWatcherCount() const { return findChildren<QDBusPendingCallWatcher *>().size(); }
    int totalGetStateRequests() const { return m_totalGetStateRequests; }
    int discardedStaleReplies() const { return m_discardedStaleReplies; }
    int discardedOutOfOrderReplies() const { return m_discardedOutOfOrderReplies; }
    QString currentOwner() const { return m_currentOwner; }
    quint64 ownerEpoch() const { return m_ownerEpoch; }
    quint64 signalRevision() const { return m_signalRevision; }

    // Pure parsing/validation helpers exposed for unit testing.
    static QVariant demarshalDbusVariant(const QVariant &value);
    static QVariantMap validateMetrics(const QVariantMap &rawData);

signals:
    void stateUpdated();
    void metricsUpdated();

private slots:
    void onServiceOwnerChanged(const QString &service,
                               const QString &oldOwner,
                               const QString &newOwner);
    void onStateChangedMessage(const QDBusMessage &message);
    void onBusDisconnected();
    void onRetryTimeout();
    void onSafetySyncTimeout();

private:
    explicit MonitoringSharedClient(const BridgeOptions &options,
                                    const QDBusConnection &bus,
                                    QString registryKey);

    void initializeConnection();
    void detachDbusHandles();
    void ensureServiceWatcher();
    void ensureSignalSubscription();
    void requestGetState(const QString &expectedOwner = QString());
    void handleGetStateFinished(QDBusPendingCallWatcher *watcher,
                                quint64 requestEpoch,
                                quint64 sentSignalRevision,
                                const QString &expectedOwner);
    void applyEnvelope(const QVariantMap &envelope, const QString &senderUniqueName);
    void applyTransportFailure(bool connected,
                               const QString &errorCode,
                               const QString &errorMessage,
                               const QString &readiness);
    void commitSnapshot(BridgeSnapshot next, bool updateTimestamp);
    void scheduleRetry(int overrideDelayMs = -1);
    void resetBackoff();
    void logErrorDeduplicated(const QString &code, const QString &message);

    BridgeOptions m_options;
    QDBusConnection m_bus;
    QString m_registryKey;
    BridgeSnapshot m_snapshot;

    QPointer<QDBusServiceWatcher> m_serviceWatcher;
    QPointer<QDBusPendingCallWatcher> m_pendingGetState;
    QTimer m_retryTimer;
    QTimer m_safetySyncTimer;

    bool m_canReconnectSystemBus = false;
    bool m_ownsBusConnection = false;
    quint64 m_busReconnectSeq = 0;
    bool m_signalSubscribed = false;
    bool m_disconnectSignalSubscribed = false;
    int m_signalSubscriptionCount = 0;
    int m_totalGetStateRequests = 0;
    int m_discardedStaleReplies = 0;
    int m_discardedOutOfOrderReplies = 0;
    int m_currentRetryDelayMs = 1000;

    enum class OwnerState {
        Unknown,
        Absent,
        Owned,
    };

    OwnerState m_ownerState = OwnerState::Unknown;
    QString m_currentOwner;
    quint64 m_ownerEpoch = 0;
    quint64 m_signalRevision = 0;
    QString m_lastLoggedErrorKey;
};

} // namespace zethropol::monitoring
