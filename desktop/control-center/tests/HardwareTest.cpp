#include "HardwareServiceClient.h"
#include "UpdateScanner.h"
#include "UpdateInstaller.h"
#include "RecoveryService.h"
#include "SecurityService.h"
#include "ServiceService.h"
#include "ApplicationService.h"
#include "NotificationService.h"
#include "core/zethropol_power_client.h"
#include "core/zethropol_monitoring_client.h"
#include <QDBusConnectionInterface>
#include <QFile>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

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
        QVERIFY2(QFile::exists(HARDWARE_BINARY), "Build services/hardware with cargo build --offline --locked first");
        service.setProgram(HARDWARE_BINARY);
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
        NotificationService notifications;
        ZethropolPowerClient power;
        ZethropolMonitoringClient monitoring;
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {"HardwareBridge", &client}, {"UpdateBridge", &scanner}, {"UpdateInstallBridge", &installer},
                 {"RecoveryBridge", &recovery}, {"SecurityBridge", &security}, {"ServiceBridge", &services},
                 {"ApplicationBridge", &applications}, {"NotificationBridge", &notifications},
                 {"PowerBridge", &power}, {"MonitoringBridge", &monitoring}})
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>("controlCenterPages");
        QVERIFY(pages);
        QCOMPARE(pages->property("count").toInt(), 11);
        for (int index = 0; index < 11; ++index) {
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
