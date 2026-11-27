#include "HardwareServiceClient.h"
#include "UpdateScanner.h"
#include "UpdateInstaller.h"
#include "RecoveryService.h"
#include "SecurityService.h"
#include "ServiceService.h"
#include "ApplicationService.h"
#include "ProcessService.h"
#include "NotificationService.h"
#include "CompatibilityManager.h"
#include "core/zethropol_power_client.h"
#include "core/zethropol_monitoring_client.h"
#include <QDBusConnectionInterface>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {
QMutex g_capturedMessagesMutex;
QStringList *g_capturedMessages = nullptr;
QtMessageHandler g_previousMessageHandler = nullptr;

void captureQtMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&g_capturedMessagesMutex);
        if (g_capturedMessages) {
            const QString category = context.category ? QString::fromUtf8(context.category) : QString();
            g_capturedMessages->append(category.isEmpty() ? message : (category + QStringLiteral(": ") + message));
        }
    }
    if (g_previousMessageHandler)
        g_previousMessageHandler(type, context, message);
}

bool writeTextFile(const QString &path, const QString &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    QTextStream out(&file);
    out << content;
    return file.flush();
}
}

class HardwareFixture : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Hardware.v1")
public:
    QVariantMap data;
    bool success = true;
public slots:
    QVariantMap GetState() {
        return {{"success", success}, {"code", success ? "ok" : "failed"},
                {"message", success ? "" : "sensor read failed"}, {"operationId", ""}, {"data", data}};
    }
};

class PerformanceFixture : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Performance.v1")
public:
    int calls = 0;
public slots:
    QVariantMap GetState() {
        ++calls;
        return {{"success", true}, {"code", "ok"}, {"data", QVariantMap{{"cpu_usage_percent", 42.0}}}};
    }
};

class MonitoringFixture : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Monitoring.v1")
public:
    bool ok = true;
public slots:
    QVariantMap GetState() {
        if (!ok)
            return {{"success", false}, {"code", "failed"}, {"message", "monitoring unavailable"}};
        return {{"success", true}, {"code", "ok"}, {"data", QVariantMap{{"cpu_usage_percent", 27.5}}}};
    }
};

class PowerFixture : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Power.v1")
public:
    QVariantMap stateData{
        {QStringLiteral("available"), true},
        {QStringLiteral("profile"), QStringLiteral("balanced")},
        {QStringLiteral("governor"), QStringLiteral("schedutil")}
    };
public slots:
    QVariantMap GetState() {
        return {
            {QStringLiteral("success"), true},
            {QStringLiteral("code"), QStringLiteral("ok")},
            {QStringLiteral("message"), QString()},
            {QStringLiteral("operationId"), QString()},
            {QStringLiteral("data"), stateData}
        };
    }
signals:
    void StateChanged(const QVariantMap &state);
};

class HardwareTest : public QObject {
    Q_OBJECT
    QProcess service;
    HardwareFixture fixture;
    bool registered = false;

    void registerFixture() {
        auto bus = QDBusConnection::systemBus();
        QVERIFY(bus.registerObject("/org/zethropol/Hardware", &fixture, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerService("org.zethropol.Hardware"));
        registered = true;
    }
    QVariantMap hybridData() {
        return {{"cpu", QVariantMap{{"model", "Fixture CPU"}, {"cores", 6}, {"threads", 12}}},
            {"memory", QVariantMap{{"total_gb", 32.0}}},
            {"gpu", QVariantMap{{"detected", true}, {"model", "Fixture Intel"}}},
            {"gpus", QVariantList{QVariantMap{{"model", "Fixture Intel"}, {"card", "card0"}, {"driver", "i915"}},
                                 QVariantMap{{"model", "Fixture NVIDIA"}, {"card", "card1"}, {"driver", "nvidia"}}}},
            {"motherboard", QVariantMap{{"model", "Fixture Board"}}}};
    }
    void startRealService() {
        // The binary path may be overridden (e.g. when target/ is read-only).
        const QString binary = qEnvironmentVariableIsSet("ZETHROPOL_HARDWARE_SERVICE_BINARY")
            ? qEnvironmentVariable("ZETHROPOL_HARDWARE_SERVICE_BINARY")
            : QStringLiteral(HARDWARE_BINARY);
        QVERIFY2(QFile::exists(binary), "Build services/hardware with cargo build --offline --locked first");
        service.setProgram(binary);
        service.setArguments({"--dbus-service"});
        service.start();
        QVERIFY(service.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QDBusConnection::systemBus().interface()->isServiceRegistered("org.zethropol.Hardware").value(), 5000);
    }

private slots:
    void initTestCase() {
        QVERIFY(QDBusConnection::systemBus().isConnected());
        // Tests must never talk to the installed system services.
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"), qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(!qgetenv("DBUS_SYSTEM_BUS_ADDRESS").isEmpty());
        qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/SystemState.qml")),
                                "Zethropol.ControlCenter", 1, 0, "SystemState");
    }
    void cleanup() {
        if (registered) {
            QDBusConnection::systemBus().unregisterService("org.zethropol.Hardware");
            QDBusConnection::systemBus().unregisterObject("/org/zethropol/Hardware");
            registered = false;
        }
        if (service.state() != QProcess::NotRunning) {
            service.terminate();
            if (!service.waitForFinished(3000)) { service.kill(); service.waitForFinished(); }
        }
    }
    void refreshErrorAndRecovery() {
        HardwareServiceClient client;
        QTRY_VERIFY(!client.error().isEmpty());
        QVERIFY(!client.loading());
        QVERIFY(!client.available());
        fixture.data = hybridData();
        fixture.success = true;
        registerFixture();
        client.refresh();
        QTRY_VERIFY(client.available());
        QCOMPARE(client.state()["gpus"].toList().size(), 2);
        QCOMPARE(client.state()["gpus"].toList()[1].toMap()["model"].toString(), "Fixture NVIDIA");
        QVERIFY(!client.lastUpdated().isEmpty());
        fixture.success = false;
        client.refresh();
        QTRY_VERIFY(!client.loading());
        QVERIFY(!client.available());
        QVERIFY(client.state().isEmpty());
        QCOMPARE(client.error(), "sensor read failed");
        fixture.success = true;
        fixture.data["cpu"] = QVariantMap{{"model", "Refreshed CPU"}};
        // The automatic poll must recover without a manual request.
        QTRY_VERIFY_WITH_TIMEOUT(client.available(), 12000);
        QCOMPARE(client.state()["cpu"].toMap()["model"].toString(), "Refreshed CPU");
    }
    void hardwareAndPerformanceUseStandardSystemBus() {
        fixture.data = hybridData();
        fixture.success = true;
        registerFixture();
        auto bus = QDBusConnection::systemBus();
        const auto name = bus.baseService();
        PerformanceFixture performance;
        QVERIFY(bus.registerObject("/org/zethropol/Performance", &performance, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerService("org.zethropol.Performance"));
        MonitoringFixture monitoring;
        QVERIFY(bus.registerObject("/org/zethropol/Monitoring", &monitoring, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerService("org.zethropol.Monitoring"));
        const auto cleanupServices = qScopeGuard([&] {
            bus.unregisterService("org.zethropol.Performance");
            bus.unregisterObject("/org/zethropol/Performance");
            bus.unregisterService("org.zethropol.Monitoring");
            bus.unregisterObject("/org/zethropol/Monitoring");
        });
        HardwareServiceClient client;
        ZethropolMonitoringClient monClient;
        QTRY_VERIFY(client.available());
        QCOMPARE(client.state()["cpu"].toMap()["model"].toString(), "Fixture CPU");
        QTRY_VERIFY(monClient.available());
        QCOMPARE(monClient.state()["cpu_usage_percent"].toDouble(), 27.5);
        // HardwareServiceClient must NEVER poll quarantined org.zethropol.Performance.
        QTest::qWait(1200);
        QCOMPARE(performance.calls, 0);
        // Monitoring disconnect clears stale state and reports unavailability cleanly.
        monitoring.ok = false;
        QTRY_VERIFY_WITH_TIMEOUT(!monClient.available(), 3000);
        QVERIFY(monClient.state().isEmpty());
        QVERIFY(!monClient.error().isEmpty());
        QCOMPARE(QDBusConnection::systemBus().baseService(), name);
    }
    void realServiceAndAllTabs() {
        startRealService();
        HardwareServiceClient client;
        QTRY_VERIFY_WITH_TIMEOUT(client.available(), 10000);
        const auto state = client.state();
        QVERIFY(state["cpu"].toMap()["threads"].toUInt() > 0);
        QVERIFY(state["memory"].toMap()["total_gb"].toDouble() > 0);
        QFile cpuinfo("/proc/cpuinfo");
        QVERIFY(cpuinfo.open(QIODevice::ReadOnly));
        QVERIFY(cpuinfo.readAll().contains(state["cpu"].toMap()["model"].toString().toUtf8()));
        QFile board("/sys/class/dmi/id/board_name");
        if (board.open(QIODevice::ReadOnly))
            QCOMPARE(state["motherboard"].toMap()["model"].toString(), QString::fromUtf8(board.readAll()).trimmed());
        const auto disk = state["normalized_storage"].toMap();
        if (disk["detected"].toBool()) {
            const auto name = disk["device_path"].toString().section('/', -1);
            QFile size("/sys/class/block/" + name + "/size");
            QVERIFY(size.open(QIODevice::ReadOnly));
            QCOMPARE(disk["capacity_gb"].toDouble(), size.readAll().trimmed().toULongLong() * 512.0 / 1e9);
        }
        for (const auto &entry : state["gpus"].toList()) {
            const auto gpu = entry.toMap();
            const auto path = "/sys/class/drm/" + gpu["card"].toString() + "/device/vendor";
            QFile vendor(path);
            if (vendor.open(QIODevice::ReadOnly))
                QCOMPARE(gpu["vendor"].toString(), QString::fromUtf8(vendor.readAll()).trimmed().remove("0x"));
        }
        QFile report("/tmp/zethropol-hardware-live-bridge.json");
        QVERIFY(report.open(QIODevice::WriteOnly));
        report.write(QJsonDocument::fromVariant(state).toJson());
        report.close();
        exerciseAllTabs(client, true);
        QTemporaryDir emptyTools;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("PATH", emptyTools.path());
        QProcess application;
        application.setProcessEnvironment(environment);
        application.start(QStringLiteral(CONTROL_CENTER_BINARY), QStringList{});
        QVERIFY(application.waitForStarted());
        QTest::qWait(1500);
        QCOMPARE(application.state(), QProcess::Running);
        application.terminate();
        QVERIFY(application.waitForFinished(3000));
        QVERIFY(!application.readAllStandardError().contains("failed to load component"));
    }
    void missingSensorsAndHybridQml() {
        fixture.data = hybridData();
        fixture.success = true;
        registerFixture();
        HardwareServiceClient client;
        QTRY_VERIFY(client.available());
        exerciseAllTabs(client, false);
    }
    void systemInfoNormalizationAndFormatting() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString rollingOsRelease = tempDir.filePath(QStringLiteral("os-release-rolling"));
        QVERIFY(writeTextFile(rollingOsRelease, QStringLiteral(
            "NAME=\"Zethropol Linux\"\n"
            "ID=cachyos\n"
            "ID_LIKE=arch\n"
            "BUILD_ID=rolling\n")));

        const QString versionedOsRelease = tempDir.filePath(QStringLiteral("os-release-versioned"));
        QVERIFY(writeTextFile(versionedOsRelease, QStringLiteral(
            "NAME=\"Zethropol Linux\"\n"
            "VERSION_ID=\"2026.11\"\n"
            "BUILD_ID=rolling\n")));

        const QString plasmaDesktop = tempDir.filePath(QStringLiteral("plasma.desktop"));
        QVERIFY(writeTextFile(plasmaDesktop, QStringLiteral(
            "[Desktop Entry]\n"
            "Type=XSession\n"
            "Name=Plasma (Wayland)\n"
            "X-KDE-PluginInfo-Version=6.8.3\n")));

        const QString missingDesktop = tempDir.filePath(QStringLiteral("missing.desktop"));

        QProcessEnvironment kdeEnv;
        kdeEnv.insert(QStringLiteral("XDG_CURRENT_DESKTOP"), QStringLiteral("KDE"));
        kdeEnv.insert(QStringLiteral("KDE_SESSION_VERSION"), QStringLiteral("6"));
        kdeEnv.insert(QStringLiteral("KDE_FULL_SESSION"), QStringLiteral("true"));
        kdeEnv.insert(QStringLiteral("XDG_SESSION_TYPE"), QStringLiteral("wayland"));
        kdeEnv.insert(QStringLiteral("DESKTOP_SESSION"), plasmaDesktop);

        const QVariantMap unknownSystem{
            {QStringLiteral("os_name"), QStringLiteral("Zethropol Linux")},
            {QStringLiteral("os_version"), QStringLiteral("Unknown")},
            {QStringLiteral("desktop_name"), QStringLiteral("Unknown")},
            {QStringLiteral("desktop_version"), QStringLiteral("Unknown")},
            {QStringLiteral("session_type"), QStringLiteral("Unknown")}
        };

        // Raw values must be preserved without pre-formatting.
        const QVariantMap normalizedRolling = HardwareServiceClient::normalizeSystemInfo(
            unknownSystem, kdeEnv, rollingOsRelease, {plasmaDesktop});
        QCOMPARE(normalizedRolling.value(QStringLiteral("os_version")).toString(), QStringLiteral("rolling"));
        QCOMPARE(normalizedRolling.value(QStringLiteral("desktop_name")).toString(), QStringLiteral("KDE"));
        QCOMPARE(normalizedRolling.value(QStringLiteral("desktop_version")).toString(), QStringLiteral("6.8.3"));
        QCOMPARE(normalizedRolling.value(QStringLiteral("session_type")).toString(), QStringLiteral("wayland"));

        // VERSION_ID takes precedence over BUILD_ID.
        const QVariantMap normalizedVersioned = HardwareServiceClient::normalizeSystemInfo(
            unknownSystem, kdeEnv, versionedOsRelease, {plasmaDesktop});
        QCOMPARE(normalizedVersioned.value(QStringLiteral("os_version")).toString(), QStringLiteral("2026.11"));

        // Falls back to KDE_SESSION_VERSION when desktop file lacks X-KDE-PluginInfo-Version.
        const QVariantMap normalizedFallback = HardwareServiceClient::normalizeSystemInfo(
            unknownSystem, kdeEnv, rollingOsRelease, {missingDesktop});
        QCOMPARE(normalizedFallback.value(QStringLiteral("desktop_version")).toString(), QStringLiteral("6"));

        // Existing service values must not be overwritten.
        const QVariantMap populatedSystem{
            {QStringLiteral("os_version"), QStringLiteral("47")},
            {QStringLiteral("desktop_name"), QStringLiteral("GNOME")},
            {QStringLiteral("desktop_version"), QStringLiteral("47.1")},
            {QStringLiteral("session_type"), QStringLiteral("x11")}
        };
        const QVariantMap normalizedPopulated = HardwareServiceClient::normalizeSystemInfo(
            populatedSystem, kdeEnv, rollingOsRelease, {plasmaDesktop});
        QCOMPARE(normalizedPopulated.value(QStringLiteral("os_version")).toString(), QStringLiteral("47"));
        QCOMPARE(normalizedPopulated.value(QStringLiteral("desktop_name")).toString(), QStringLiteral("GNOME"));
        QCOMPARE(normalizedPopulated.value(QStringLiteral("desktop_version")).toString(), QStringLiteral("47.1"));
        QCOMPARE(normalizedPopulated.value(QStringLiteral("session_type")).toString(), QStringLiteral("x11"));

        // Missing sources must never invent hardcoded defaults.
        const QProcessEnvironment emptyEnv;
        const QVariantMap normalizedEmpty = HardwareServiceClient::normalizeSystemInfo(
            unknownSystem, emptyEnv, tempDir.filePath(QStringLiteral("nonexistent-os-release")), {missingDesktop});
        QCOMPARE(normalizedEmpty.value(QStringLiteral("os_version")).toString(), QStringLiteral("Unknown"));
        QCOMPARE(normalizedEmpty.value(QStringLiteral("desktop_name")).toString(), QStringLiteral("Unknown"));
        QCOMPARE(normalizedEmpty.value(QStringLiteral("desktop_version")).toString(), QStringLiteral("Unknown"));
        QCOMPARE(normalizedEmpty.value(QStringLiteral("session_type")).toString(), QStringLiteral("Unknown"));

        // End-to-end UI formatting check via System tab in Main.qml.
        const QList<QByteArray> envKeys = {
            "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "KDE_SESSION_VERSION",
            "KDE_FULL_SESSION", "XDG_SESSION_TYPE", "DESKTOP_SESSION",
            "WAYLAND_DISPLAY", "DISPLAY", "PATH"
        };
        QHash<QByteArray, QPair<bool, QByteArray>> savedEnv;
        for (const QByteArray &key : envKeys)
            savedEnv.insert(key, {qEnvironmentVariableIsSet(key.constData()), qgetenv(key.constData())});
        const auto restoreEnv = qScopeGuard([&] {
            for (auto it = savedEnv.cbegin(); it != savedEnv.cend(); ++it) {
                if (it.value().first)
                    qputenv(it.key().constData(), it.value().second);
                else
                    qunsetenv(it.key().constData());
            }
        });

        QTemporaryDir emptyTools;
        QVERIFY(emptyTools.isValid());
        qputenv("PATH", emptyTools.path().toUtf8());
        qputenv("XDG_CURRENT_DESKTOP", "KDE");
        qunsetenv("XDG_SESSION_DESKTOP");
        qputenv("KDE_SESSION_VERSION", "6");
        qputenv("KDE_FULL_SESSION", "true");
        qputenv("XDG_SESSION_TYPE", "wayland");
        qputenv("DESKTOP_SESSION", plasmaDesktop.toUtf8());

        fixture.data = hybridData();
        fixture.data.insert(QStringLiteral("system"), QVariantMap{
            {QStringLiteral("os_name"), QStringLiteral("Zethropol Linux")},
            {QStringLiteral("os_version"), QStringLiteral("rolling")},
            {QStringLiteral("base_system"), QStringLiteral("arch")},
            {QStringLiteral("architecture"), QStringLiteral("x86_64")},
            {QStringLiteral("kernel_version"), QStringLiteral("6.19.0-test")},
            {QStringLiteral("desktop_name"), QStringLiteral("Unknown")},
            {QStringLiteral("desktop_version"), QStringLiteral("Unknown")},
            {QStringLiteral("session_type"), QStringLiteral("Unknown")},
            {QStringLiteral("hostname"), QStringLiteral("zethropol-test")},
            {QStringLiteral("uptime_seconds"), 3660}
        });
        fixture.success = true;
        registerFixture();

        HardwareServiceClient client;
        QTRY_VERIFY(client.available());
        const QVariantMap rawSystem = client.state().value(QStringLiteral("system")).toMap();
        QCOMPARE(rawSystem.value(QStringLiteral("os_version")).toString(), QStringLiteral("rolling"));
        QCOMPARE(rawSystem.value(QStringLiteral("desktop_name")).toString(), QStringLiteral("KDE"));
        QCOMPARE(rawSystem.value(QStringLiteral("desktop_version")).toString(), QStringLiteral("6.8.3"));
        QCOMPARE(rawSystem.value(QStringLiteral("session_type")).toString(), QStringLiteral("wayland"));

        UpdateScanner scanner;
        UpdateInstaller installer(&scanner);
        RecoveryService recovery;
        SecurityService security;
        ServiceService services;
        ApplicationService applications;
        ProcessService processes;
        NotificationService notifications;
        ZethropolPowerClient power;
        ZethropolMonitoringClient monitoring;
        CompatibilityManager compatibility;
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {"HardwareBridge", &client}, {"UpdateBridge", &scanner}, {"UpdateInstallBridge", &installer},
                 {"RecoveryBridge", &recovery}, {"SecurityBridge", &security}, {"ServiceBridge", &services},
                 {"ApplicationBridge", &applications}, {"ProcessBridge", &processes},
                 {"NotificationBridge", &notifications},
                 {"PowerBridge", &power}, {"MonitoringBridge", &monitoring},
                 {"CompatibilityBridge", &compatibility}})
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>(QStringLiteral("controlCenterPages"));
        QVERIFY(pages);
        QVERIFY(pages->setProperty("currentIndex", 3));
        QTest::qWait(60);

        const auto pageItems = pages->childItems();
        QVERIFY(pageItems.size() > 3);
        auto *systemPage = pageItems.at(3);
        QVERIFY(systemPage);

        auto collectPageTexts = [this](QQuickItem *page) {
            QString result;
            for (const auto *item : visualItems(page)) {
                const QVariant text = item->property("text");
                if (text.isValid() && !text.toString().isEmpty())
                    result += text.toString() + QLatin1Char('\n');
            }
            return result;
        };

        const QString kdeTexts = collectPageTexts(systemPage);
        QVERIFY2(kdeTexts.contains(QStringLiteral("Version: Rolling")), qPrintable(kdeTexts));
        QVERIFY2(kdeTexts.contains(QStringLiteral("KDE Plasma")), qPrintable(kdeTexts));
        QVERIFY2(kdeTexts.contains(QStringLiteral("Plasma Version: 6.8.3")), qPrintable(kdeTexts));
        QVERIFY2(kdeTexts.contains(QStringLiteral("Session: Wayland")), qPrintable(kdeTexts));

        // Non-KDE desktop should keep "Version:" label and format X11 cleanly.
        fixture.data.insert(QStringLiteral("system"), QVariantMap{
            {QStringLiteral("os_name"), QStringLiteral("Zethropol Linux")},
            {QStringLiteral("os_version"), QStringLiteral("47")},
            {QStringLiteral("base_system"), QStringLiteral("arch")},
            {QStringLiteral("architecture"), QStringLiteral("x86_64")},
            {QStringLiteral("kernel_version"), QStringLiteral("6.19.0-test")},
            {QStringLiteral("desktop_name"), QStringLiteral("GNOME")},
            {QStringLiteral("desktop_version"), QStringLiteral("47.1")},
            {QStringLiteral("session_type"), QStringLiteral("x11")},
            {QStringLiteral("hostname"), QStringLiteral("zethropol-test")},
            {QStringLiteral("uptime_seconds"), 3660}
        });
        client.refresh();
        QTRY_COMPARE(client.state().value(QStringLiteral("system")).toMap().value(QStringLiteral("desktop_name")).toString(),
                     QStringLiteral("GNOME"));
        QTest::qWait(50);

        const QString gnomeTexts = collectPageTexts(systemPage);
        QVERIFY2(gnomeTexts.contains(QStringLiteral("Version: 47")), qPrintable(gnomeTexts));
        QVERIFY2(gnomeTexts.contains(QStringLiteral("GNOME")), qPrintable(gnomeTexts));
        QVERIFY2(gnomeTexts.contains(QStringLiteral("Version: 47.1")), qPrintable(gnomeTexts));
        QVERIFY2(!gnomeTexts.contains(QStringLiteral("Plasma Version:")), qPrintable(gnomeTexts));
        QVERIFY2(gnomeTexts.contains(QStringLiteral("Session: X11")), qPrintable(gnomeTexts));

        QStringList errors;
        for (const auto &signal : warnings)
            for (const auto &warning : qvariant_cast<QList<QQmlError>>(signal.first()))
                errors.append(warning.toString());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    }
    void powerClientStateChangedSignal() {
        QStringList capturedMessages;
        {
            QMutexLocker locker(&g_capturedMessagesMutex);
            g_capturedMessages = &capturedMessages;
            g_previousMessageHandler = qInstallMessageHandler(captureQtMessages);
        }
        const auto restoreHandler = qScopeGuard([&] {
            qInstallMessageHandler(g_previousMessageHandler);
            QMutexLocker locker(&g_capturedMessagesMutex);
            g_capturedMessages = nullptr;
            g_previousMessageHandler = nullptr;
        });

        auto bus = QDBusConnection::systemBus();
        PowerFixture powerFixture;
        QVERIFY(bus.registerObject(
            QStringLiteral("/org/zethropol/Power"),
            &powerFixture,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(bus.registerService(QStringLiteral("org.zethropol.Power")));
        const auto cleanupPower = qScopeGuard([&] {
            bus.unregisterService(QStringLiteral("org.zethropol.Power"));
            bus.unregisterObject(QStringLiteral("/org/zethropol/Power"));
        });

        ZethropolPowerClient powerClient;
        QVERIFY(powerClient.signalConnected());
        QCOMPARE(powerClient.state().value(QStringLiteral("profile")).toString(), QStringLiteral("balanced"));
        QCOMPARE(powerClient.state().value(QStringLiteral("governor")).toString(), QStringLiteral("schedutil"));

        QSignalSpy stateChangedSpy(&powerClient, &ZethropolPowerClient::powerStateChanged);
        emit powerFixture.StateChanged(QVariantMap{
            {QStringLiteral("available"), true},
            {QStringLiteral("profile"), QStringLiteral("performance")},
            {QStringLiteral("governor"), QStringLiteral("performance")}
        });
        QTRY_COMPARE(stateChangedSpy.count(), 1);
        QCOMPARE(powerClient.state().value(QStringLiteral("profile")).toString(), QStringLiteral("performance"));
        QCOMPARE(powerClient.state().value(QStringLiteral("governor")).toString(), QStringLiteral("performance"));

        // Empty signal payload must not overwrite valid state.
        emit powerFixture.StateChanged(QVariantMap{});
        QTest::qWait(100);
        QCOMPARE(stateChangedSpy.count(), 1);
        QCOMPARE(powerClient.state().value(QStringLiteral("profile")).toString(), QStringLiteral("performance"));

        QStringList dbusWarnings;
        {
            QMutexLocker locker(&g_capturedMessagesMutex);
            for (const QString &msg : capturedMessages) {
                if (msg.contains(QStringLiteral("Could not connect"))
                    || msg.contains(QStringLiteral("Type not registered with QtDBus"))
                    || msg.contains(QStringLiteral("handleDbusSignal"))
                    || msg.contains(QStringLiteral("org.zethropol.Power.v1"))) {
                    dbusWarnings.append(msg);
                }
            }
        }
        QVERIFY2(dbusWarnings.isEmpty(), qPrintable(dbusWarnings.join(QLatin1Char('\n'))));
    }

private:
    QList<QQuickItem *> visualItems(QQuickItem *root) {
        QList<QQuickItem *> result;
        for (auto *item : root->childItems()) {
            result.append(item);
            result.append(visualItems(item));
        }
        return result;
    }
    void exerciseAllTabs(HardwareServiceClient &client, bool live) {
        // Prevent unrelated update scan tools from running or contacting the
        // network. The already running Hardware process retains its real PATH.
        QTemporaryDir emptyTools;
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", emptyTools.path().toUtf8());
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
        UpdateScanner scanner;
        UpdateInstaller installer(&scanner);
        RecoveryService recovery;
        SecurityService security;
        ServiceService services;
        ApplicationService applications;
        ProcessService processes;
        NotificationService notifications;
        ZethropolPowerClient power;
        ZethropolMonitoringClient monitoring;
        CompatibilityManager compatibility;
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {"HardwareBridge", &client}, {"UpdateBridge", &scanner}, {"UpdateInstallBridge", &installer},
                 {"RecoveryBridge", &recovery}, {"SecurityBridge", &security}, {"ServiceBridge", &services},
                 {"ApplicationBridge", &applications}, {"ProcessBridge", &processes},
                 {"NotificationBridge", &notifications},
                 {"PowerBridge", &power}, {"MonitoringBridge", &monitoring},
                 {"CompatibilityBridge", &compatibility}})
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>("controlCenterPages");
        QVERIFY(pages);
        // Overview, Hardware, Diagnostics, System, Performance, Power, Updates,
        // Recovery, Security, Services, Applications, Processes, Compatibility.
        QCOMPARE(pages->property("count").toInt(), 13);
        for (int index = 0; index < 13; ++index) {
            QVERIFY(pages->setProperty("currentIndex", index));
            QTest::qWait(35);
            QCOMPARE(pages->property("currentIndex").toInt(), index);
        }
        pages->setProperty("currentIndex", 1);
        QTest::qWait(100);
        auto *page = window->findChild<QQuickItem *>("hardwarePage");
        QVERIFY(page);
        const auto items = visualItems(page);
        int cardCount = 0;
        for (const auto *item : items)
            if (item->objectName() == "hardwareCard") ++cardCount;
        QVERIFY2(cardCount >= 9, qPrintable(QString("Only %1 visual cards").arg(cardCount)));
        if (!live) {
            QString texts;
            for (const auto *item : items)
                texts += item->property("text").toString() + '\n';
            QVERIFY(texts.contains("Fixture Intel"));
            QVERIFY(texts.contains("Fixture NVIDIA"));
            QVERIFY(texts.contains("Temperature: Unavailable"));
            QVERIFY(!texts.contains("NaN"));
            QVERIFY(!texts.contains("undefined"));
        }
        // The page controls must stay inside the viewport when the cards are
        // scrolled, including in a smaller window. Checking visible alone would
        // miss items clipped by a Flickable ancestor.
        auto fullyVisible = [window](QQuickItem *item) {
            const auto bounds = item->mapRectToScene(item->boundingRect());
            QRectF visible = bounds.intersected(QRectF(0, 0, window->width(), window->height()));
            for (auto *parent = item->parentItem(); parent; parent = parent->parentItem())
                if (parent->clip())
                    visible = visible.intersected(parent->mapRectToScene(parent->boundingRect()));
            return item->isVisible() && visible.width() >= bounds.width() - 0.5
                && visible.height() >= bounds.height() - 0.5;
        };
        auto *scroll = window->findChild<QQuickItem *>("hardwareScroll");
        QVERIFY(scroll);
        auto *flickable = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(flickable);
        const QSize originalSize = window->size();
        for (const auto &size : {QSize(1280, 800), QSize(900, 600)}) {
            window->resize(size);
            QTest::qWait(50);
            const qreal bottom = qMax(0.0, flickable->property("contentHeight").toDouble() - flickable->height());
            QVERIFY(flickable->setProperty("contentY", bottom));
            QTest::qWait(30);
            for (const auto &name : {"hardwareTitle", "hardwareRefresh", "hardwareStatus"}) {
                auto *control = window->findChild<QQuickItem *>(name);
                QVERIFY(control);
                QVERIFY2(fullyVisible(control), name);
            }
        }
        window->resize(originalSize);
        QVERIFY(flickable->setProperty("contentY", 0));
        QTest::qWait(50);
        // Capture the same source page rendered in the development application.
        if (live) {
            const auto capture = window->grabWindow();
            QVERIFY(!capture.isNull());
            QVERIFY(capture.save("/tmp/zethropol-hardware-live.png"));
        }
        QStringList errors;
        for (const auto &signal : warnings)
            for (const auto &warning : qvariant_cast<QList<QQmlError>>(signal.first()))
                errors.append(warning.toString());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    }
};

QTEST_MAIN(HardwareTest)
#include "HardwareTest.moc"
