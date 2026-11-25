#include "MonitoringBridge.h"
#include "MonitoringSharedClient.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace zethropol::monitoring;

namespace {

QVariantMap full25MetricsMap(double cpuUsage = 18.5, bool includePing = false)
{
    QVariantMap m{
        {QStringLiteral("cpu_usage_percent"), cpuUsage},
        {QStringLiteral("cpu_temperature_c"), 43.2},
        {QStringLiteral("cpu_frequency_ghz"), 3.82},
        {QStringLiteral("gpu_usage_percent"), 0.0},
        {QStringLiteral("gpu_memory_usage_percent"), 3.0},
        {QStringLiteral("gpu_core_clock_mhz"), 1149.0},
        {QStringLiteral("gpu_vram_used_gb"), 0.42},
        {QStringLiteral("gpu_vram_total_gb"), 4.0},
        {QStringLiteral("gpu_temperature_c"), 29.0},
        {QStringLiteral("gpu_memory_clock_mhz"), 300.0},
        {QStringLiteral("memory_used_gb"), 5.12},
        {QStringLiteral("memory_total_gb"), 31.26},
        {QStringLiteral("storage_used_gb"), 255.84},
        {QStringLiteral("storage_capacity_gb"), 500.11},
        {QStringLiteral("storage_available_gb"), 237.79},
        {QStringLiteral("storage_temperature_c"), 33.0},
        {QStringLiteral("storage_filesystem_total_gb"), 495.81},
        {QStringLiteral("storage_filesystem_used_gb"), 255.84},
        {QStringLiteral("storage_filesystem_free_gb"), 239.98},
        {QStringLiteral("storage_filesystem_available_gb"), 237.79},
        {QStringLiteral("storage_filesystem_usage_percent"), 51.83},
        {QStringLiteral("storage_physical_capacity_gb"), 500.11},
        {QStringLiteral("network_download_mbps"), 0.0},
        {QStringLiteral("network_upload_mbps"), 1.25},
    };
    if (includePing) {
        m.insert(QStringLiteral("network_ping_ms"), 6.1);
    }
    return m;
}

QVariantMap makeEnvelope(bool success,
                         const QString &code,
                         const QString &message,
                         const QString &readiness,
                         const QVariantMap &data)
{
    return QVariantMap{
        {QStringLiteral("success"), success},
        {QStringLiteral("code"), code},
        {QStringLiteral("message"), message},
        {QStringLiteral("operationId"), QString()},
        {QStringLiteral("readiness"), readiness},
        {QStringLiteral("data"), data},
    };
}

} // namespace

class MockMonitoringDbusService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Monitoring.v1")

public:
    explicit MockMonitoringDbusService(QDBusConnection bus, QObject *parent = nullptr)
        : QObject(parent)
        , m_bus(std::move(bus))
    {}

    QVariantMap nextReply = makeEnvelope(
        true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap());
    int getStateCalls = 0;
    int delayReplyMs = 0;
    bool holdReplyManually = false;
    QDBusMessage heldRequest;
    bool hasHeldRequest = false;

    void sendHeldReply(const QVariantMap &envelope)
    {
        if (!hasHeldRequest) {
            return;
        }
        const QDBusMessage reply = heldRequest.createReply(QVariant::fromValue(envelope));
        m_bus.send(reply);
        hasHeldRequest = false;
    }

    void emitStateChanged(const QVariantMap &envelope)
    {
        QDBusMessage sig = QDBusMessage::createSignal(
            QStringLiteral("/org/zethropol/Monitoring"),
            QStringLiteral("org.zethropol.Monitoring.v1"),
            QStringLiteral("StateChanged"));
        sig << QVariant::fromValue(envelope);
        m_bus.send(sig);
    }

public slots:
    QVariantMap GetState()
    {
        ++getStateCalls;
        if (holdReplyManually) {
            setDelayedReply(true);
            heldRequest = message();
            hasHeldRequest = true;
            return {};
        }
        if (delayReplyMs > 0) {
            setDelayedReply(true);
            const QDBusMessage req = message();
            const QVariantMap payload = nextReply;
            const QDBusConnection busCopy = m_bus;
            QTimer::singleShot(delayReplyMs, this, [req, payload, busCopy]() mutable {
                busCopy.send(req.createReply(QVariant::fromValue(payload)));
            });
            return {};
        }
        return nextReply;
    }

private:
    QDBusConnection m_bus;
};

class MonitoringBridgeTest : public QObject
{
    Q_OBJECT

private:
    QDBusConnection openPeerBus(const QString &connName)
    {
        const QString addr = QString::fromUtf8(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"));
        return QDBusConnection::connectToBus(addr, connName);
    }

    BridgeOptions fastOptions()
    {
        BridgeOptions opts;
        opts.callTimeoutMs = 400;
        opts.initialRetryDelayMs = 60;
        opts.maxRetryDelayMs = 200;
        opts.incompatibleRetryDelayMs = 200;
        opts.safetySyncIntervalMs = 60000;
        return opts;
    }

private slots:
    void initTestCase()
    {
        // Must run strictly on isolated private D-Bus, never live host bus.
        QVERIFY(!qgetenv("DBUS_SYSTEM_BUS_ADDRESS").isEmpty());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"), qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(QDBusConnection::systemBus().isConnected());
    }

    // Scenarios 1, 2, 3, 4, 5, 6, 7, 8, 9:
    // Initial GetState, initializing, degraded -> ready, StateChanged updates, missing metrics,
    // real zero preservation, invalid types/NaN/Infinity filtering, failed envelope, staleness.
    void lifecycleInitializingDegradedReadySignalsValidationAndStaleness()
    {
        auto serverBus = openPeerBus(QStringLiteral("mock_srv_1"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringDbusService mock(serverBus);
        mock.nextReply = makeEnvelope(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("Monitoring telemetry is initializing."),
            QStringLiteral("initializing"),
            {});
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("mock_srv_1"));
        });

        auto client = MonitoringSharedClient::createIsolated(fastOptions());
        MonitoringBridge bridge(client);

        // 2. Initializing before first sample
        QTRY_VERIFY(bridge.connected());
        QVERIFY(!bridge.stateAvailable());
        QCOMPARE(bridge.readiness(), QStringLiteral("initializing"));
        QCOMPARE(bridge.errorCode(), QStringLiteral("not-available"));
        QVERIFY(bridge.metrics().isEmpty());
        QCOMPARE(bridge.lastUpdateMs(), 0);
        QCOMPARE(client->signalSubscriptionCount(), 1);

        // 3 & 4 & 5 & 6. StateChanged -> degraded (warming up, missing cpu_usage_percent, real 0.0 GPU)
        QVariantMap warming = full25MetricsMap(0.0, false);
        warming.remove(QStringLiteral("cpu_usage_percent"));
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("degraded"), warming));

        QTRY_VERIFY(bridge.stateAvailable());
        QCOMPARE(bridge.readiness(), QStringLiteral("degraded"));
        QVERIFY(!bridge.hasMetric(QStringLiteral("cpu_usage_percent")));
        QVERIFY(!bridge.metricValue(QStringLiteral("cpu_usage_percent")).isValid());
        QVERIFY(!bridge.hasMetric(QStringLiteral("network_ping_ms")));
        QVERIFY(bridge.hasMetric(QStringLiteral("gpu_usage_percent")));
        QCOMPARE(bridge.metricValue(QStringLiteral("gpu_usage_percent")).toDouble(), 0.0);
        QVERIFY(bridge.hasMetric(QStringLiteral("network_download_mbps")));
        QCOMPARE(bridge.metricValue(QStringLiteral("network_download_mbps")).toDouble(), 0.0);
        QVERIFY(bridge.lastUpdate().isValid());
        QVERIFY(bridge.lastUpdateMs() > 0);

        // 3 & 4. StateChanged -> ready (all 24 active metrics + 6 new Storage fields)
        mock.emitStateChanged(makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(24.5, false)));

        QTRY_COMPARE(bridge.readiness(), QStringLiteral("ready"));
        QVERIFY(bridge.hasMetric(QStringLiteral("cpu_usage_percent")));
        QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 24.5);
        QCOMPARE(bridge.metricValue(QStringLiteral("storage_filesystem_usage_percent")).toDouble(), 51.83);
        QCOMPARE(bridge.metricValue(QStringLiteral("storage_physical_capacity_gb")).toDouble(), 500.11);

        // Identical telemetry signal -> no redundant QML repaint signals
        QSignalSpy stateSpy(&bridge, &MonitoringBridge::stateChanged);
        QSignalSpy metricsSpy(&bridge, &MonitoringBridge::metricsChanged);
        mock.emitStateChanged(makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(24.5, false)));
        QTest::qWait(80);
        QCOMPARE(stateSpy.count(), 0);
        QCOMPARE(metricsSpy.count(), 0);

        // 7. Invalid types, NaN, +/-Infinity, out-of-range, and inconsistent capacities filtered while valid metrics survive
        QVariantMap mixed{
            {QStringLiteral("cpu_usage_percent"), std::numeric_limits<double>::quiet_NaN()},
            {QStringLiteral("cpu_temperature_c"), std::numeric_limits<double>::infinity()},
            {QStringLiteral("cpu_frequency_ghz"), -std::numeric_limits<double>::infinity()},
            {QStringLiteral("gpu_usage_percent"), 140.0},
            {QStringLiteral("gpu_core_clock_mhz"), QStringLiteral("1149 MHz")},
            {QStringLiteral("gpu_vram_used_gb"), 6.0},
            {QStringLiteral("gpu_vram_total_gb"), 4.0}, // inconsistent used > total
            {QStringLiteral("memory_used_gb"), true},
            {QStringLiteral("memory_total_gb"), 32.0},
            {QStringLiteral("storage_filesystem_usage_percent"), 0.0}, // real zero preserved
            {QStringLiteral("storage_capacity_gb"), 0.0}, // zero capacity rejected
            {QStringLiteral("storage_physical_capacity_gb"), -5.0},
        };
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("degraded"), mixed));
        QTRY_COMPARE(bridge.metrics().size(), 2);
        QVERIFY(bridge.hasMetric(QStringLiteral("memory_total_gb")));
        QCOMPARE(bridge.metricValue(QStringLiteral("memory_total_gb")).toDouble(), 32.0);
        QVERIFY(bridge.hasMetric(QStringLiteral("storage_filesystem_usage_percent")));
        QCOMPARE(bridge.metricValue(QStringLiteral("storage_filesystem_usage_percent")).toDouble(), 0.0);
        QVERIFY(!bridge.hasMetric(QStringLiteral("cpu_usage_percent")));
        QVERIFY(!bridge.hasMetric(QStringLiteral("gpu_usage_percent")));
        QVERIFY(!bridge.hasMetric(QStringLiteral("gpu_vram_used_gb")));
        QVERIFY(!bridge.hasMetric(QStringLiteral("gpu_vram_total_gb")));
        QVERIFY(!bridge.hasMetric(QStringLiteral("memory_used_gb")));
        QVERIFY(!bridge.hasMetric(QStringLiteral("storage_capacity_gb")));

        // 9. Proactive staleness notification clears metrics cleanly (no stale data kept)
        mock.emitStateChanged(makeEnvelope(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("Monitoring telemetry is stale; waiting for a fresh hardware sample."),
            QStringLiteral("dependency-failed"),
            {}));
        QTRY_VERIFY(!bridge.stateAvailable());
        QCOMPARE(bridge.readiness(), QStringLiteral("dependency-failed"));
        QCOMPARE(bridge.errorCode(), QStringLiteral("not-available"));
        QVERIFY(bridge.metrics().isEmpty());
        QCOMPARE(bridge.lastUpdateMs(), 0);

        // 8 & 16. Unsupported version error envelope
        mock.emitStateChanged(makeEnvelope(
            false,
            QStringLiteral("unsupported"),
            QStringLiteral("Incompatible zethropol-hardware version."),
            QStringLiteral("dependency-failed"),
            {}));
        QTRY_COMPARE(bridge.errorCode(), QStringLiteral("unsupported"));
        QVERIFY(!bridge.stateAvailable());
        QVERIFY(bridge.metrics().isEmpty());
    }

    // Scenarios 10, 11, 12, 13: Service unavailable, restart, owner change, and delayed reply
    // from previous owner MUST be discarded.
    void serviceUnavailableRestartOwnerChangeAndDelayedReplyRejection()
    {
        // 10. Start bridge while service is unavailable
        auto client = MonitoringSharedClient::createIsolated(fastOptions());
        MonitoringBridge bridge(client);
        QTRY_COMPARE(bridge.errorCode(), QStringLiteral("not-available"));
        QVERIFY(!bridge.connected());
        QVERIFY(!bridge.stateAvailable());
        QVERIFY(bridge.metrics().isEmpty());

        // Start Owner A (holds GetState manually to simulate delayed reply across owner swap)
        auto busA = openPeerBus(QStringLiteral("mock_owner_a"));
        QVERIFY(busA.isConnected());
        MockMonitoringDbusService mockA(busA);
        mockA.holdReplyManually = true;
        QVERIFY(busA.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mockA, QDBusConnection::ExportAllSlots));
        QVERIFY(busA.registerService(QStringLiteral("org.zethropol.Monitoring")));

        QTRY_VERIFY(mockA.hasHeldRequest);
        const quint64 epochA = client->ownerEpoch();

        // Owner A stops -> immediate invalidation & owner epoch increment
        busA.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
        QTRY_VERIFY(client->ownerEpoch() > epochA);
        QVERIFY(!bridge.connected());
        QVERIFY(!bridge.stateAvailable());
        QVERIFY(bridge.metrics().isEmpty());

        // Start Owner B with fresh cpu_usage_percent = 47.0
        auto busB = openPeerBus(QStringLiteral("mock_owner_b"));
        QVERIFY(busB.isConnected());
        MockMonitoringDbusService mockB(busB);
        mockB.nextReply = makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(47.0, false));
        QVERIFY(busB.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mockB, QDBusConnection::ExportAllSlots));
        QVERIFY(busB.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            busB.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            busB.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            busA.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("mock_owner_a"));
            QDBusConnection::disconnectFromBus(QStringLiteral("mock_owner_b"));
        });

        QTRY_VERIFY(bridge.stateAvailable());
        QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 47.0);

        // 13. Delayed reply & delayed signal from old Owner A arrives AFTER Owner B is active -> MUST be rejected!
        mockA.sendHeldReply(makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(99.9, false)));
        mockA.emitStateChanged(makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(88.8, false)));
        QTest::qWait(100);
        QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 47.0);
        QVERIFY(client->discardedStaleReplies() >= 1);
    }

    // Scenarios 14, 15, 18: GetState vs newer StateChanged ordering race, D-Bus timeout,
    // and main thread non-blocking check.
    void getStateVsStateChangedRaceTimeoutAndNonBlockingMainThread()
    {
        auto serverBus = openPeerBus(QStringLiteral("mock_srv_race"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringDbusService mock(serverBus);
        mock.holdReplyManually = true;
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("mock_srv_race"));
        });

        // 18. Creating bridge with slow/hung service must NOT block the main thread
        QElapsedTimer ctorTimer;
        ctorTimer.start();
        auto client = MonitoringSharedClient::createIsolated(fastOptions());
        MonitoringBridge bridge(client);
        QVERIFY2(ctorTimer.elapsed() < 50,
                 qPrintable(QStringLiteral("Bridge constructor blocked for %1 ms").arg(ctorTimer.elapsed())));

        QTRY_VERIFY(mock.hasHeldRequest);

        // 14. While initial GetState is still in flight, newer StateChanged arrives with cpu=33.0
        mock.emitStateChanged(makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(33.0, false)));
        QTRY_VERIFY(bridge.stateAvailable());
        QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 33.0);

        // Now older GetState finishes with stale cpu=11.0 -> MUST NOT overwrite 33.0!
        mock.sendHeldReply(makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(11.0, false)));
        QTest::qWait(80);
        QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 33.0);
        QVERIFY(client->discardedOutOfOrderReplies() >= 1);

        // 15. D-Bus timeout test with fresh client & held reply exceeding callTimeoutMs
        BridgeOptions timeoutOpts = fastOptions();
        timeoutOpts.callTimeoutMs = 120;
        timeoutOpts.initialRetryDelayMs = 2000;
        auto timeoutClient = MonitoringSharedClient::createIsolated(timeoutOpts);
        MonitoringBridge timeoutBridge(timeoutClient);
        QTRY_COMPARE_WITH_TIMEOUT(timeoutBridge.errorCode(), QStringLiteral("timeout"), 1500);
        QVERIFY(!timeoutBridge.stateAvailable());
        QVERIFY(timeoutBridge.metrics().isEmpty());
    }

    // Scenarios 17, 19: Shared client across multiple QML/C++ bridges prevents duplicate
    // subscriptions, and safe destruction cleans up completely without leaks.
    void sharedClientNoDuplicateSubscriptionsQmlPluginLoadAndSafeDestruction()
    {
        auto serverBus = openPeerBus(QStringLiteral("mock_srv_shared"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringDbusService mock(serverBus);
        mock.nextReply = makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(21.5, false));
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("mock_srv_shared"));
        });

        std::weak_ptr<MonitoringSharedClient> weakShared;
        {
            // Multiple C++ bridges share ONE MonitoringSharedClient -> 1 subscription, 1 GetState
            auto b1 = std::make_unique<MonitoringBridge>();
            auto b2 = std::make_unique<MonitoringBridge>();
            auto b3 = std::make_unique<MonitoringBridge>();
            QCOMPARE(b1->sharedClient().get(), b2->sharedClient().get());
            QCOMPARE(b2->sharedClient().get(), b3->sharedClient().get());
            weakShared = b1->sharedClient();

            QTRY_VERIFY(b1->stateAvailable());
            QVERIFY(b2->stateAvailable());
            QVERIFY(b3->stateAvailable());
            QCOMPARE(b1->sharedClient()->signalSubscriptionCount(), 1);
            QCOMPARE(mock.getStateCalls, 1);

            // Destroy one bridge while others continue receiving live StateChanged updates
            b2.reset();
            mock.emitStateChanged(makeEnvelope(
                true,
                QStringLiteral("ok"),
                QString(),
                QStringLiteral("ready"),
                full25MetricsMap(29.0, false)));
            QTRY_COMPARE(b1->metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 29.0);
            QCOMPARE(b3->metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 29.0);
        }
        // After all bridges are destroyed, the shared backend MUST be freed immediately
        QVERIFY(weakShared.expired());

        // Real QML plugin import test (`import org.zethropol.monitoring 1.0`)
        // BridgeConsumer.qml instantiates TWO MonitoringBridge elements; they must share
        // ONE plugin-local MonitoringSharedClient (exact +1 GetState call, not +2).
        const int callsBeforeQml = mock.getStateCalls;
        const QString bridgeImportDir = qEnvironmentVariable("ZETHROPOL_BRIDGE_QML_IMPORT_DIR").isEmpty()
            ? QStringLiteral(BRIDGE_QML_IMPORT_DIR)
            : qEnvironmentVariable("ZETHROPOL_BRIDGE_QML_IMPORT_DIR");
        QQmlEngine engine;
        engine.addImportPath(bridgeImportDir);
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(BRIDGE_TEST_QML_FILE)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> obj(component.create());
        QVERIFY(obj != nullptr);

        QTRY_VERIFY(obj->property("stateAvailable").toBool());
        QCOMPARE(obj->property("secondaryStateAvailable").toBool(), true);
        QCOMPARE(obj->property("readiness").toString(), QStringLiteral("ready"));
        QCOMPARE(obj->property("secondaryReadiness").toString(), QStringLiteral("ready"));
        QCOMPARE(obj->property("hasCpuUsage").toBool(), true);
        QCOMPARE(obj->property("cpuUsageValue").toDouble(), 21.5);
        QCOMPARE(obj->property("hasGpuUsage").toBool(), true);
        QCOMPARE(obj->property("gpuUsageValue").toDouble(), 0.0);
        QCOMPARE(obj->property("hasPing").toBool(), false);
        QCOMPARE(obj->property("hasFsUsagePercent").toBool(), true);
        QCOMPARE(obj->property("fsUsagePercentValue").toDouble(), 51.83);
        QCOMPARE(mock.getStateCalls, callsBeforeQml + 1);
        QVERIFY2(warnings.isEmpty(),
                 qPrintable(QStringLiteral("Unexpected QML warnings: %1").arg(warnings.count())));
    }

    // Dedicated bus-disconnect test: killing the D-Bus daemon transitions the bridge
    // cleanly to explicit unavailable state without leaking or retaining stale metrics.
    void busDisconnectedProducesExplicitUnavailableState()
    {
        QProcess ephemeralBus;
        ephemeralBus.setProgram(QStringLiteral("dbus-daemon"));
        ephemeralBus.setArguments({
            QStringLiteral("--session"),
            QStringLiteral("--nofork"),
            QStringLiteral("--nopidfile"),
            QStringLiteral("--print-address=1"),
        });
        ephemeralBus.start();
        QVERIFY(ephemeralBus.waitForStarted(3000));
        QVERIFY(ephemeralBus.waitForReadyRead(3000));
        const QString busAddr = QString::fromUtf8(ephemeralBus.readLine()).trimmed();
        QVERIFY(!busAddr.isEmpty());

        const auto stopBus = qScopeGuard([&] {
            QDBusConnection::disconnectFromBus(QStringLiteral("ephemeral_srv"));
            QDBusConnection::disconnectFromBus(QStringLiteral("ephemeral_cli"));
            if (ephemeralBus.state() != QProcess::NotRunning) {
                ephemeralBus.terminate();
                if (!ephemeralBus.waitForFinished(2000)) {
                    ephemeralBus.kill();
                    ephemeralBus.waitForFinished();
                }
            }
        });

        auto srvConn = QDBusConnection::connectToBus(busAddr, QStringLiteral("ephemeral_srv"));
        auto cliConn = QDBusConnection::connectToBus(busAddr, QStringLiteral("ephemeral_cli"));
        QVERIFY(srvConn.isConnected());
        QVERIFY(cliConn.isConnected());

        MockMonitoringDbusService mock(srvConn);
        mock.nextReply = makeEnvelope(
            true,
            QStringLiteral("ok"),
            QString(),
            QStringLiteral("ready"),
            full25MetricsMap(19.0, false));
        QVERIFY(srvConn.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(srvConn.registerService(QStringLiteral("org.zethropol.Monitoring")));

        auto client = MonitoringSharedClient::createIsolated(fastOptions(), cliConn);
        MonitoringBridge bridge(client);
        QTRY_VERIFY(bridge.stateAvailable());
        QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 19.0);

        // Terminate the D-Bus daemon abruptly -> bridge must detect disconnection cleanly
        ephemeralBus.terminate();
        QVERIFY(ephemeralBus.waitForFinished(3000));

        QTRY_VERIFY(!bridge.connected());
        QVERIFY(!bridge.stateAvailable());
        QCOMPARE(bridge.readiness(), QStringLiteral("dependency-failed"));
        QCOMPARE(bridge.errorCode(), QStringLiteral("not-available"));
        QVERIFY(bridge.metrics().isEmpty());
        QCOMPARE(bridge.lastUpdateMs(), 0);
    }

    // Scenario 20 + Real Release Binaries Integration:
    // Launches real new `hardware` + `monitoring` release binaries (when env vars provided)
    // and verifies live QML plugin + multiple bridges receive real telemetry, survive
    // live Monitoring daemon restart, and cause zero independent Hardware polling per client.
    void realReleaseServicesIntegratedPrivateBusTest()
    {
        const QString hwBin = qEnvironmentVariable("ZETHROPOL_HARDWARE_RELEASE_BIN");
        const QString monBin = qEnvironmentVariable("ZETHROPOL_MONITORING_RELEASE_BIN");
        if (hwBin.isEmpty() || monBin.isEmpty()) {
            QSKIP("Set ZETHROPOL_HARDWARE_RELEASE_BIN and ZETHROPOL_MONITORING_RELEASE_BIN to run real-binary E2E");
        }
        QVERIFY(QFile::exists(hwBin));
        QVERIFY(QFile::exists(monBin));

        QProcess hwProc;
        hwProc.setProgram(hwBin);
        hwProc.setArguments({QStringLiteral("--dbus-service")});
        hwProc.start();
        QVERIFY(hwProc.waitForStarted(3000));

        QProcess monProc;
        monProc.setProgram(monBin);
        monProc.start();
        QVERIFY(monProc.waitForStarted(3000));

        const auto stopDaemons = qScopeGuard([&] {
            if (monProc.state() != QProcess::NotRunning) {
                monProc.terminate();
                if (!monProc.waitForFinished(2000)) {
                    monProc.kill();
                    monProc.waitForFinished();
                }
            }
            if (hwProc.state() != QProcess::NotRunning) {
                hwProc.terminate();
                if (!hwProc.waitForFinished(2000)) {
                    hwProc.kill();
                    hwProc.waitForFinished();
                }
            }
        });

        QTRY_VERIFY_WITH_TIMEOUT(
            QDBusConnection::systemBus().interface()->isServiceRegistered(
                QStringLiteral("org.zethropol.Monitoring")).value(),
            5000);

        // Create 4 concurrent C++ bridge instances + 2 QML plugin consumers on the real binaries
        MonitoringBridge b1;
        MonitoringBridge b2;
        MonitoringBridge b3;
        MonitoringBridge b4;
        QCOMPARE(b1.sharedClient().get(), b4.sharedClient().get());
        QCOMPARE(b1.sharedClient()->signalSubscriptionCount(), 1);

        const QString bridgeImportDir = qEnvironmentVariable("ZETHROPOL_BRIDGE_QML_IMPORT_DIR").isEmpty()
            ? QStringLiteral(BRIDGE_QML_IMPORT_DIR)
            : qEnvironmentVariable("ZETHROPOL_BRIDGE_QML_IMPORT_DIR");
        QQmlEngine engine;
        engine.addImportPath(bridgeImportDir);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(BRIDGE_TEST_QML_FILE)));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> qmlObj(component.create());
        QVERIFY(qmlObj != nullptr);

        // Wait for Monitoring's 2nd sample so CPU delta warms up -> readiness == "ready"
        QTRY_COMPARE_WITH_TIMEOUT(b1.readiness(), QStringLiteral("ready"), 5000);
        QVERIFY(b1.connected());
        QVERIFY(b1.stateAvailable());
        QVERIFY(b1.hasMetric(QStringLiteral("cpu_usage_percent")));
        QVERIFY(b1.hasMetric(QStringLiteral("memory_used_gb")));
        QVERIFY(b1.hasMetric(QStringLiteral("memory_total_gb")));
        QVERIFY(b1.hasMetric(QStringLiteral("storage_filesystem_total_gb")));
        QVERIFY(b1.hasMetric(QStringLiteral("storage_filesystem_usage_percent")));
        QVERIFY(!b1.hasMetric(QStringLiteral("network_ping_ms")));

        // All 4 C++ bridges and both QML plugin consumers stay synchronized via shared backends
        QTRY_COMPARE_WITH_TIMEOUT(qmlObj->property("readiness").toString(), QStringLiteral("ready"), 3000);
        QCOMPARE(qmlObj->property("secondaryReadiness").toString(), QStringLiteral("ready"));
        QCOMPARE(b2.metrics(), b1.metrics());
        QCOMPARE(b3.metrics(), b1.metrics());
        QCOMPARE(b4.metrics(), b1.metrics());
        QCOMPARE(b1.sharedClient()->totalGetStateRequests(), 1);

        // Live restart of real Monitoring release binary -> immediate invalidation + clean recovery
        monProc.terminate();
        QVERIFY(monProc.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!b1.connected(), 3000);
        QVERIFY(!b1.stateAvailable());
        QVERIFY(b1.metrics().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!qmlObj->property("stateAvailable").toBool(), 3000);

        monProc.start();
        QVERIFY(monProc.waitForStarted(3000));
        QTRY_COMPARE_WITH_TIMEOUT(b1.readiness(), QStringLiteral("ready"), 6000);
        QTRY_COMPARE_WITH_TIMEOUT(qmlObj->property("readiness").toString(), QStringLiteral("ready"), 4000);
        QVERIFY(b1.stateAvailable());
        QVERIFY(b1.hasMetric(QStringLiteral("cpu_usage_percent")));
        QCOMPARE(b1.sharedClient()->signalSubscriptionCount(), 1);
    }

    // Stage 04.5 Görev 4 & 7:
    // Scenarios 7 (rapid owner swaps + in-flight watcher drainage), 10 (malformed D-Bus reply
    // envelope), 17 (StateChanged signal loss -> safety sync recovery), 19 (repeated error log
    // deduplication verified with message handler).
    void faultResilienceRapidOwnerSwapsMalformedRepliesSignalLossAndLogDedup()
    {
        // --- Scenario 10: Malformed D-Bus envelope clears previously cached metrics ---
        {
            auto serverBus = openPeerBus(QStringLiteral("mock_srv_malformed"));
            QVERIFY(serverBus.isConnected());
            MockMonitoringDbusService mock(serverBus);
            QVERIFY(serverBus.registerObject(
                QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
            QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
            const auto cleanup = qScopeGuard([&] {
                serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
                serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
                QDBusConnection::disconnectFromBus(QStringLiteral("mock_srv_malformed"));
            });

            BridgeOptions opts = fastOptions();
            opts.safetySyncIntervalMs = 150;
            auto client = MonitoringSharedClient::createIsolated(opts);
            MonitoringBridge bridge(client);
            QTRY_VERIFY(bridge.stateAvailable());
            QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 18.5);

            // Emit envelope with malformed `data` (string instead of map) -> must invalidate!
            QVariantMap malformed = makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), {});
            malformed.insert(QStringLiteral("data"), QStringLiteral("corrupted-not-a-map"));
            mock.emitStateChanged(malformed);
            QTRY_VERIFY(!bridge.stateAvailable());
            QCOMPARE(bridge.errorCode(), QStringLiteral("failed"));
            QVERIFY(bridge.metrics().isEmpty());

            // --- Scenario 17: Signal loss -> safety sync recovers fresh telemetry automatically ---
            // Update nextReply WITHOUT emitting StateChanged; safetySyncIntervalMs=150ms must sync it.
            mock.nextReply = makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(52.5, false));
            QTRY_COMPARE_WITH_TIMEOUT(
                bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 52.5, 1500);
            QVERIFY(bridge.stateAvailable());

            // Also verify safety sync recovers when service silently turns stale without delivering signal
            mock.nextReply = makeEnvelope(
                false,
                QStringLiteral("not-available"),
                QStringLiteral("Monitoring telemetry is stale; waiting for a fresh hardware sample."),
                QStringLiteral("dependency-failed"),
                {});
            QTRY_VERIFY_WITH_TIMEOUT(!bridge.stateAvailable(), 1500);
            QVERIFY(bridge.metrics().isEmpty());

            // --- Scenario 19: Repeated identical error produces exact 1 logged warning ---
            static std::atomic<int> s_loggedBridgeErrors{0};
            s_loggedBridgeErrors.store(0);
            const auto prevHandler = qInstallMessageHandler(
                [](QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
                    Q_UNUSED(type);
                    if (ctx.category && QString::fromLatin1(ctx.category) == QLatin1String("zethropol.monitoring.bridge")
                        && msg.contains(QLatin1String("repeated synthetic error"))) {
                        s_loggedBridgeErrors.fetch_add(1);
                    }
                });
            const auto restoreHandler = qScopeGuard([&] { qInstallMessageHandler(prevHandler); });

            const QVariantMap repeatedErr = makeEnvelope(
                false,
                QStringLiteral("failed"),
                QStringLiteral("repeated synthetic error"),
                QStringLiteral("dependency-failed"),
                {});
            for (int i = 0; i < 10; ++i) {
                mock.emitStateChanged(repeatedErr);
            }
            QTRY_COMPARE(bridge.errorCode(), QStringLiteral("failed"));
            QTest::qWait(80);
            QCOMPARE(s_loggedBridgeErrors.load(), 1);
        }

        // --- Scenario 7: Rapid consecutive owner swaps with held replies ---
        {
            BridgeOptions opts = fastOptions();
            opts.callTimeoutMs = 250;
            auto client = MonitoringSharedClient::createIsolated(opts);
            MonitoringBridge bridge(client);
            QTRY_COMPARE(bridge.errorCode(), QStringLiteral("not-available"));

            auto busA = openPeerBus(QStringLiteral("rapid_owner_a"));
            auto busB = openPeerBus(QStringLiteral("rapid_owner_b"));
            QVERIFY(busA.isConnected() && busB.isConnected());
            MockMonitoringDbusService mockA(busA);
            MockMonitoringDbusService mockB(busB);
            mockA.holdReplyManually = true;
            mockB.holdReplyManually = true;
            QVERIFY(busA.registerObject(QStringLiteral("/org/zethropol/Monitoring"), &mockA, QDBusConnection::ExportAllSlots));
            QVERIFY(busB.registerObject(QStringLiteral("/org/zethropol/Monitoring"), &mockB, QDBusConnection::ExportAllSlots));
            const auto cleanup = qScopeGuard([&] {
                busA.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
                busB.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
                busA.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
                busB.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
                QDBusConnection::disconnectFromBus(QStringLiteral("rapid_owner_a"));
                QDBusConnection::disconnectFromBus(QStringLiteral("rapid_owner_b"));
            });

            // Alternate ownership rapidly 10 times
            for (int i = 0; i < 5; ++i) {
                QVERIFY(busA.registerService(QStringLiteral("org.zethropol.Monitoring")));
                QCoreApplication::processEvents();
                busA.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
                QCoreApplication::processEvents();
                QVERIFY(busB.registerService(QStringLiteral("org.zethropol.Monitoring")));
                QCoreApplication::processEvents();
                busB.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
                QCoreApplication::processEvents();
            }

            // Final stable owner B responds with cpu=66.5
            mockB.holdReplyManually = false;
            mockB.nextReply = makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(66.5, false));
            QVERIFY(busB.registerService(QStringLiteral("org.zethropol.Monitoring")));
            QTRY_COMPARE_WITH_TIMEOUT(
                bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 66.5, 2000);

            // Send stale held reply from old owner A if captured -> MUST NOT overwrite 66.5
            if (mockA.hasHeldRequest) {
                mockA.sendHeldReply(makeEnvelope(
                    true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(99.0, false)));
            }
            // Wait for all superseded in-flight watchers to finish/timeout and drain cleanly to 0
            QTRY_COMPARE_WITH_TIMEOUT(client->inFlightWatcherCount(), 0, 1500);
            QCOMPARE(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 66.5);
            QCOMPARE(client->signalSubscriptionCount(), 1);
        }
    }

    // Stage 04.5 Görev 5 & Scenario 22:
    // Repeated create/destroy churn (100 cycles), staggered multi-client join/leave,
    // destruction while a GetState call is actively held in flight, and FD count stability.
    void lifecycleChurnPendingCallDestructionAndMultiClientJoinLeave()
    {
        auto serverBus = openPeerBus(QStringLiteral("mock_srv_churn"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringDbusService mock(serverBus);
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("mock_srv_churn"));
        });

        auto countOpenFds = []() -> int {
            return QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
        };

        // Warm up QtDBus baseline once before measuring FD stability
        {
            MonitoringBridge warmup;
            QTRY_VERIFY(warmup.stateAvailable());
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        const int fdsBefore = countOpenFds();

        // 1. Destroying the last MonitoringBridge while GetState is actively held in flight
        mock.holdReplyManually = true;
        std::weak_ptr<MonitoringSharedClient> weakInFlight;
        {
            auto b = std::make_unique<MonitoringBridge>();
            weakInFlight = b->sharedClient();
            QTRY_VERIFY(mock.hasHeldRequest);
            QVERIFY(b->sharedClient()->inFlightWatcherCount() >= 1);
        }
        QVERIFY(weakInFlight.expired());
        // Delayed reply arrives AFTER client & bridge destruction -> must NOT crash or UAF
        mock.sendHeldReply(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(12.3, false)));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        QTest::qWait(50);

        // 2. 100 create/destroy churn cycles + staggered 10-client waves (Scenario 22)
        mock.holdReplyManually = false;
        for (int wave = 0; wave < 10; ++wave) {
            std::vector<std::unique_ptr<MonitoringBridge>> bridges;
            std::weak_ptr<MonitoringSharedClient> waveShared;
            for (int j = 0; j < 10; ++j) {
                bridges.push_back(std::make_unique<MonitoringBridge>());
                if (j == 0) {
                    waveShared = bridges.back()->sharedClient();
                } else {
                    QCOMPARE(bridges.back()->sharedClient().get(), bridges.front()->sharedClient().get());
                }
            }
            QTRY_VERIFY(bridges.front()->stateAvailable());
            QCOMPARE(bridges.front()->sharedClient()->signalSubscriptionCount(), 1);
            // Leave staggered: destroy half, verify remaining half stay live, then destroy rest
            bridges.resize(5);
            mock.emitStateChanged(makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(20.0 + wave, false)));
            QTRY_COMPARE(bridges.back()->metricValue(QStringLiteral("cpu_usage_percent")).toDouble(), 20.0 + wave);
            bridges.clear();
            QVERIFY(waveShared.expired());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
        }

        const int fdsAfter = countOpenFds();
        QVERIFY2(fdsAfter <= fdsBefore + 1,
                 qPrintable(QStringLiteral("FD leak detected after 100 bridge churn cycles: before=%1 after=%2")
                                .arg(fdsBefore).arg(fdsAfter)));
    }

    // Stage 04.5 Scenario 21 regression test:
    // Killing and restarting the private D-Bus daemon on the same unix socket path must
    // transition MonitoringBridge immediately to unavailable (clearing stale data) and then
    // automatically reconnect via a fresh private system-bus connection once the bus returns.
    void privateBusRestartAutoRecovery()
    {
        if (qEnvironmentVariableIsSet("ZETHROPOL_BRIDGE_S21_CHILD")) {
            MonitoringBridge bridge;
            QTRY_VERIFY_WITH_TIMEOUT(
                bridge.connected() && bridge.stateAvailable()
                    && bridge.readiness() == QStringLiteral("ready")
                    && std::abs(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble() - 31.4) < 0.01,
                5000);
            fprintf(stdout, "S21_READY1\n");
            fflush(stdout);

            QTRY_VERIFY_WITH_TIMEOUT(
                !bridge.connected() && !bridge.stateAvailable() && bridge.metrics().isEmpty(),
                5000);
            fprintf(stdout, "S21_DOWN\n");
            fflush(stdout);

            QTRY_VERIFY_WITH_TIMEOUT(
                bridge.connected() && bridge.stateAvailable()
                    && bridge.readiness() == QStringLiteral("ready")
                    && std::abs(bridge.metricValue(QStringLiteral("cpu_usage_percent")).toDouble() - 63.2) < 0.01,
                10000);
            fprintf(stdout, "S21_READY2\n");
            fflush(stdout);
            return;
        }

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString sockPath = tempDir.filePath(QStringLiteral("bus.sock"));
        const QString busAddr = QStringLiteral("unix:path=%1").arg(sockPath);
        const QString confPath = tempDir.filePath(QStringLiteral("bus.conf"));
        {
            QFile f(confPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            const QByteArray conf = QStringLiteral(
                "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                "<busconfig>\n"
                "  <type>session</type>\n"
                "  <listen>%1</listen>\n"
                "  <auth>EXTERNAL</auth>\n"
                "  <policy context=\"default\">\n"
                "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                "    <allow eavesdrop=\"true\"/>\n"
                "    <allow own=\"*\"/>\n"
                "  </policy>\n"
                "</busconfig>\n").arg(busAddr).toUtf8();
            QCOMPARE(f.write(conf), conf.size());
        }

        auto startBus = [&](QProcess &proc) -> bool {
            QFile::remove(sockPath);
            proc.setProgram(QStringLiteral("dbus-daemon"));
            proc.setArguments({QStringLiteral("--config-file=%1").arg(confPath), QStringLiteral("--nofork")});
            proc.start();
            if (!proc.waitForStarted(3000)) {
                return false;
            }
            QElapsedTimer t;
            t.start();
            while (!QFile::exists(sockPath) && t.elapsed() < 3000) {
                QTest::qWait(20);
            }
            return QFile::exists(sockPath);
        };

        QProcess bus1;
        QProcess bus2;
        QProcess child;
        const auto cleanup = qScopeGuard([&] {
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                child.waitForFinished(2000);
            }
            QDBusConnection::disconnectFromBus(QStringLiteral("s21_srv_1"));
            QDBusConnection::disconnectFromBus(QStringLiteral("s21_srv_2"));
            if (bus1.state() != QProcess::NotRunning) {
                bus1.kill();
                bus1.waitForFinished(2000);
            }
            if (bus2.state() != QProcess::NotRunning) {
                bus2.kill();
                bus2.waitForFinished(2000);
            }
        });

        QVERIFY(startBus(bus1));
        QDBusConnection srv1 = QDBusConnection::connectToBus(busAddr, QStringLiteral("s21_srv_1"));
        QVERIFY(srv1.isConnected());
        MockMonitoringDbusService mock1(srv1);
        mock1.nextReply = makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(31.4, false));
        QVERIFY(srv1.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock1, QDBusConnection::ExportAllSlots));
        QVERIFY(srv1.registerService(QStringLiteral("org.zethropol.Monitoring")));

        QProcessEnvironment childEnv = QProcessEnvironment::systemEnvironment();
        childEnv.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), busAddr);
        childEnv.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), busAddr);
        childEnv.insert(QStringLiteral("ZETHROPOL_BRIDGE_S21_CHILD"), QStringLiteral("1"));
        child.setProcessEnvironment(childEnv);
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({QStringLiteral("privateBusRestartAutoRecovery")});
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start();
        QVERIFY(child.waitForStarted(3000));

        QByteArray childOut;
        auto waitChildMarker = [&](const QByteArray &marker, int timeoutMs) -> bool {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < timeoutMs) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                child.waitForReadyRead(20);
                childOut += child.readAll();
                if (childOut.contains(marker)) {
                    return true;
                }
            }
            return false;
        };

        QVERIFY2(waitChildMarker("S21_READY1", 6000), childOut.constData());

        // Kill bus1 abruptly (simulating D-Bus daemon restart)
        QDBusConnection::disconnectFromBus(QStringLiteral("s21_srv_1"));
        bus1.kill();
        QVERIFY(bus1.waitForFinished(3000));

        QVERIFY2(waitChildMarker("S21_DOWN", 6000), childOut.constData());

        // Start bus2 on the EXACT SAME unix socket path and serve fresh metrics (63.2%)
        QVERIFY(startBus(bus2));
        QDBusConnection srv2 = QDBusConnection::connectToBus(busAddr, QStringLiteral("s21_srv_2"));
        QVERIFY(srv2.isConnected());
        MockMonitoringDbusService mock2(srv2);
        mock2.nextReply = makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), full25MetricsMap(63.2, false));
        QVERIFY(srv2.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock2, QDBusConnection::ExportAllSlots));
        QVERIFY(srv2.registerService(QStringLiteral("org.zethropol.Monitoring")));

        QVERIFY2(waitChildMarker("S21_READY2", 10000), childOut.constData());
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitCode(), 0);
    }
};

QTEST_MAIN(MonitoringBridgeTest)
#include "MonitoringBridgeTest.moc"
