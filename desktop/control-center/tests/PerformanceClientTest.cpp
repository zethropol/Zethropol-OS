#include "PerformanceClient.h"

#include <QDBusConnection>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

namespace {

QString collectTexts(QQuickItem *root)
{
    if (!root)
        return {};
    QString out;
    const QVariant t = root->property("text");
    if (t.isValid() && t.typeId() == QMetaType::QString && !t.toString().isEmpty())
        out += t.toString() + QLatin1Char('\n');
    for (QQuickItem *child : root->childItems())
        out += collectTexts(child);
    return out;
}

class MockMonitoringState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
public:
    QVariantMap state() const { return m_state; }
signals:
    void stateChanged();
private:
    QVariantMap m_state{
        {QStringLiteral("cpu_usage_percent"), 21.5},
        {QStringLiteral("gpu_vram_used_gb"), 1.25},
        {QStringLiteral("gpu_vram_total_gb"), 4.0},
        {QStringLiteral("storage_used_gb"), 120.0},
        {QStringLiteral("storage_capacity_gb"), 512.0}
    };
};

class MockPerformanceService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Performance.v1")
public:
    QString activeProfile = QStringLiteral("general");
    QString cpuGovernor = QStringLiteral("schedutil");
    QString gpuDpm = QStringLiteral("auto");
    bool thermalGuard = false;
    bool gamingSession = true;
    int setCalls = 0;

    QVariantMap makeProfileData() const
    {
        const QString profilesJson = QStringLiteral(
            "[{\"id\":\"general\",\"title\":\"General\",\"description\":\"Varsayılan dengeli\",\"target_governor\":\"schedutil\",\"target_gpu_dpm\":\"auto\",\"thermal_guard_applies\":false,\"allowed_now\":true,\"blocked_reason\":\"\"},"
            "{\"id\":\"gaming\",\"title\":\"Gaming\",\"description\":\"Düşük gecikme oyun profili\",\"target_governor\":\"performance\",\"target_gpu_dpm\":\"high\",\"thermal_guard_applies\":true,\"allowed_now\":%1,\"blocked_reason\":\"%2\"},"
            "{\"id\":\"developer\",\"title\":\"Developer\",\"description\":\"Derleme ve paralel iş yükü\",\"target_governor\":\"performance\",\"target_gpu_dpm\":\"auto\",\"thermal_guard_applies\":false,\"allowed_now\":true,\"blocked_reason\":\"\"},"
            "{\"id\":\"creator\",\"title\":\"Creator\",\"description\":\"Medya/render sürekliliği\",\"target_governor\":\"schedutil\",\"target_gpu_dpm\":\"auto\",\"thermal_guard_applies\":false,\"allowed_now\":true,\"blocked_reason\":\"\"}]")
            .arg(thermalGuard ? QStringLiteral("false") : QStringLiteral("true"),
                 thermalGuard ? QStringLiteral("GPU sıcaklığı 84.0 °C sınırını aştı.") : QString());

        return QVariantMap{
            {QStringLiteral("active_profile"), activeProfile},
            {QStringLiteral("default_profile"), QStringLiteral("general")},
            {QStringLiteral("cpu_governor"), cpuGovernor},
            {QStringLiteral("available_governors"), QStringList{QStringLiteral("schedutil"), QStringLiteral("performance")}},
            {QStringLiteral("gpu_dpm_level"), gpuDpm},
            {QStringLiteral("thermal_guard_active"), thermalGuard},
            {QStringLiteral("thermal_guard_reason"), thermalGuard
                ? QStringLiteral("Termal koruma etkin: GPU sıcaklığı 84.0 °C sınırını aştı.")
                : QStringLiteral("Termal sınırlar güvenli aralıkta.")},
            {QStringLiteral("thermal_cpu_limit_c"), 85.0},
            {QStringLiteral("thermal_gpu_limit_c"), 82.0},
            {QStringLiteral("gaming_session_active"), gamingSession},
            {QStringLiteral("detected_gaming_processes"), gamingSession ? QStringList{QStringLiteral("gamescope")} : QStringList{}},
            {QStringLiteral("recommended_profile"), (gamingSession && !thermalGuard) ? QStringLiteral("gaming") : QStringLiteral("general")},
            {QStringLiteral("gaming_session_advisory"), QStringLiteral("Sessiz profil dayatması kapalıdır.")},
            {QStringLiteral("silent_auto_switch_blocked"), true},
            {QStringLiteral("profiles_json"), profilesJson}
        };
    }

public slots:
    QVariantMap GetProfileState()
    {
        return QVariantMap{
            {QStringLiteral("success"), true},
            {QStringLiteral("code"), QStringLiteral("ok")},
            {QStringLiteral("message"), QString()},
            {QStringLiteral("operationId"), QString()},
            {QStringLiteral("data"), makeProfileData()}
        };
    }

    QVariantMap SetProfile(const QString &profile, bool userConfirmed)
    {
        ++setCalls;
        if (!userConfirmed) {
            return QVariantMap{
                {QStringLiteral("success"), false},
                {QStringLiteral("code"), QStringLiteral("confirmation-required")},
                {QStringLiteral("message"), QStringLiteral("Açık kullanıcı onayı gereklidir.")}
            };
        }
        if (profile == QStringLiteral("gaming") && thermalGuard) {
            return QVariantMap{
                {QStringLiteral("success"), false},
                {QStringLiteral("code"), QStringLiteral("thermal-limit")},
                {QStringLiteral("message"), QStringLiteral("Termal koruma etkin: GPU sıcaklığı sınırı aştı.")},
                {QStringLiteral("data"), makeProfileData()}
            };
        }
        activeProfile = profile;
        cpuGovernor = (profile == QStringLiteral("gaming") || profile == QStringLiteral("developer"))
            ? QStringLiteral("performance")
            : QStringLiteral("schedutil");
        gpuDpm = (profile == QStringLiteral("gaming")) ? QStringLiteral("high") : QStringLiteral("auto");
        const QVariantMap data = makeProfileData();
        emit ProfileChanged(data);
        return QVariantMap{
            {QStringLiteral("success"), true},
            {QStringLiteral("code"), QStringLiteral("ok")},
            {QStringLiteral("message"), QStringLiteral("Profil uygulandı.")},
            {QStringLiteral("data"), data}
        };
    }

    QVariantMap GetGamingReadiness()
    {
        return QVariantMap{
            {QStringLiteral("success"), true},
            {QStringLiteral("code"), QStringLiteral("ok")},
            {QStringLiteral("message"), QString()},
            {QStringLiteral("data"), QVariantMap{
                {QStringLiteral("gpu_name"), QStringLiteral("AMD Radeon RX 560 Series (RADV POLARIS11)")},
                {QStringLiteral("matched_profile"), QStringLiteral("amd-rx560-polaris11")},
                {QStringLiteral("tier"), QStringLiteral("dxvk_ready_vkd3d_limited")},
                {QStringLiteral("tier_label"), QStringLiteral("DXVK Hazır · VKD3D Sınırlı (Polaris11)")},
                {QStringLiteral("driver_name"), QStringLiteral("radv")},
                {QStringLiteral("driver_info"), QStringLiteral("Mesa 26.3.1")},
                {QStringLiteral("vulkan_api_version"), QStringLiteral("1.4.363")},
                {QStringLiteral("vulkan_available"), true},
                {QStringLiteral("hardware_vulkan_13"), true},
                {QStringLiteral("dxvk_supported"), true},
                {QStringLiteral("vkd3d_supported"), false},
                {QStringLiteral("dxvk_available"), false},
                {QStringLiteral("vkd3d_available"), false},
                {QStringLiteral("steam_available"), true},
                {QStringLiteral("proton_available"), false},
                {QStringLiteral("gamescope_available"), false},
                {QStringLiteral("mangohud_available"), true},
                {QStringLiteral("gamemode_available"), true},
                {QStringLiteral("wine_available"), true},
                {QStringLiteral("limitations"), QStringList{QStringLiteral("RX 560D 4 GB VRAM ve 82 °C termal sınırı.")}},
                {QStringLiteral("missing_components"), QStringList{QStringLiteral("dxvk-mingw-git"), QStringLiteral("gamescope")}},
                {QStringLiteral("kernel_anticheat_bypass_forbidden"), true},
                {QStringLiteral("anticheat_eula_notice"), QStringLiteral("Uyarı: Çekirdek seviyesi anti-cheat atlatma yasaktır.")}
            }}
        };
    }

signals:
    void ProfileChanged(const QVariantMap &state);
};

} // namespace

class PerformanceClientTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(QDBusConnection::systemBus().isConnected());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"), qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(!qgetenv("DBUS_SYSTEM_BUS_ADDRESS").isEmpty());
    }

    void rejectsInvalidProfileAndUnconfirmedSilentSwitchLocally()
    {
        PerformanceClient client;
        client.setAutoRefreshEnabled(false);
        QSignalSpy opSpy(&client, &PerformanceClient::operationFinished);

        client.setProfile(QStringLiteral("turbo-overclock"), true);
        QCOMPARE(opSpy.count(), 1);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), false);

        client.setProfile(QStringLiteral("gaming"), false);
        QCOMPARE(opSpy.count(), 1);
        const QList<QVariant> ev = opSpy.takeFirst();
        QCOMPARE(ev.at(1).toBool(), false);
        QVERIFY(ev.at(2).toString().contains(QStringLiteral("onayı")));
        QVERIFY(client.silentAutoSwitchBlocked());
        QCOMPARE(client.activeProfile(), QStringLiteral("general"));
    }

    void privateBusProfileSwitchThermalGuardAndReadinessWorkEndToEnd()
    {
        auto bus = QDBusConnection::systemBus();
        MockPerformanceService svc;
        QVERIFY(bus.registerObject(
            QStringLiteral("/org/zethropol/Performance"),
            &svc,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(bus.registerService(QStringLiteral("org.zethropol.Performance")));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(QStringLiteral("org.zethropol.Performance"));
            bus.unregisterObject(QStringLiteral("/org/zethropol/Performance"));
        });

        PerformanceClient client;
        client.setAutoRefreshEnabled(false);
        QVERIFY(client.signalConnected());

        QTRY_VERIFY_WITH_TIMEOUT(client.available(), 4000);
        QCOMPARE(client.activeProfile(), QStringLiteral("general"));
        QCOMPARE(client.profiles().size(), 4);
        QVERIFY(client.gamingSessionActive());
        QCOMPARE(client.recommendedProfile(), QStringLiteral("gaming"));
        QVERIFY(client.silentAutoSwitchBlocked());

        QTRY_COMPARE_WITH_TIMEOUT(
            client.readiness().value(QStringLiteral("matchedProfile")).toString(),
            QStringLiteral("amd-rx560-polaris11"),
            4000);
        QCOMPARE(client.readiness().value(QStringLiteral("steamAvailable")).toBool(), true);
        QCOMPARE(client.readiness().value(QStringLiteral("gamescopeAvailable")).toBool(), false);

        // Unconfirmed switch must never even reach D-Bus service
        client.setProfile(QStringLiteral("gaming"), false);
        QCOMPARE(svc.setCalls, 0);

        // Thermal guard blocks Gaming even when confirmed
        svc.thermalGuard = true;
        QSignalSpy opSpy(&client, &PerformanceClient::operationFinished);
        client.setProfile(QStringLiteral("gaming"), true);
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 4000);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), false);
        QCOMPARE(client.activeProfile(), QStringLiteral("general"));
        QVERIFY(client.thermalGuardActive());

        // Cool down -> confirmed Gaming succeeds and switches governor/GPU DPM
        svc.thermalGuard = false;
        client.setProfile(QStringLiteral("gaming"), true);
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 4000);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), true);
        QCOMPARE(client.activeProfile(), QStringLiteral("gaming"));
        QCOMPARE(client.cpuGovernor(), QStringLiteral("performance"));
        QCOMPARE(client.gpuDpmLevel(), QStringLiteral("high"));

        // Switch to Developer and Creator
        client.setProfile(QStringLiteral("developer"), true);
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 4000);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), true);
        QCOMPARE(client.activeProfile(), QStringLiteral("developer"));
        QCOMPARE(client.gpuDpmLevel(), QStringLiteral("auto"));

        client.setProfile(QStringLiteral("creator"), true);
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 4000);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), true);
        QCOMPARE(client.activeProfile(), QStringLiteral("creator"));
        QCOMPARE(client.cpuGovernor(), QStringLiteral("schedutil"));
    }

    void performancePageQmlRendersAllFourProfilesSessionAndReadinessWithoutWarnings()
    {
        auto bus = QDBusConnection::systemBus();
        MockPerformanceService svc;
        QVERIFY(bus.registerObject(
            QStringLiteral("/org/zethropol/Performance"),
            &svc,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
        QVERIFY(bus.registerService(QStringLiteral("org.zethropol.Performance")));
        const auto cleanup = qScopeGuard([&] {
            bus.unregisterService(QStringLiteral("org.zethropol.Performance"));
            bus.unregisterObject(QStringLiteral("/org/zethropol/Performance"));
        });

        PerformanceClient perfClient;
        perfClient.setAutoRefreshEnabled(false);
        MockMonitoringState mon;
        QTRY_VERIFY_WITH_TIMEOUT(perfClient.available(), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(
            perfClient.readiness().value(QStringLiteral("matchedProfile")).toString(),
            QStringLiteral("amd-rx560-polaris11"),
            4000);

        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.rootContext()->setContextProperty(QStringLiteral("PerformanceBridge"), &perfClient);
        engine.rootContext()->setContextProperty(QStringLiteral("MonitoringBridge"), &mon);

        QQmlComponent comp(&engine, QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/PerformancePage.qml")));
        QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));
        std::unique_ptr<QQuickItem> item(qobject_cast<QQuickItem *>(comp.create()));
        QVERIFY(item != nullptr);
        item->setWidth(960);
        item->setHeight(760);
        QTest::qWait(40);

        const QString texts = collectTexts(item.get());
        QVERIFY(texts.contains(QStringLiteral("General")));
        QVERIFY(texts.contains(QStringLiteral("Gaming")));
        QVERIFY(texts.contains(QStringLiteral("Developer")));
        QVERIFY(texts.contains(QStringLiteral("Creator")));
        QVERIFY(texts.contains(QStringLiteral("gamescope")));
        QVERIFY(texts.contains(QStringLiteral("POLARIS11")));
        QVERIFY(texts.contains(QStringLiteral("anti-cheat")));
        QVERIFY(texts.contains(QStringLiteral("VRAM: 1.25 / 4.00 GB")));
        QVERIFY(!texts.contains(QStringLiteral("NaN")));
        QVERIFY(!texts.contains(QStringLiteral("undefined")));
        QVERIFY2(warnings.isEmpty(),
                 qPrintable(QStringLiteral("Unexpected QML warnings: %1").arg(warnings.count())));
    }
};

QTEST_MAIN(PerformanceClientTest)
#include "PerformanceClientTest.moc"
