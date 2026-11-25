#include <QColor>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtQml>
#include <QtTest>

#include <cmath>
#include <limits>
#include <memory>

namespace {

// Minimal test-only host shims for Plasma 6 attached types so the real, unmodified
// `services/system-monitor/contents/ui/main.qml` can be rendered in a headless offscreen
// QQuickWindow and tested with reactive `Plasmoid.configuration.backgroundOpacity` changes
// while dynamically importing the real built `org.zethropol.monitoring 1.0` QML plugin.

static QString pathOverrideOrDefault(const char *envName, const QString &fallback)
{
    const QString overridePath = qEnvironmentVariable(envName);
    return overridePath.isEmpty() ? fallback : overridePath;
}

class MockPlasmaTypes : public QObject
{
    Q_OBJECT
public:
    enum BackgroundHints {
        NoBackground = 0,
        StandardBackground = 1,
    };
    Q_ENUM(BackgroundHints)
};

class MockPlasmoidConfig : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int backgroundOpacity READ backgroundOpacity WRITE setBackgroundOpacity NOTIFY backgroundOpacityChanged)

public:
    explicit MockPlasmoidConfig(QObject *parent = nullptr)
        : QObject(parent)
    {}

    int backgroundOpacity() const { return m_backgroundOpacity; }
    void setBackgroundOpacity(int value)
    {
        if (m_backgroundOpacity == value) {
            return;
        }
        m_backgroundOpacity = value;
        emit backgroundOpacityChanged();
    }

signals:
    void backgroundOpacityChanged();

private:
    int m_backgroundOpacity = 60; // Exact default from contents/config/main.xml
};

class MockPlasmoidAttached : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(int backgroundHints READ backgroundHints WRITE setBackgroundHints NOTIFY backgroundHintsChanged)
    Q_PROPERTY(QObject *configuration READ configuration CONSTANT)

public:
    explicit MockPlasmoidAttached(QObject *parent = nullptr)
        : QObject(parent)
        , m_config(new MockPlasmoidConfig(this))
    {}

    QString title() const { return m_title; }
    void setTitle(const QString &t)
    {
        if (m_title != t) {
            m_title = t;
            emit titleChanged();
        }
    }

    int backgroundHints() const { return m_backgroundHints; }
    void setBackgroundHints(int h)
    {
        if (m_backgroundHints != h) {
            m_backgroundHints = h;
            emit backgroundHintsChanged();
        }
    }

    MockPlasmoidConfig *configObject() const { return m_config; }
    QObject *configuration() const { return m_config; }

signals:
    void titleChanged();
    void backgroundHintsChanged();

private:
    QString m_title;
    int m_backgroundHints = MockPlasmaTypes::StandardBackground;
    MockPlasmoidConfig *m_config = nullptr;
};

class MockPlasmoid : public QObject
{
    Q_OBJECT
public:
    static MockPlasmoidAttached *qmlAttachedProperties(QObject *object)
    {
        return new MockPlasmoidAttached(object);
    }
};

class MockPlasmoidItem : public QQuickItem
{
    Q_OBJECT
public:
    explicit MockPlasmoidItem(QQuickItem *parent = nullptr)
        : QQuickItem(parent)
    {}
};

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

QVariantMap sampleFullMetrics()
{
    // Intentionally selects values where:
    // - storage_filesystem_usage_percent (51.83%) differs from 255.84 / 495.81 * 100 (51.60%)
    //   to prove Btrfs filesystem usage % is taken directly and never recalculated.
    // - storage_physical_capacity_gb (1000.0) & storage_capacity_gb (1000.0) differ from
    //   storage_filesystem_total_gb (495.81) to prove physical capacity is never used.
    return QVariantMap{
        {QStringLiteral("cpu_usage_percent"), 24.5},
        {QStringLiteral("cpu_temperature_c"), 46.4},
        {QStringLiteral("cpu_frequency_ghz"), 3.825},
        {QStringLiteral("gpu_usage_percent"), 37.2},
        {QStringLiteral("gpu_memory_usage_percent"), 15.0},
        {QStringLiteral("gpu_core_clock_mhz"), 1149.0},
        {QStringLiteral("gpu_vram_used_gb"), 0.42},
        {QStringLiteral("gpu_vram_total_gb"), 4.0},
        {QStringLiteral("gpu_temperature_c"), 31.2},
        {QStringLiteral("gpu_memory_clock_mhz"), 300.0},
        {QStringLiteral("memory_used_gb"), 8.0},
        {QStringLiteral("memory_total_gb"), 32.0},
        {QStringLiteral("storage_used_gb"), 255.84},
        {QStringLiteral("storage_capacity_gb"), 1000.0},
        {QStringLiteral("storage_available_gb"), 237.79},
        {QStringLiteral("storage_temperature_c"), 33.4},
        {QStringLiteral("storage_filesystem_total_gb"), 495.81},
        {QStringLiteral("storage_filesystem_used_gb"), 255.84},
        {QStringLiteral("storage_filesystem_free_gb"), 239.98},
        {QStringLiteral("storage_filesystem_available_gb"), 237.79},
        {QStringLiteral("storage_filesystem_usage_percent"), 51.83},
        {QStringLiteral("storage_physical_capacity_gb"), 1000.0},
        {QStringLiteral("network_download_mbps"), 12.34},
        {QStringLiteral("network_upload_mbps"), 5.67},
        // network_ping_ms intentionally omitted by default (no external ICMP ping)
    };
}

QString labelText(QObject *root, const char *objectName)
{
    QObject *child = root->findChild<QObject *>(QString::fromLatin1(objectName));
    if (!child) {
        return QStringLiteral("<MISSING:%1>").arg(QString::fromLatin1(objectName));
    }
    return child->property("text").toString();
}

QObject *findChildStrict(QObject *root, const char *objectName)
{
    return root->findChild<QObject *>(QString::fromLatin1(objectName));
}

bool anyLabelContainsInvalidToken(QObject *root)
{
    const auto labels = root->findChildren<QObject *>();
    for (QObject *obj : labels) {
        const QVariant textVar = obj->property("text");
        if (!textVar.isValid() || textVar.typeId() != QMetaType::QString) {
            continue;
        }
        const QString txt = textVar.toString();
        if (txt.contains(QStringLiteral("NaN"), Qt::CaseInsensitive)
            || txt.contains(QStringLiteral("Infinity"), Qt::CaseInsensitive)
            || txt.contains(QStringLiteral("undefined"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

} // namespace

QML_DECLARE_TYPEINFO(MockPlasmoid, QML_HAS_ATTACHED_PROPERTIES)

class MockHardwareTelemetryService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Hardware.v1")

public:
    explicit MockHardwareTelemetryService(QObject *parent = nullptr)
        : QObject(parent)
    {}

    QVariantMap nextEnvelope;
    int getTelemetryCalls = 0;

public slots:
    QVariantMap GetTelemetry()
    {
        ++getTelemetryCalls;
        return nextEnvelope;
    }
};

class MonitoringSignalCapture : public QObject
{
    Q_OBJECT
public:
    explicit MonitoringSignalCapture(QObject *parent = nullptr)
        : QObject(parent)
    {}

    QVariantList receivedEnvelopes;

public slots:
    void onStateChanged(const QDBusMessage &msg)
    {
        if (!msg.arguments().isEmpty()) {
            receivedEnvelopes.append(msg.arguments().constFirst());
        }
    }
};

static QVariant unwrapDbusArg(const QVariant &v)
{
    if (v.userType() == qMetaTypeId<QDBusVariant>()) {
        return unwrapDbusArg(v.value<QDBusVariant>().variant());
    }
    if (v.typeId() == QMetaType::QVariantMap) {
        QVariantMap out;
        const QVariantMap m = v.toMap();
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
            out.insert(it.key(), unwrapDbusArg(it.value()));
        }
        return out;
    }
    if (v.userType() == qMetaTypeId<QDBusArgument>()) {
        const auto arg = v.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::MapType) {
            QVariantMap out;
            arg.beginMap();
            while (!arg.atEnd()) {
                arg.beginMapEntry();
                QString k;
                QVariant val;
                arg >> k >> val;
                out.insert(k, unwrapDbusArg(val));
                arg.endMapEntry();
            }
            arg.endMap();
            return out;
        }
    }
    return v;
}

class MockMonitoringService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Monitoring.v1")

public:
    explicit MockMonitoringService(QDBusConnection bus, QObject *parent = nullptr)
        : QObject(parent)
        , m_bus(std::move(bus))
    {}

    QVariantMap nextReply = makeEnvelope(
        true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), sampleFullMetrics());
    int getStateCalls = 0;

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
        return nextReply;
    }

private:
    QDBusConnection m_bus;
};

struct LoadedMonitorHost {
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QQuickWindow> window;
    QQuickItem *rootItem = nullptr;
    MockPlasmoidAttached *plasmoidAttached = nullptr;
    QList<QQmlError> warnings;

    ~LoadedMonitorHost()
    {
        if (rootItem) {
            rootItem->setParentItem(nullptr);
            delete rootItem;
            rootItem = nullptr;
        }
        window.reset();
        engine.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
};

static std::unique_ptr<LoadedMonitorHost> loadMainQmlInWindow()
{
    auto host = std::make_unique<LoadedMonitorHost>();
    host->engine = std::make_unique<QQmlEngine>();
    const QString qmlImportDir = pathOverrideOrDefault(
        "ZETHROPOL_SYSTEM_MONITOR_QML_IMPORT_DIR", QStringLiteral(SYSTEM_MONITOR_QML_IMPORT_DIR));
    const QString mainQmlPath = pathOverrideOrDefault(
        "ZETHROPOL_SYSTEM_MONITOR_MAIN_QML", QStringLiteral(SYSTEM_MONITOR_MAIN_QML));

    host->engine->addImportPath(qmlImportDir);
    host->engine->addImportPath(QStringLiteral(SYSTEM_MONITOR_TEST_STUB_IMPORT_DIR));
    QObject::connect(host->engine.get(), &QQmlEngine::warnings, host->engine.get(),
                     [raw = host.get()](const QList<QQmlError> &errs) {
                         raw->warnings.append(errs);
                     });

    QQmlComponent component(host->engine.get(), QUrl::fromLocalFile(mainQmlPath));
    if (!component.isReady()) {
        qWarning() << "Component errors:" << component.errorString();
        return nullptr;
    }

    QObject *created = component.create();
    auto *item = qobject_cast<QQuickItem *>(created);
    if (!item) {
        delete created;
        return nullptr;
    }

    host->window = std::make_unique<QQuickWindow>();
    host->window->resize(948, 180);
    item->setParentItem(host->window->contentItem());
    item->setSize(QSizeF(948, 180));
    host->window->show();

    host->rootItem = item;
    host->plasmoidAttached = qobject_cast<MockPlasmoidAttached *>(
        qmlAttachedPropertiesObject<MockPlasmoid>(item, false));
    QCoreApplication::processEvents();
    return host;
}

class SystemMonitorQmlTest : public QObject
{
    Q_OBJECT

private:
    QDBusConnection openPeerBus(const QString &connName)
    {
        const QString addr = QString::fromUtf8(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"));
        return QDBusConnection::connectToBus(addr, connName);
    }

private slots:
    void initTestCase()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));

        // Strictly private isolated D-Bus, never live host system bus.
        QVERIFY(!qgetenv("DBUS_SYSTEM_BUS_ADDRESS").isEmpty());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"), qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(QDBusConnection::systemBus().isConnected());

        qmlRegisterUncreatableType<MockPlasmaTypes>(
            "org.kde.plasma.core", 2, 0, "Types", QStringLiteral("Enum container"));
        qmlRegisterUncreatableType<MockPlasmoid>(
            "org.kde.plasma.plasmoid", 2, 0, "Plasmoid", QStringLiteral("Attached type"));
        qmlRegisterType<MockPlasmoidItem>(
            "org.kde.plasma.plasmoid", 2, 0, "PlasmoidItem");
    }

    // Scenario 21: Verify old CLI, Plasma5Support, user-specific paths, and wrong units are gone.
    void staticLegacyPathRemovalAudit()
    {
        const QString mainQmlPath = pathOverrideOrDefault(
            "ZETHROPOL_SYSTEM_MONITOR_MAIN_QML", QStringLiteral(SYSTEM_MONITOR_MAIN_QML));
        QFile f(mainQmlPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString src = QString::fromUtf8(f.readAll());

        QVERIFY(src.contains(QStringLiteral("import org.zethropol.monitoring 1.0")));
        QVERIFY(src.contains(QStringLiteral("MonitoringBridge")));
        QVERIFY(!src.contains(QStringLiteral("Plasma5Support")));
        QVERIFY(!src.contains(QStringLiteral("plasma5support")));
        QVERIFY(!src.contains(QStringLiteral("DataSource")));
        QVERIFY(!src.contains(QStringLiteral("executable")));
        QVERIFY(!src.contains(QStringLiteral("/home/Zevor")));
        QVERIFY(!src.contains(QStringLiteral("zethropol-hardware")));
        QVERIFY(!src.contains(QStringLiteral("--monitor")));
        QVERIFY(!src.contains(QStringLiteral("MB/s")));
        QVERIFY(!src.contains(QStringLiteral("MiB/s")));
        QVERIFY(!src.contains(QStringLiteral("KSystemStats")));
    }

    // Scenarios 1, 2, 3, 4, 8, 9, 10, 11, 12, 13, 20:
    // Five pods connected to single MonitoringBridge, reactive updates, Canvas paint verification,
    // unit conversions/formatting (CPU MHz, RAM/VRAM GiB, Network Mbps, missing ping, Storage Btrfs %
    // and decimal GB), and backgroundOpacity preservation.
    void fivePodsSingleBridgeReactiveCanvasUnitsAndOpacity()
    {
        auto serverBus = openPeerBus(QStringLiteral("sm_srv_1"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringService mock(serverBus);
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("sm_srv_1"));
        });

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);
        QVERIFY(host->rootItem != nullptr);
        QVERIFY(host->plasmoidAttached != nullptr);
        QVERIFY2(host->warnings.isEmpty(), "Unexpected QML warnings while loading main.qml");

        // Scenario 2: Exact single MonitoringBridge instance and single initial GetState call
        const auto bridges = host->rootItem->findChildren<QObject *>(QStringLiteral("systemMonitorBridge"));
        QCOMPARE(bridges.size(), 1);
        QCOMPARE(host->plasmoidAttached->title(), QStringLiteral("Zethropol System Monitor"));
        QCOMPARE(host->plasmoidAttached->backgroundHints(), int(MockPlasmaTypes::NoBackground));

        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));
        QCOMPARE(mock.getStateCalls, 1);

        // Scenario 1, 8: CPU LOAD (GHz -> MHz)
        QCOMPARE(labelText(host->rootItem, "cpuTempLabel"), QStringLiteral("46 °C"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("3825 MHz"));

        // Scenario 1, 9, 10: RAM USAGE (GiB + used / total * 100)
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("25.0"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("32.0 GiB RAM"));

        // Scenario 1, 9: GPU LOAD (GiB VRAM)
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("37.2"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("31 °C  •  0.4/4.0 GiB"));
        QCOMPARE(labelText(host->rootItem, "gpuClockLabel"), QStringLiteral("1149 MHz"));

        // Scenario 1, 11, 12: NETWORK (Mbps + missing ping -> "— ms")
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("12.34 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkUploadLabel"), QStringLiteral("5.67 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkPingLabel"), QStringLiteral("— ms  •  PING"));

        // Scenario 1, 13: STORAGE (direct Btrfs 51.83% -> "51.8", NOT 255.84/495.81 = 51.6%, decimal GB)
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("51.8"));
        QCOMPARE(labelText(host->rootItem, "storageCapacityLabel"), QStringLiteral("255.8 / 495.8 GB"));
        QCOMPARE(labelText(host->rootItem, "storageTempLabel"), QStringLiteral("33 °C"));

        // Scenario 4: Verify all 4 Canvas gauges actually painted active arcs in offscreen software mode
        QObject *cpuGauge = findChildStrict(host->rootItem, "cpuGauge");
        QObject *ramGauge = findChildStrict(host->rootItem, "ramGauge");
        QObject *gpuGauge = findChildStrict(host->rootItem, "gpuGauge");
        QObject *storageGauge = findChildStrict(host->rootItem, "storageGauge");
        QVERIFY(cpuGauge && ramGauge && gpuGauge && storageGauge);

        QTRY_VERIFY(cpuGauge->property("paintCount").toInt() >= 1);
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(ramGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(gpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(storageGauge->property("lastPaintedActive").toBool());
        QCOMPARE(qRound(storageGauge->property("lastPaintedValue").toDouble() * 100), 5183);

        const int cpuPaintsBefore = cpuGauge->property("paintCount").toInt();
        const int gpuPaintsBefore = gpuGauge->property("paintCount").toInt();

        // Scenario 3 & 4: StateChanged reactively updates all 5 pods and triggers Canvas repaints
        // (synthetic high GPU load 98.5% without running real GPU stress!)
        QVariantMap updated = sampleFullMetrics();
        updated.insert(QStringLiteral("cpu_usage_percent"), 73.4);
        updated.insert(QStringLiteral("cpu_frequency_ghz"), 4.20);
        updated.insert(QStringLiteral("memory_used_gb"), 16.0);
        updated.insert(QStringLiteral("gpu_usage_percent"), 98.5);
        updated.insert(QStringLiteral("gpu_vram_used_gb"), 3.8);
        updated.insert(QStringLiteral("network_download_mbps"), 145.67);
        updated.insert(QStringLiteral("network_ping_ms"), 6.1);
        updated.insert(QStringLiteral("storage_filesystem_usage_percent"), 62.4);

        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), updated));

        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("73.4"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("4200 MHz"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("50.0"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("98.5"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("31 °C  •  3.8/4.0 GiB"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("145.67 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkPingLabel"), QStringLiteral("6.1 ms  •  PING"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("62.4"));

        QTRY_VERIFY(cpuGauge->property("paintCount").toInt() > cpuPaintsBefore);
        QTRY_VERIFY(gpuGauge->property("paintCount").toInt() > gpuPaintsBefore);
        QCOMPARE(qRound(gpuGauge->property("lastPaintedValue").toDouble() * 10), 985);

        // Scenario 20: backgroundOpacity configuration preserved & clamped across all 5 pods
        const char *podNames[] = {"cpuPod", "ramPod", "gpuPod", "networkPod", "storagePod"};
        for (const char *pod : podNames) {
            QObject *rect = findChildStrict(host->rootItem, pod);
            QVERIFY(rect != nullptr);
            const QColor col = rect->property("color").value<QColor>();
            QVERIFY(std::abs(col.alphaF() - 0.60) < 0.02);
        }

        host->plasmoidAttached->configObject()->setBackgroundOpacity(0);
        for (const char *pod : podNames) {
            const QColor col = findChildStrict(host->rootItem, pod)->property("color").value<QColor>();
            QVERIFY(std::abs(col.alphaF() - 0.0) < 0.01);
        }

        host->plasmoidAttached->configObject()->setBackgroundOpacity(100);
        for (const char *pod : podNames) {
            const QColor col = findChildStrict(host->rootItem, pod)->property("color").value<QColor>();
            QVERIFY(std::abs(col.alphaF() - 1.0) < 0.01);
        }

        host->plasmoidAttached->configObject()->setBackgroundOpacity(150);
        QVERIFY(std::abs(findChildStrict(host->rootItem, "cpuPod")->property("color").value<QColor>().alphaF() - 1.0) < 0.01);

        host->plasmoidAttached->configObject()->setBackgroundOpacity(-25);
        QVERIFY(std::abs(findChildStrict(host->rootItem, "cpuPod")->property("color").value<QColor>().alphaF() - 0.0) < 0.01);

        QVERIFY(!anyLabelContainsInvalidToken(host->rootItem));
        QVERIFY(host->warnings.isEmpty());
    }

    // Scenarios 5, 6, 7, 10, 14, 15, 16:
    // Real zeros preserved, missing sensors separated, NaN/Inf/used>total/zero-denominator guarded,
    // physical disk capacity never used for storage, GPU-less system, degraded partial telemetry.
    void zerosMissingInvalidMathGpuLessStorageNoPhysicalFallbackAndDegraded()
    {
        auto serverBus = openPeerBus(QStringLiteral("sm_srv_2"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringService mock(serverBus);
        // Start in initializing state
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
            QDBusConnection::disconnectFromBus(QStringLiteral("sm_srv_2"));
        });

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);

        // Initializing: all 5 pods show "—", all 4 gauges neutral
        QTRY_COMPARE(host->rootItem->property("telemetryReadiness").toString(), QStringLiteral("initializing"));
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("— Mbps"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("—"));

        QObject *cpuGauge = findChildStrict(host->rootItem, "cpuGauge");
        QObject *ramGauge = findChildStrict(host->rootItem, "ramGauge");
        QObject *gpuGauge = findChildStrict(host->rootItem, "gpuGauge");
        QObject *storageGauge = findChildStrict(host->rootItem, "storageGauge");
        QTRY_VERIFY(cpuGauge->property("paintCount").toInt() >= 1);
        QVERIFY(!cpuGauge->property("lastPaintedActive").toBool());
        QVERIFY(!ramGauge->property("lastPaintedActive").toBool());
        QVERIFY(!gpuGauge->property("lastPaintedActive").toBool());
        QVERIFY(!storageGauge->property("lastPaintedActive").toBool());

        // Scenario 5: Real zero values preserved across all 5 pods
        QVariantMap realZeros{
            {QStringLiteral("cpu_usage_percent"), 0.0},
            {QStringLiteral("cpu_temperature_c"), 0.0},
            {QStringLiteral("cpu_frequency_ghz"), 0.0},
            {QStringLiteral("memory_used_gb"), 0.0},
            {QStringLiteral("memory_total_gb"), 32.0},
            {QStringLiteral("gpu_usage_percent"), 0.0},
            {QStringLiteral("gpu_temperature_c"), 0.0},
            {QStringLiteral("gpu_core_clock_mhz"), 0.0},
            {QStringLiteral("gpu_vram_used_gb"), 0.0},
            {QStringLiteral("gpu_vram_total_gb"), 4.0},
            {QStringLiteral("network_download_mbps"), 0.0},
            {QStringLiteral("network_upload_mbps"), 0.0},
            {QStringLiteral("storage_filesystem_usage_percent"), 0.0},
            {QStringLiteral("storage_filesystem_used_gb"), 0.0},
            {QStringLiteral("storage_filesystem_total_gb"), 495.8},
            {QStringLiteral("storage_temperature_c"), 0.0},
        };
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), realZeros));

        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("0.0"));
        QCOMPARE(labelText(host->rootItem, "cpuTempLabel"), QStringLiteral("0 °C"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("0 MHz"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("0.0"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("32.0 GiB RAM"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("0.0"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("0 °C  •  0.0/4.0 GiB"));
        QCOMPARE(labelText(host->rootItem, "gpuClockLabel"), QStringLiteral("0 MHz"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("0.00 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkUploadLabel"), QStringLiteral("0.00 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkPingLabel"), QStringLiteral("— ms  •  PING"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("0.0"));
        QCOMPARE(labelText(host->rootItem, "storageCapacityLabel"), QStringLiteral("0.0 / 495.8 GB"));
        QCOMPARE(labelText(host->rootItem, "storageTempLabel"), QStringLiteral("0 °C"));
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(gpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(storageGauge->property("lastPaintedActive").toBool());

        // Scenarios 6, 14, 15, 16: GPU-less system + degraded partial telemetry +
        // physical storage present WITHOUT filesystem metrics (MUST NOT fall back to physical capacity!)
        QVariantMap gpuLessAndNoFs{
            {QStringLiteral("cpu_usage_percent"), 15.0},
            // cpu_temperature_c & cpu_frequency_ghz missing
            {QStringLiteral("memory_used_gb"), 6.0},
            {QStringLiteral("memory_total_gb"), 24.0},
            // ALL 5 GPU metrics absent (GPU-less host)
            // Network offline (all network metrics absent)
            // Physical disk present, filesystem metrics absent:
            {QStringLiteral("storage_used_gb"), 250.0},
            {QStringLiteral("storage_capacity_gb"), 1000.0},
            {QStringLiteral("storage_physical_capacity_gb"), 1000.0},
            {QStringLiteral("storage_temperature_c"), 35.0},
        };
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("degraded"), gpuLessAndNoFs));

        QTRY_COMPARE(host->rootItem->property("telemetryReadiness").toString(), QStringLiteral("degraded"));
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("15.0"));
        QCOMPARE(labelText(host->rootItem, "cpuTempLabel"), QStringLiteral("— °C"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("— MHz"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("25.0"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("24.0 GiB RAM"));
        // GPU-less: no crash, no NaN, no fake 0.0/0.0 GiB, neutral gauge
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("— °C  •  —/— GiB"));
        QCOMPARE(labelText(host->rootItem, "gpuClockLabel"), QStringLiteral("— MHz"));
        QTRY_VERIFY(!gpuGauge->property("lastPaintedActive").toBool());
        // Offline network -> "—"
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("— Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkUploadLabel"), QStringLiteral("— Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkPingLabel"), QStringLiteral("— ms  •  PING"));
        // Storage: NO fallback to storage_physical_capacity_gb (1000.0) or storage_used_gb (250.0)!
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "storageCapacityLabel"), QStringLiteral("— / — GB"));
        QCOMPARE(labelText(host->rootItem, "storageTempLabel"), QStringLiteral("35 °C"));
        QTRY_VERIFY(!storageGauge->property("lastPaintedActive").toBool());

        // Scenarios 7 & 10: NaN, Infinity, RAM zero denominator, RAM used > total, VRAM used > total
        QVariantMap invalidEdgeCases{
            {QStringLiteral("cpu_usage_percent"), std::numeric_limits<double>::quiet_NaN()},
            {QStringLiteral("cpu_temperature_c"), std::numeric_limits<double>::infinity()},
            {QStringLiteral("cpu_frequency_ghz"), -std::numeric_limits<double>::infinity()},
            {QStringLiteral("memory_used_gb"), 4.0},
            {QStringLiteral("memory_total_gb"), 0.0}, // zero denominator!
            {QStringLiteral("gpu_vram_used_gb"), 6.0},
            {QStringLiteral("gpu_vram_total_gb"), 4.0}, // inconsistent VRAM used > total!
            {QStringLiteral("network_download_mbps"), 1.0}, // keep at least 1 valid metric so envelope succeeds
        };
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("degraded"), invalidEdgeCases));

        QTRY_COMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("1.00 Mbps"));
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "cpuTempLabel"), QStringLiteral("— °C"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("— MHz"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("— GiB RAM"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("— °C  •  —/— GiB"));
        QTRY_VERIFY(!cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(!ramGauge->property("lastPaintedActive").toBool());
        QVERIFY(!anyLabelContainsInvalidToken(host->rootItem));

        // RAM used > total -> invalid measurement (never >100%)
        QVariantMap ramOverTotal{
            {QStringLiteral("memory_used_gb"), 40.0},
            {QStringLiteral("memory_total_gb"), 32.0},
            {QStringLiteral("cpu_usage_percent"), 10.0},
        };
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("degraded"), ramOverTotal));

        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("10.0"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("— GiB RAM"));
        QTRY_VERIFY(!ramGauge->property("lastPaintedActive").toBool());
        QVERIFY(!anyLabelContainsInvalidToken(host->rootItem));
        QVERIFY(host->warnings.isEmpty());
    }

    // Scenarios 17, 18, 19: Proactive staleness notification, service disconnect invalidation,
    // no continuous repaints during error state, and automatic recovery upon service restart.
    void stalenessDisconnectNoErrorSpamAndRestartRecovery()
    {
        auto serverBus = openPeerBus(QStringLiteral("sm_srv_3"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringService mock(serverBus);
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("sm_srv_3"));
        });

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);

        QObject *cpuGauge = findChildStrict(host->rootItem, "cpuGauge");
        QObject *storageGauge = findChildStrict(host->rootItem, "storageGauge");
        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(storageGauge->property("lastPaintedActive").toBool());

        // Scenario 18: Proactive staleness notification clears all 5 pods immediately
        mock.emitStateChanged(makeEnvelope(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("Monitoring telemetry is stale; waiting for a fresh hardware sample."),
            QStringLiteral("dependency-failed"),
            {}));

        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("— Mbps"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("—"));
        QTRY_VERIFY(!cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(!storageGauge->property("lastPaintedActive").toBool());

        // Repeated identical error/stale envelopes must NOT cause continuous Canvas repaints
        const int paintsAfterStale = cpuGauge->property("paintCount").toInt();
        mock.emitStateChanged(makeEnvelope(
            false,
            QStringLiteral("not-available"),
            QStringLiteral("Monitoring telemetry is stale; waiting for a fresh hardware sample."),
            QStringLiteral("dependency-failed"),
            {}));
        QTest::qWait(80);
        QCOMPARE(cpuGauge->property("paintCount").toInt(), paintsAfterStale);

        // Recover via StateChanged before testing service disconnect
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), sampleFullMetrics()));
        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());

        // Scenario 17: Service disconnects -> immediate invalidation of all 5 pods
        serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
        QTRY_VERIFY(!host->rootItem->property("telemetryConnected").toBool());
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("— Mbps"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("—"));
        QTRY_VERIFY(!cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(!storageGauge->property("lastPaintedActive").toBool());

        // Scenario 19: Service restarts -> automatic recovery with fresh telemetry
        QVariantMap recovered = sampleFullMetrics();
        recovered.insert(QStringLiteral("cpu_usage_percent"), 41.2);
        mock.nextReply = makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), recovered);
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));

        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("41.2"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("51.8"));
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(storageGauge->property("lastPaintedActive").toBool());
        QVERIFY(host->warnings.isEmpty());
    }

    // Scenario 22 (part 1): Integrated QML test of real `services/system-monitor/contents/ui/main.qml`
    // against newly built `hardware` + `monitoring` release binaries on isolated private D-Bus.
    void realReleaseBinariesIntegratedMainQmlTest()
    {
        const QString hwBin = qEnvironmentVariable("ZETHROPOL_HARDWARE_RELEASE_BIN");
        const QString monBin = qEnvironmentVariable("ZETHROPOL_MONITORING_RELEASE_BIN");
        if (hwBin.isEmpty() || monBin.isEmpty()) {
            QSKIP("Set ZETHROPOL_HARDWARE_RELEASE_BIN and ZETHROPOL_MONITORING_RELEASE_BIN to run real-binary QML E2E");
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

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);
        QVERIFY(host->warnings.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(
            host->rootItem->property("telemetryReadiness").toString(),
            QStringLiteral("ready"),
            6000);

        QVERIFY(labelText(host->rootItem, "cpuUsageLabel") != QStringLiteral("—"));
        QVERIFY(labelText(host->rootItem, "ramUsageLabel") != QStringLiteral("—"));
        QVERIFY(labelText(host->rootItem, "ramTotalLabel").endsWith(QStringLiteral(" GiB RAM")));
        QVERIFY(labelText(host->rootItem, "storageUsageLabel") != QStringLiteral("—"));
        QVERIFY(labelText(host->rootItem, "storageCapacityLabel").endsWith(QStringLiteral(" GB")));
        QVERIFY(labelText(host->rootItem, "networkDownloadLabel").endsWith(QStringLiteral(" Mbps")));
        QVERIFY(labelText(host->rootItem, "networkUploadLabel").endsWith(QStringLiteral(" Mbps")));
        QCOMPARE(labelText(host->rootItem, "networkPingLabel"), QStringLiteral("— ms  •  PING"));
        QVERIFY(!anyLabelContainsInvalidToken(host->rootItem));

        QObject *cpuGauge = findChildStrict(host->rootItem, "cpuGauge");
        QObject *ramGauge = findChildStrict(host->rootItem, "ramGauge");
        QObject *storageGauge = findChildStrict(host->rootItem, "storageGauge");
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(ramGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(storageGauge->property("lastPaintedActive").toBool());

        // Live Monitoring daemon restart -> immediate invalidation + automatic recovery
        monProc.terminate();
        QVERIFY(monProc.waitForFinished(3000));
        QTRY_COMPARE_WITH_TIMEOUT(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"), 3000);
        QTRY_VERIFY(!cpuGauge->property("lastPaintedActive").toBool());

        monProc.start();
        QVERIFY(monProc.waitForStarted(3000));
        QTRY_COMPARE_WITH_TIMEOUT(
            host->rootItem->property("telemetryReadiness").toString(),
            QStringLiteral("ready"),
            6000);
        QVERIFY(labelText(host->rootItem, "cpuUsageLabel") != QStringLiteral("—"));
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QVERIFY(!anyLabelContainsInvalidToken(host->rootItem));
    }

    // Stage 04.3 Görev 7: Full 6-layer controlled telemetry cross-stack integrity check:
    // Mock Hardware.GetTelemetry (Layers 1-2) -> REAL `monitoring` release binary
    // Monitoring.GetState (Layer 3) + Monitoring.StateChanged (Layer 4) ->
    // REAL `libzethropol_monitoring_qml.so` MonitoringBridge.metrics (Layer 5) ->
    // REAL 5-pod `services/system-monitor/contents/ui/main.qml` Labels & Canvases (Layer 6).
    void controlledSixLayerCrossStackVerification()
    {
        const QString monBin = qEnvironmentVariable("ZETHROPOL_MONITORING_RELEASE_BIN");
        if (monBin.isEmpty()) {
            QSKIP("Set ZETHROPOL_MONITORING_RELEASE_BIN to run 6-layer cross-stack E2E");
        }
        QVERIFY(QFile::exists(monBin));

        auto hwBus = openPeerBus(QStringLiteral("sm_hw_6layer"));
        QVERIFY(hwBus.isConnected());
        MockHardwareTelemetryService mockHw;

        auto makeHwOk = [](const QVariantMap &sections) {
            return QVariantMap{
                {QStringLiteral("success"), true},
                {QStringLiteral("code"), QStringLiteral("ok")},
                {QStringLiteral("message"), QString()},
                {QStringLiteral("operationId"), QString()},
                {QStringLiteral("data"), sections},
            };
        };

        // Case 1: Initial warming sample (cpu usage omitted, active link rates omitted -> degraded)
        mockHw.nextEnvelope = makeHwOk(QVariantMap{
            {QStringLiteral("cpu"), QVariantMap{
                {QStringLiteral("temperature_c"), 42.0},
                {QStringLiteral("frequency_ghz"), 3.60},
            }},
            {QStringLiteral("gpu"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("usage_percent"), 0.0},
                {QStringLiteral("vram_used_gb"), 0.42},
                {QStringLiteral("vram_total_gb"), 4.0},
                {QStringLiteral("temperature_c"), 29.0},
                {QStringLiteral("core_clock_mhz"), 995.0},
            }},
            {QStringLiteral("memory"), QVariantMap{
                {QStringLiteral("used_gb"), 8.0},
                {QStringLiteral("total_gb"), 32.0},
            }},
            {QStringLiteral("normalized_storage"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("capacity_gb"), 500.11},
                {QStringLiteral("physical_capacity_gb"), 500.11},
                {QStringLiteral("used_gb"), 255.84},
                {QStringLiteral("available_gb"), 237.79},
                {QStringLiteral("filesystem_total_gb"), 495.81},
                {QStringLiteral("filesystem_used_gb"), 255.84},
                {QStringLiteral("filesystem_free_gb"), 239.98},
                {QStringLiteral("filesystem_available_gb"), 237.79},
                {QStringLiteral("filesystem_usage_percent"), 51.83},
                {QStringLiteral("temperature_c"), 33.0},
            }},
            {QStringLiteral("network"), QVariantMap{
                {QStringLiteral("interface"), QStringLiteral("enp34s0")},
                {QStringLiteral("operstate"), QStringLiteral("up")},
            }},
        });

        QVERIFY(hwBus.registerObject(
            QStringLiteral("/org/zethropol/Hardware"), &mockHw, QDBusConnection::ExportAllSlots));
        QVERIFY(hwBus.registerService(QStringLiteral("org.zethropol.Hardware")));

        QProcess monProc;
        monProc.setProgram(monBin);
        monProc.start();
        QVERIFY(monProc.waitForStarted(3000));

        const auto cleanup = qScopeGuard([&] {
            if (monProc.state() != QProcess::NotRunning) {
                monProc.terminate();
                if (!monProc.waitForFinished(2000)) {
                    monProc.kill();
                    monProc.waitForFinished();
                }
            }
            hwBus.unregisterService(QStringLiteral("org.zethropol.Hardware"));
            hwBus.unregisterObject(QStringLiteral("/org/zethropol/Hardware"));
            QDBusConnection::disconnectFromBus(QStringLiteral("sm_hw_6layer"));
        });

        QTRY_VERIFY_WITH_TIMEOUT(
            QDBusConnection::systemBus().interface()->isServiceRegistered(
                QStringLiteral("org.zethropol.Monitoring")).value(),
            5000);

        MonitoringSignalCapture sigCap;
        QVERIFY(QDBusConnection::systemBus().connect(
            QStringLiteral("org.zethropol.Monitoring"),
            QStringLiteral("/org/zethropol/Monitoring"),
            QStringLiteral("org.zethropol.Monitoring.v1"),
            QStringLiteral("StateChanged"),
            &sigCap,
            SLOT(onStateChanged(QDBusMessage))));

        auto callMonGetState = []() -> QVariantMap {
            const QDBusMessage msg = QDBusMessage::createMethodCall(
                QStringLiteral("org.zethropol.Monitoring"),
                QStringLiteral("/org/zethropol/Monitoring"),
                QStringLiteral("org.zethropol.Monitoring.v1"),
                QStringLiteral("GetState"));
            const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 2000);
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
                return {};
            }
            return unwrapDbusArg(reply.arguments().constFirst()).toMap();
        };

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);
        QObject *bridge = findChildStrict(host->rootItem, "systemMonitorBridge");
        QVERIFY(bridge != nullptr);
        QObject *cpuGauge = findChildStrict(host->rootItem, "cpuGauge");
        QObject *ramGauge = findChildStrict(host->rootItem, "ramGauge");
        QObject *gpuGauge = findChildStrict(host->rootItem, "gpuGauge");
        QObject *storageGauge = findChildStrict(host->rootItem, "storageGauge");

        // Verify Case 1 (warming degraded) across Layers 3, 5, 6
        QTRY_COMPARE_WITH_TIMEOUT(
            host->rootItem->property("telemetryReadiness").toString(),
            QStringLiteral("degraded"),
            4000);
        const QVariantMap l3Warm = callMonGetState();
        QCOMPARE(l3Warm.value(QStringLiteral("readiness")).toString(), QStringLiteral("degraded"));
        QVERIFY(!l3Warm.value(QStringLiteral("data")).toMap().contains(QStringLiteral("cpu_usage_percent")));
        QVERIFY(!bridge->property("metrics").toMap().contains(QStringLiteral("cpu_usage_percent")));
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("3600 MHz"));
        QTRY_VERIFY(!cpuGauge->property("lastPaintedActive").toBool());

        // Case 2: Normal full measurements across all 6 layers
        mockHw.nextEnvelope = makeHwOk(QVariantMap{
            {QStringLiteral("cpu"), QVariantMap{
                {QStringLiteral("usage_percent"), 24.5},
                {QStringLiteral("temperature_c"), 46.4},
                {QStringLiteral("frequency_ghz"), 3.825},
            }},
            {QStringLiteral("gpu"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("usage_percent"), 37.2},
                {QStringLiteral("memory_usage_percent"), 15.0},
                {QStringLiteral("vram_used_gb"), 0.42},
                {QStringLiteral("vram_total_gb"), 4.0},
                {QStringLiteral("temperature_c"), 31.2},
                {QStringLiteral("core_clock_mhz"), 1149.0},
                {QStringLiteral("memory_clock_mhz"), 300.0},
            }},
            {QStringLiteral("memory"), QVariantMap{
                {QStringLiteral("used_gb"), 8.0},
                {QStringLiteral("total_gb"), 32.0},
            }},
            {QStringLiteral("normalized_storage"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("capacity_gb"), 500.11},
                {QStringLiteral("physical_capacity_gb"), 500.11},
                {QStringLiteral("used_gb"), 255.84},
                {QStringLiteral("available_gb"), 237.79},
                {QStringLiteral("filesystem_total_gb"), 495.81},
                {QStringLiteral("filesystem_used_gb"), 255.84},
                {QStringLiteral("filesystem_free_gb"), 239.98},
                {QStringLiteral("filesystem_available_gb"), 237.79},
                {QStringLiteral("filesystem_usage_percent"), 51.83},
                {QStringLiteral("temperature_c"), 33.4},
            }},
            {QStringLiteral("network"), QVariantMap{
                {QStringLiteral("interface"), QStringLiteral("enp34s0")},
                {QStringLiteral("operstate"), QStringLiteral("up")},
                {QStringLiteral("download_mbps"), 12.34},
                {QStringLiteral("upload_mbps"), 5.67},
            }},
        });

        QTRY_COMPARE_WITH_TIMEOUT(
            host->rootItem->property("telemetryReadiness").toString(),
            QStringLiteral("ready"),
            4000);
        const QVariantMap l3Norm = callMonGetState();
        const QVariantMap l3Data = l3Norm.value(QStringLiteral("data")).toMap();
        const QVariantMap l5Data = bridge->property("metrics").toMap();
        QVERIFY(!sigCap.receivedEnvelopes.isEmpty());
        const QVariantMap l4Last = unwrapDbusArg(sigCap.receivedEnvelopes.constLast()).toMap();
        const QVariantMap l4Data = l4Last.value(QStringLiteral("data")).toMap();

        // Exact cross-layer equality across L3 (GetState), L4 (StateChanged), L5 (Bridge)
        QCOMPARE(l3Data.value(QStringLiteral("cpu_usage_percent")).toDouble(), 24.5);
        QCOMPARE(l4Data.value(QStringLiteral("cpu_usage_percent")).toDouble(), 24.5);
        QCOMPARE(l5Data.value(QStringLiteral("cpu_usage_percent")).toDouble(), 24.5);
        QCOMPARE(l3Data.value(QStringLiteral("storage_filesystem_usage_percent")).toDouble(), 51.83);
        QCOMPARE(l4Data.value(QStringLiteral("storage_filesystem_usage_percent")).toDouble(), 51.83);
        QCOMPARE(l5Data.value(QStringLiteral("storage_filesystem_usage_percent")).toDouble(), 51.83);
        QVERIFY(!l3Data.contains(QStringLiteral("network_ping_ms")));
        QVERIFY(!l5Data.contains(QStringLiteral("network_ping_ms")));

        // Layer 6 UI assertions
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("3825 MHz"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("25.0"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("32.0 GiB RAM"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("37.2"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("31 °C  •  0.4/4.0 GiB"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("12.34 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkUploadLabel"), QStringLiteral("5.67 Mbps"));
        QCOMPARE(labelText(host->rootItem, "networkPingLabel"), QStringLiteral("— ms  •  PING"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("51.8"));
        QCOMPARE(labelText(host->rootItem, "storageCapacityLabel"), QStringLiteral("255.8 / 495.8 GB"));
        QTRY_VERIFY(cpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(ramGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(gpuGauge->property("lastPaintedActive").toBool());
        QTRY_VERIFY(storageGauge->property("lastPaintedActive").toBool());

        // Case 3: Real zero values preserved identically across Layers 2-6
        mockHw.nextEnvelope = makeHwOk(QVariantMap{
            {QStringLiteral("cpu"), QVariantMap{
                {QStringLiteral("usage_percent"), 0.0},
                {QStringLiteral("temperature_c"), 0.0},
                {QStringLiteral("frequency_ghz"), 0.0},
            }},
            {QStringLiteral("gpu"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("usage_percent"), 0.0},
                {QStringLiteral("memory_usage_percent"), 0.0},
                {QStringLiteral("vram_used_gb"), 0.0},
                {QStringLiteral("vram_total_gb"), 4.0},
                {QStringLiteral("temperature_c"), 0.0},
                {QStringLiteral("core_clock_mhz"), 0.0},
                {QStringLiteral("memory_clock_mhz"), 0.0},
            }},
            {QStringLiteral("memory"), QVariantMap{
                {QStringLiteral("used_gb"), 0.0},
                {QStringLiteral("total_gb"), 32.0},
            }},
            {QStringLiteral("normalized_storage"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("capacity_gb"), 500.11},
                {QStringLiteral("physical_capacity_gb"), 500.11},
                {QStringLiteral("used_gb"), 0.0},
                {QStringLiteral("available_gb"), 495.81},
                {QStringLiteral("filesystem_total_gb"), 495.81},
                {QStringLiteral("filesystem_used_gb"), 0.0},
                {QStringLiteral("filesystem_free_gb"), 495.81},
                {QStringLiteral("filesystem_available_gb"), 495.81},
                {QStringLiteral("filesystem_usage_percent"), 0.0},
                {QStringLiteral("temperature_c"), 0.0},
            }},
            {QStringLiteral("network"), QVariantMap{
                {QStringLiteral("interface"), QStringLiteral("enp34s0")},
                {QStringLiteral("operstate"), QStringLiteral("up")},
                {QStringLiteral("download_mbps"), 0.0},
                {QStringLiteral("upload_mbps"), 0.0},
            }},
        });

        QTRY_COMPARE_WITH_TIMEOUT(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("0.0"), 4000);
        const QVariantMap l5Zero = bridge->property("metrics").toMap();
        QCOMPARE(l5Zero.value(QStringLiteral("cpu_usage_percent")).toDouble(), 0.0);
        QCOMPARE(l5Zero.value(QStringLiteral("gpu_vram_used_gb")).toDouble(), 0.0);
        QCOMPARE(l5Zero.value(QStringLiteral("network_download_mbps")).toDouble(), 0.0);
        QCOMPARE(l5Zero.value(QStringLiteral("storage_filesystem_usage_percent")).toDouble(), 0.0);
        QCOMPARE(labelText(host->rootItem, "cpuFreqLabel"), QStringLiteral("0 MHz"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("0.0"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("0 °C  •  0.0/4.0 GiB"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("0.00 Mbps"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("0.0"));

        // Case 4: Invalid types, NaN, +/-Inf, out-of-range, and INCONSISTENT CAPACITIES
        // MUST be rejected at Layer 3 (Monitoring) before reaching Layers 4, 5, 6!
        mockHw.nextEnvelope = makeHwOk(QVariantMap{
            {QStringLiteral("cpu"), QVariantMap{
                {QStringLiteral("usage_percent"), std::numeric_limits<double>::quiet_NaN()},
                {QStringLiteral("temperature_c"), 44.0}, // valid!
                {QStringLiteral("frequency_ghz"), std::numeric_limits<double>::infinity()},
            }},
            {QStringLiteral("gpu"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("usage_percent"), 145.0}, // out of range
                {QStringLiteral("vram_used_gb"), 6.0},
                {QStringLiteral("vram_total_gb"), 4.0}, // inconsistent used > total!
            }},
            {QStringLiteral("memory"), QVariantMap{
                {QStringLiteral("used_gb"), 40.0},
                {QStringLiteral("total_gb"), 32.0}, // inconsistent used > total!
            }},
            {QStringLiteral("normalized_storage"), QVariantMap{
                {QStringLiteral("detected"), true},
                {QStringLiteral("filesystem_total_gb"), 0.0}, // invalid zero capacity!
                {QStringLiteral("filesystem_used_gb"), 10.0},
                {QStringLiteral("physical_capacity_gb"), 500.0},
            }},
            {QStringLiteral("network"), QVariantMap{
                {QStringLiteral("interface"), QStringLiteral("enp34s0")},
                {QStringLiteral("operstate"), QStringLiteral("up")},
                {QStringLiteral("download_mbps"), 2.5}, // valid!
                {QStringLiteral("upload_mbps"), QStringLiteral("not-a-number")}, // wrong type
            }},
        });

        QTRY_COMPARE_WITH_TIMEOUT(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("2.50 Mbps"), 4000);
        const QVariantMap l3Bad = callMonGetState();
        const QVariantMap l3BadData = l3Bad.value(QStringLiteral("data")).toMap();
        const QVariantMap l5BadData = bridge->property("metrics").toMap();
        QCOMPARE(l3Bad.value(QStringLiteral("readiness")).toString(), QStringLiteral("degraded"));
        for (const char *rejected : {
                 "cpu_usage_percent",
                 "cpu_frequency_ghz",
                 "gpu_usage_percent",
                 "gpu_vram_used_gb",
                 "gpu_vram_total_gb",
                 "memory_used_gb",
                 "memory_total_gb",
                 "storage_filesystem_total_gb",
                 "network_upload_mbps",
             }) {
            const QString k = QString::fromLatin1(rejected);
            QVERIFY2(!l3BadData.contains(k), qPrintable(QStringLiteral("Monitoring leaked %1").arg(k)));
            QVERIFY2(!l5BadData.contains(k), qPrintable(QStringLiteral("Bridge leaked %1").arg(k)));
        }
        QCOMPARE(labelText(host->rootItem, "cpuTempLabel"), QStringLiteral("44 °C"));
        QCOMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "ramTotalLabel"), QStringLiteral("— GiB RAM"));
        QCOMPARE(labelText(host->rootItem, "gpuMetaLabel"), QStringLiteral("— °C  •  —/— GiB"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("—"));
        QCOMPARE(labelText(host->rootItem, "storageCapacityLabel"), QStringLiteral("— / — GB"));
        QVERIFY(!anyLabelContainsInvalidToken(host->rootItem));

        // Case 5: Dependency failure clears all layers cleanly (no stale data retained)
        mockHw.nextEnvelope = QVariantMap{
            {QStringLiteral("success"), false},
            {QStringLiteral("code"), QStringLiteral("failed")},
            {QStringLiteral("message"), QStringLiteral("synthetic hardware failure")},
            {QStringLiteral("operationId"), QString()},
            {QStringLiteral("data"), QVariantMap{}},
        };

        QTRY_COMPARE_WITH_TIMEOUT(
            host->rootItem->property("telemetryReadiness").toString(),
            QStringLiteral("dependency-failed"),
            4000);
        const QVariantMap l3Fail = callMonGetState();
        QCOMPARE(l3Fail.value(QStringLiteral("success")).toBool(), false);
        QCOMPARE(l3Fail.value(QStringLiteral("readiness")).toString(), QStringLiteral("dependency-failed"));
        QVERIFY(l3Fail.value(QStringLiteral("data")).toMap().isEmpty());
        QVERIFY(bridge->property("metrics").toMap().isEmpty());
        QCOMPARE(labelText(host->rootItem, "cpuTempLabel"), QStringLiteral("— °C"));
        QCOMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("— Mbps"));
        QTRY_VERIFY(!cpuGauge->property("lastPaintedActive").toBool());
        QVERIFY(host->warnings.isEmpty());
    }

    // Stage 04.4 Görev 1, 2, 4, 5, 7:
    // Five-pod visual consistency (all 5 accent bars & aligned titles), text overflow and
    // collision checks (including extreme long numbers), opacity isolation (only card background
    // alpha changes), and zero redundant Canvas repaints.
    void visualConsistencyAccentBarsTitleAlignmentTextOverflowAndRedundantRepaintGuard()
    {
        auto serverBus = openPeerBus(QStringLiteral("sm_srv_vis"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringService mock(serverBus);
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("sm_srv_vis"));
        });

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);
        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));

        // 1. All 5 pods have identical 180x180 geometry, radius 24, spacing 12, and matching bottom accent bar
        const char *pods[5] = {"cpuPod", "ramPod", "gpuPod", "networkPod", "storagePod"};
        const char *accents[5] = {"cpuAccentBar", "ramAccentBar", "gpuAccentBar", "networkAccentBar", "storageAccentBar"};
        const char *titles[5] = {"cpuTitleLabel", "ramTitleLabel", "gpuTitleLabel", "networkTitleLabel", "storageTitleLabel"};

        qreal refAccentY = -1.0;
        qreal refTitleY = -1.0;
        qreal prevPodRight = -1.0;

        for (int i = 0; i < 5; ++i) {
            auto *pod = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, pods[i]));
            auto *accent = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, accents[i]));
            auto *title = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, titles[i]));
            QVERIFY2(pod != nullptr, pods[i]);
            QVERIFY2(accent != nullptr, accents[i]);
            QVERIFY2(title != nullptr, titles[i]);

            QCOMPARE(pod->width(), 180.0);
            QCOMPARE(pod->height(), 180.0);
            QCOMPARE(pod->property("radius").toDouble(), 24.0);

            const QPointF podPos = pod->mapToItem(host->rootItem, QPointF(0, 0));
            if (prevPodRight >= 0.0) {
                QVERIFY(std::abs((podPos.x() - prevPodRight) - 12.0) < 0.5);
            }
            prevPodRight = podPos.x() + pod->width();

            // Accent bar geometry & color parity across all 5 pods (including GPU!)
            QVERIFY(std::abs(accent->width() - (180.0 * 0.42)) < 0.5);
            QCOMPARE(accent->height(), 2.0);
            QCOMPARE(accent->property("color").value<QColor>(), QColor(QStringLiteral("#168cff")));
            const qreal accentTopInPod = accent->mapToItem(pod, QPointF(0, 0)).y();
            if (refAccentY < 0.0) {
                refAccentY = accentTopInPod;
            } else {
                QVERIFY2(std::abs(accentTopInPod - refAccentY) < 0.5,
                         qPrintable(QStringLiteral("Accent Y mismatch on %1").arg(QLatin1String(pods[i]))));
            }

            // Title vertical alignment parity across all 5 pods
            const qreal titleTopInPod = title->mapToItem(pod, QPointF(0, 0)).y();
            if (refTitleY < 0.0) {
                refTitleY = titleTopInPod;
            } else {
                QVERIFY2(std::abs(titleTopInPod - refTitleY) < 0.5,
                         qPrintable(QStringLiteral("Title Y mismatch on %1").arg(QLatin1String(pods[i]))));
            }
        }

        auto verifyPodLabelsNoOverflowOrCollision = [&](const char *phase) {
            for (int i = 0; i < 5; ++i) {
                auto *pod = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, pods[i]));
                auto *accent = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, accents[i]));
                QVERIFY(pod && accent);
                const qreal accentTop = accent->mapToItem(pod, QPointF(0, 0)).y();

                QList<QRectF> labelRects;
                const auto children = pod->findChildren<QQuickItem *>();
                for (QQuickItem *child : children) {
                    const QVariant textVar = child->property("text");
                    if (!textVar.isValid() || textVar.typeId() != QMetaType::QString || textVar.toString().isEmpty()) {
                        continue;
                    }
                    const QPointF topLeft = child->mapToItem(pod, QPointF(0, 0));
                    const QRectF rect(topLeft, QSizeF(child->width(), child->height()));
                    QVERIFY2(rect.left() >= -0.5 && rect.right() <= pod->width() + 0.5,
                             qPrintable(QStringLiteral("[%1] Horizontal out-of-bounds in %2: %3")
                                            .arg(QLatin1String(phase), QLatin1String(pods[i]), textVar.toString())));
                    QVERIFY2(rect.top() >= -0.5 && rect.bottom() <= accentTop + 0.5,
                             qPrintable(QStringLiteral("[%1] Label overlaps/passes accent in %2: %3 (bottom=%4 accent=%5)")
                                            .arg(QLatin1String(phase), QLatin1String(pods[i]), textVar.toString())
                                            .arg(rect.bottom()).arg(accentTop)));
                    const QVariant paintedW = child->property("paintedWidth");
                    if (paintedW.isValid()) {
                        QVERIFY2(paintedW.toDouble() <= child->width() + 1.0,
                                 qPrintable(QStringLiteral("[%1] Text overflow in %2: %3 (painted=%4 > width=%5)")
                                                .arg(QLatin1String(phase), QLatin1String(pods[i]), textVar.toString())
                                                .arg(paintedW.toDouble()).arg(child->width())));
                    }
                    for (const QRectF &other : labelRects) {
                        const QRectF inter = rect.intersected(other);
                        QVERIFY2(inter.height() <= 1.0 || inter.width() <= 1.0,
                                 qPrintable(QStringLiteral("[%1] Overlapping labels in %2 near %3")
                                                .arg(QLatin1String(phase), QLatin1String(pods[i]), textVar.toString())));
                    }
                    labelRects.append(rect);
                }
            }
        };

        verifyPodLabelsNoOverflowOrCollision("normal");

        // Extreme long synthetic values & 100% max values (zero real GPU stress)
        QVariantMap extremeLong{
            {QStringLiteral("cpu_usage_percent"), 100.0},
            {QStringLiteral("cpu_temperature_c"), 105.0},
            {QStringLiteral("cpu_frequency_ghz"), 5.85},
            {QStringLiteral("memory_used_gb"), 128.0},
            {QStringLiteral("memory_total_gb"), 128.0},
            {QStringLiteral("gpu_usage_percent"), 100.0},
            {QStringLiteral("gpu_temperature_c"), 99.0},
            {QStringLiteral("gpu_core_clock_mhz"), 2650.0},
            {QStringLiteral("gpu_vram_used_gb"), 24.0},
            {QStringLiteral("gpu_vram_total_gb"), 24.0},
            {QStringLiteral("network_download_mbps"), 9999.99},
            {QStringLiteral("network_upload_mbps"), 9999.99},
            {QStringLiteral("network_ping_ms"), 999.9},
            {QStringLiteral("storage_filesystem_usage_percent"), 100.0},
            {QStringLiteral("storage_filesystem_used_gb"), 9999.9},
            {QStringLiteral("storage_filesystem_total_gb"), 9999.9},
            {QStringLiteral("storage_temperature_c"), 72.0},
        };
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), extremeLong));
        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("100.0"));
        QCOMPARE(labelText(host->rootItem, "ramUsageLabel"), QStringLiteral("100.0"));
        QCOMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("100.0"));
        QCOMPARE(labelText(host->rootItem, "storageUsageLabel"), QStringLiteral("100.0"));
        QCoreApplication::processEvents();
        verifyPodLabelsNoOverflowOrCollision("extreme-100pct");

        // Redundant Canvas repaint guard:
        // (a) Changing backgroundOpacity MUST NOT repaint Canvas gauges, and MUST keep all other elements opaque
        QObject *cpuGauge = findChildStrict(host->rootItem, "cpuGauge");
        QObject *ramGauge = findChildStrict(host->rootItem, "ramGauge");
        QObject *gpuGauge = findChildStrict(host->rootItem, "gpuGauge");
        QObject *storageGauge = findChildStrict(host->rootItem, "storageGauge");
        QTRY_VERIFY(cpuGauge->property("paintCount").toInt() >= 1);
        const int cpuP0 = cpuGauge->property("paintCount").toInt();
        const int ramP0 = ramGauge->property("paintCount").toInt();
        const int gpuP0 = gpuGauge->property("paintCount").toInt();
        const int stgP0 = storageGauge->property("paintCount").toInt();

        // (0) Re-emitting identical unchanged ready telemetry MUST NOT repaint any of the 4 Canvas gauges
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), extremeLong));
        QTest::qWait(50);
        QCOMPARE(cpuGauge->property("paintCount").toInt(), cpuP0);
        QCOMPARE(ramGauge->property("paintCount").toInt(), ramP0);
        QCOMPARE(gpuGauge->property("paintCount").toInt(), gpuP0);
        QCOMPARE(storageGauge->property("paintCount").toInt(), stgP0);

        for (int op : {0, 60, 90, 100}) {
            host->plasmoidAttached->configObject()->setBackgroundOpacity(op);
            QCoreApplication::processEvents();
            for (int i = 0; i < 5; ++i) {
                auto *pod = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, pods[i]));
                auto *accent = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, accents[i]));
                auto *title = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, titles[i]));
                const QColor bg = pod->property("color").value<QColor>();
                QVERIFY(std::abs(bg.alphaF() - (op / 100.0)) < 0.02);
                QCOMPARE(pod->opacity(), 1.0);
                QCOMPARE(accent->property("color").value<QColor>().alphaF(), 1.0);
                QCOMPARE(title->property("color").value<QColor>().alphaF(), 1.0);
            }
        }
        QTest::qWait(50);
        QCOMPARE(cpuGauge->property("paintCount").toInt(), cpuP0);
        QCOMPARE(ramGauge->property("paintCount").toInt(), ramP0);
        QCOMPARE(gpuGauge->property("paintCount").toInt(), gpuP0);
        QCOMPARE(storageGauge->property("paintCount").toInt(), stgP0);

        // (b) Changing ONLY network_download_mbps MUST NOT repaint any of the 4 circular Canvas gauges
        QVariantMap netOnlyChange = extremeLong;
        netOnlyChange.insert(QStringLiteral("network_download_mbps"), 42.5);
        mock.emitStateChanged(makeEnvelope(
            true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), netOnlyChange));
        QTRY_COMPARE(labelText(host->rootItem, "networkDownloadLabel"), QStringLiteral("42.50 Mbps"));
        QTest::qWait(50);
        QCOMPARE(cpuGauge->property("paintCount").toInt(), cpuP0);
        QCOMPARE(ramGauge->property("paintCount").toInt(), ramP0);
        QCOMPARE(gpuGauge->property("paintCount").toInt(), gpuP0);
        QCOMPARE(storageGauge->property("paintCount").toInt(), stgP0);
        QVERIFY(host->warnings.isEmpty());
    }

    // Stage 04.4 Görev 3:
    // Responsive aspect-preserving scaling across logical container sizes corresponding to
    // 1366x768, 1920x1080, 2560x1440, 3840x2160 at 100%/125%/150%/175%/200% scale factors,
    // plus constrained narrow/tall/wide containers and circular Canvas aspect preservation.
    void responsiveScalingContainersAndLogicalDesktopEvaluation()
    {
        auto serverBus = openPeerBus(QStringLiteral("sm_srv_scale"));
        QVERIFY(serverBus.isConnected());
        MockMonitoringService mock(serverBus);
        QVERIFY(serverBus.registerObject(
            QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
        QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
        const auto cleanup = qScopeGuard([&] {
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
            QDBusConnection::disconnectFromBus(QStringLiteral("sm_srv_scale"));
        });

        auto host = loadMainQmlInWindow();
        QVERIFY(host != nullptr);
        QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));

        auto *podsRow = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, "podsRow"));
        auto *cpuPod = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, "cpuPod"));
        auto *cpuGauge = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, "cpuGauge"));
        auto *storageGauge = qobject_cast<QQuickItem *>(findChildStrict(host->rootItem, "storageGauge"));
        QVERIFY(podsRow && cpuPod && cpuGauge && storageGauge);

        // Matrix of logical widths/heights across 1366x768, 1920x1080, 2560x1440, 3840x2160
        // at 100%, 125%, 150%, 175%, 200% DPI plus wide/tall/upscaled widget containers
        struct Case {
            const char *name;
            int w;
            int h;
            bool expectBelowMin;
        };
        const Case cases[] = {
            {"standard-948x180", 948, 180, false},
            {"1366x768@100pct-desktop-widget", 948, 180, false},
            {"1366x768@125pct-logical-1093x180", 1093, 180, false},
            {"1366x768@150pct-logical-910x180", 910, 180, false},
            {"1366x768@175pct-logical-780x160", 780, 160, false},
            {"1366x768@200pct-logical-683x140", 683, 140, false},
            {"1920x1080@125pct-logical-1536x240", 1536, 240, false},
            {"1920x1080@150pct-logical-1280x220", 1280, 220, false},
            {"1920x1080@175pct-logical-1097x200", 1097, 200, false},
            {"1920x1080@200pct-logical-960x180", 960, 180, false},
            {"2560x1440@150pct-logical-1706x270", 1706, 270, false},
            {"3840x2160@200pct-logical-1920x320", 1920, 320, false},
            {"wide-short-1400x180", 1400, 180, false},
            {"narrow-tall-720x320", 720, 320, false},
            {"min-readable-522x99", 522, 99, false},
            {"sub-minimum-420x80", 420, 80, true},
        };

        for (const Case &c : cases) {
            host->window->resize(c.w, c.h);
            host->rootItem->setSize(QSizeF(c.w, c.h));
            QCoreApplication::processEvents();
            QTest::qWait(15);

            QCOMPARE(host->rootItem->property("belowMinimumReadableSize").toBool(), c.expectBelowMin);
            QVERIFY2(std::abs(cpuPod->width() - cpuPod->height()) < 0.5, c.name);
            QVERIFY2(std::abs(cpuGauge->width() - cpuGauge->height()) < 0.5, c.name);
            QVERIFY2(std::abs(storageGauge->width() - storageGauge->height()) < 0.5, c.name);

            if (!c.expectBelowMin) {
                QVERIFY2(podsRow->width() <= c.w + 1.0, c.name);
                QVERIFY2(podsRow->height() <= c.h + 1.0, c.name);
            } else {
                // Below minimum readable floor: scale clamps cleanly at 0.55 and root clips overflow
                QVERIFY(std::abs(host->rootItem->property("uiScale").toDouble() - 0.55) < 0.01);
                QVERIFY(host->rootItem->clip());
            }
        }
        QVERIFY(host->warnings.isEmpty());
    }

    // Stage 04.4 Görev 4 & 6:
    // Appearance.qml settings reactivity, isolated save/cancel/reload/default persistence,
    // main.xml schema verification, and optional offscreen software screenshot evidence capture.
    void appearanceSettingsPersistenceAndEvidenceCapture()
    {
        // Verify main.xml keeps default=60, min=0, max=100
        const QString mainXmlPath = pathOverrideOrDefault(
            "ZETHROPOL_SYSTEM_MONITOR_MAIN_XML", QStringLiteral(SYSTEM_MONITOR_MAIN_XML));
        const QString appearanceQmlPath = pathOverrideOrDefault(
            "ZETHROPOL_SYSTEM_MONITOR_APPEARANCE_QML", QStringLiteral(SYSTEM_MONITOR_APPEARANCE_QML));
        QFile xmlFile(mainXmlPath);
        QVERIFY(xmlFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString xml = QString::fromUtf8(xmlFile.readAll());
        QVERIFY(xml.contains(QStringLiteral("name=\"backgroundOpacity\"")));
        QVERIFY(xml.contains(QStringLiteral("<default>60</default>")));
        QVERIFY(xml.contains(QStringLiteral("<min>0</min>")));
        QVERIFY(xml.contains(QStringLiteral("<max>100</max>")));

        // Isolated config file in QTemporaryDir (never touches live user Plasma config!)
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString iniPath = tempDir.filePath(QStringLiteral("isolated-system-monitor.ini"));

        auto loadAppearance = [&]() -> std::pair<std::unique_ptr<QQmlEngine>, std::unique_ptr<QQuickWindow>> {
            auto eng = std::make_unique<QQmlEngine>();
            QQmlComponent comp(eng.get(), QUrl::fromLocalFile(appearanceQmlPath));
            if (!comp.isReady()) {
                qWarning() << "Appearance.qml errors:" << comp.errorString();
                return {nullptr, nullptr};
            }
            auto *item = qobject_cast<QQuickItem *>(comp.create());
            if (!item) {
                return {nullptr, nullptr};
            }
            auto win = std::make_unique<QQuickWindow>();
            win->setColor(QColor(QStringLiteral("#f5f7fa")));
            win->resize(540, 170);
            item->setParentItem(win->contentItem());
            item->setX(16);
            item->setSize(QSizeF(508, 170));
            win->show();
            QCoreApplication::processEvents();
            return {std::move(eng), std::move(win)};
        };

        // Step 1: Load default Appearance.qml (60%) + test Cancel discards unsaved change
        {
            QSettings cfg(iniPath, QSettings::IniFormat);
            const int storedInitial = cfg.value(QStringLiteral("General/backgroundOpacity"), 60).toInt();
            QCOMPARE(storedInitial, 60);

            auto [eng, win] = loadAppearance();
            QVERIFY(eng && win);
            QQuickItem *page = win->contentItem()->childItems().first();
            QCOMPARE(page->property("cfg_backgroundOpacityDefault").toInt(), 60);
            QCOMPARE(page->property("cfg_backgroundOpacity").toInt(), 60);
            QCOMPARE(labelText(page, "opacityValueLabel"), QStringLiteral("60 %"));

            // Out-of-range slider clamp checks
            QObject *slider = findChildStrict(page, "opacitySlider");
            QVERIFY(slider != nullptr);
            slider->setProperty("value", -25);
            QCOMPARE(page->property("cfg_backgroundOpacity").toInt(), 0);
            QCOMPARE(labelText(page, "opacityValueLabel"), QStringLiteral("0 %"));
            slider->setProperty("value", 150);
            QCOMPARE(page->property("cfg_backgroundOpacity").toInt(), 100);
            QCOMPARE(labelText(page, "opacityValueLabel"), QStringLiteral("100 %"));

            // Move to 90 and simulate Cancel (do NOT save)
            slider->setProperty("value", 90);
            QCOMPARE(labelText(page, "opacityValueLabel"), QStringLiteral("90 %"));
            QCOMPARE(cfg.value(QStringLiteral("General/backgroundOpacity"), 60).toInt(), 60);

            // Simulate Save (90)
            cfg.setValue(QStringLiteral("General/backgroundOpacity"), page->property("cfg_backgroundOpacity").toInt());
            cfg.sync();
        }

        // Step 2: Reload fresh Appearance.qml from isolated persisted config -> must reflect 90%
        const QString evidenceDir = qEnvironmentVariable("ZETHROPOL_EVIDENCE_DIR");
        {
            QSettings cfg(iniPath, QSettings::IniFormat);
            const int reloadedVal = cfg.value(QStringLiteral("General/backgroundOpacity"), 60).toInt();
            QCOMPARE(reloadedVal, 90);

            auto [eng, win] = loadAppearance();
            QVERIFY(eng && win);
            QQuickItem *page = win->contentItem()->childItems().first();
            page->setProperty("cfg_backgroundOpacity", reloadedVal);
            QCoreApplication::processEvents();
            QCOMPARE(page->property("cfg_backgroundOpacity").toInt(), 90);
            QCOMPARE(labelText(page, "opacityValueLabel"), QStringLiteral("90 %"));

            if (!evidenceDir.isEmpty()) {
                QDir().mkpath(evidenceDir);
                QTest::qWait(60);
                const QImage img = win->grabWindow();
                QVERIFY(!img.isNull());
                QVERIFY(img.save(evidenceDir + QStringLiteral("/07_appearance_settings_page.png")));
            }

            // Simulate Restore Defaults -> returns to 60
            const int defVal = page->property("cfg_backgroundOpacityDefault").toInt();
            page->setProperty("cfg_backgroundOpacity", defVal);
            QCOMPARE(page->property("cfg_backgroundOpacity").toInt(), 60);
            QCOMPARE(labelText(page, "opacityValueLabel"), QStringLiteral("60 %"));
        }

        // Step 3: When ZETHROPOL_EVIDENCE_DIR is provided, capture offscreen software screenshots 1-6
        if (!evidenceDir.isEmpty()) {
            auto serverBus = openPeerBus(QStringLiteral("sm_srv_evid"));
            QVERIFY(serverBus.isConnected());
            MockMonitoringService mock(serverBus);
            QVERIFY(serverBus.registerObject(
                QStringLiteral("/org/zethropol/Monitoring"), &mock, QDBusConnection::ExportAllSlots));
            QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
            const auto cleanup = qScopeGuard([&] {
                serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
                serverBus.unregisterObject(QStringLiteral("/org/zethropol/Monitoring"));
                QDBusConnection::disconnectFromBus(QStringLiteral("sm_srv_evid"));
            });

            auto host = loadMainQmlInWindow();
            QVERIFY(host != nullptr);
            host->window->setColor(QColor(QStringLiteral("#2c3e50"))); // visible desktop backdrop for alpha proof
            QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));
            QTest::qWait(80);

            // 1. Standard 948x180 view (default 60% opacity)
            QVERIFY(host->window->grabWindow().save(
                evidenceDir + QStringLiteral("/01_standard_5pods_948x180.png")));

            // 5. Opacity 0%, 60%, 90%, 100% (individual + stacked comparison sheet)
            QList<QImage> opImages;
            const int opacities[4] = {0, 60, 90, 100};
            for (int op : opacities) {
                host->plasmoidAttached->configObject()->setBackgroundOpacity(op);
                QCoreApplication::processEvents();
                QTest::qWait(60);
                const QImage snap = host->window->grabWindow();
                QVERIFY(!snap.isNull());
                QVERIFY(snap.save(QStringLiteral("%1/05_opacity_%2.png").arg(evidenceDir).arg(op)));
                opImages.append(snap);
            }
            {
                const int w = opImages.first().width();
                const int h = opImages.first().height();
                QImage sheet(w, h * 4 + 24, QImage::Format_ARGB32);
                sheet.fill(QColor(QStringLiteral("#11161d")));
                QPainter p(&sheet);
                for (int i = 0; i < 4; ++i) {
                    p.drawImage(0, i * (h + 8), opImages[i]);
                }
                p.end();
                QVERIFY(sheet.save(evidenceDir + QStringLiteral("/05_opacity_0_60_90_100_sheet.png")));
            }
            host->plasmoidAttached->configObject()->setBackgroundOpacity(90);

            // 2. Synthetic high GPU load (clearly synthetic test data!)
            QVariantMap highGpu = sampleFullMetrics();
            highGpu.insert(QStringLiteral("gpu_usage_percent"), 98.5);
            highGpu.insert(QStringLiteral("gpu_temperature_c"), 76.0);
            highGpu.insert(QStringLiteral("gpu_vram_used_gb"), 3.8);
            highGpu.insert(QStringLiteral("gpu_core_clock_mhz"), 1187.0);
            mock.emitStateChanged(makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), highGpu));
            QTRY_COMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("98.5"));
            QTest::qWait(80);
            QVERIFY(host->window->grabWindow().save(
                evidenceDir + QStringLiteral("/02_synthetic_high_gpu_load.png")));

            // 3. Missing sensors + GPU-less + offline network (degraded)
            QVariantMap missingNetGpu{
                {QStringLiteral("cpu_usage_percent"), 14.2},
                {QStringLiteral("memory_used_gb"), 6.4},
                {QStringLiteral("memory_total_gb"), 32.0},
                {QStringLiteral("storage_filesystem_usage_percent"), 51.83},
                {QStringLiteral("storage_filesystem_used_gb"), 255.84},
                {QStringLiteral("storage_filesystem_total_gb"), 495.81},
            };
            mock.emitStateChanged(makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("degraded"), missingNetGpu));
            QTRY_COMPARE(labelText(host->rootItem, "gpuUsageLabel"), QStringLiteral("—"));
            QTest::qWait(80);
            QVERIFY(host->window->grabWindow().save(
                evidenceDir + QStringLiteral("/03_missing_sensors_offline_network.png")));

            // 4. Service unavailable
            serverBus.unregisterService(QStringLiteral("org.zethropol.Monitoring"));
            QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("—"));
            QTest::qWait(80);
            QVERIFY(host->window->grabWindow().save(
                evidenceDir + QStringLiteral("/04_service_unavailable.png")));

            // 6. Narrow container (1366x768 @ 200% logical width -> 683x140) with live data restored
            mock.nextReply = makeEnvelope(
                true, QStringLiteral("ok"), QString(), QStringLiteral("ready"), sampleFullMetrics());
            QVERIFY(serverBus.registerService(QStringLiteral("org.zethropol.Monitoring")));
            QTRY_COMPARE(labelText(host->rootItem, "cpuUsageLabel"), QStringLiteral("24.5"));
            host->window->resize(683, 140);
            host->rootItem->setSize(QSizeF(683, 140));
            QCoreApplication::processEvents();
            QTest::qWait(100);
            QVERIFY(host->window->grabWindow().save(
                evidenceDir + QStringLiteral("/06_narrow_container_683x140.png")));
        }
    }
};

QTEST_MAIN(SystemMonitorQmlTest)
#include "SystemMonitorQmlTest.moc"
