#include "CompatibilityManager.h"

#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

namespace {

QString writeExecutable(const QDir &dir, const QString &name, const QByteArray &body)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    f.write("#!/usr/bin/env bash\n");
    f.write(body);
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                     | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                     | QFileDevice::ReadOther | QFileDevice::ExeOther);
    return path;
}

} // namespace

class CompatibilityManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void missingToolsDegradesGracefullyWithoutCrashing()
    {
        QTemporaryDir emptyDir;
        QVERIFY(emptyDir.isValid());
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", emptyDir.path().toUtf8());
        const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });

        CompatibilityManager mgr;
        QSignalSpy refreshSpy(&mgr, &CompatibilityManager::refreshFinished);
        mgr.refresh();

        QCOMPARE(refreshSpy.count(), 1);
        QCOMPARE(refreshSpy.takeFirst().at(0).toBool(), false);
        QVERIFY(!mgr.available());
        QVERIFY(!mgr.busy());
        QCOMPARE(mgr.profileCount(), 0);
        QVERIFY(mgr.statusMessage().contains(QStringLiteral("kurulu değil")));
    }

    void inputValidationFailsClosedWithoutSpawningProcess()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString marker = tmp.filePath(QStringLiteral("invoked.marker"));
        const QString mockCtl = writeExecutable(
            QDir(tmp.path()),
            QStringLiteral("mock-ctl"),
            QByteArray("touch \"") + marker.toUtf8() + QByteArray("\"\nexit 0\n"));
        QVERIFY(!mockCtl.isEmpty());

        CompatibilityManager mgr;
        mgr.setPrefixCtlPath(mockCtl);
        mgr.setWineRunPath(mockCtl);

        // Traversal / uppercase / bad template / bad renderer
        mgr.initProfile(QStringLiteral("../escape"), QStringLiteral("default-win64"), QStringLiteral("auto"), true, false);
        mgr.initProfile(QStringLiteral("BadUpper"), QStringLiteral("default-win64"), QStringLiteral("auto"), true, false);
        mgr.initProfile(QStringLiteral("good-prof"), QStringLiteral("win95"), QStringLiteral("auto"), true, false);
        mgr.initProfile(QStringLiteral("good-prof"), QStringLiteral("default-win64"), QStringLiteral("opengl"), true, false);

        // Unconfirmed removal must never invoke CLI
        mgr.removeProfile(QStringLiteral("default-win64"), false);

        // Unsupported file extension must never invoke runner
        const QString badFile = tmp.filePath(QStringLiteral("evil.sh"));
        QFile f(badFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("echo nope\n");
        f.close();
        mgr.previewLaunch(badFile, QStringLiteral("default-win64"), QStringLiteral("auto"), true);
        mgr.launchTarget(badFile, QStringLiteral("default-win64"), QStringLiteral("auto"), true);

        QTest::qWait(50);
        QVERIFY(!QFile::exists(marker));
        QVERIFY(!mgr.busy());
    }

    void refreshLifecycleParsesProbeListAndPerProfileAudits()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString stateFile = tmp.filePath(QStringLiteral("state.txt"));
        const QString argsLog = tmp.filePath(QStringLiteral("args.log"));

        const QByteArray mockCtlBody =
            QByteArray("echo \"$*\" >> \"") + argsLog.toUtf8() + QByteArray("\"\n")
            + QByteArray(
                "cmd=\"${1:-}\"\n"
                "if [ \"$cmd\" = \"probe\" ]; then\n"
                "  printf '%s\\n' '{\"version\":\"0.1.0\",\"wine\":{\"binary\":\"/usr/bin/wine\",\"version\":\"wine-11.22\",\"supports_native_win32\":false,\"supports_wow64\":true,\"mode\":\"wow64-only\"},\"vulkan\":{\"available\":true,\"hardware_vulkan_13\":true,\"devices\":[{\"name\":\"AMD Radeon RX 560 Series (RADV POLARIS11)\",\"device_type\":\"PHYSICAL_DEVICE_TYPE_DISCRETE_GPU\",\"api_version\":\"1.4.354\",\"driver_name\":\"radv\",\"is_hardware\":true,\"meets_dxvk_min_version\":true}],\"reason\":\"Hardware Vulkan 1.4.354 GPU detected.\"},\"translation_layers\":{\"dxvk_available\":false,\"dxvk_source\":\"none\",\"vkd3d_available\":false,\"vkd3d_source\":\"none\"},\"bwrap\":\"/usr/bin/bwrap\"}'\n"
                "  exit 0\n"
                "fi\n"
                "if [ \"$cmd\" = \"list\" ]; then\n"
                "  if [ -f \"") + stateFile.toUtf8() + QByteArray("\" ]; then\n"
                "    printf '%s\\n' '{\"profiles\":[]}'\n"
                "  else\n"
                "    printf '%s\\n' '{\"profiles\":[{\"profile\":\"default-win64\",\"template\":\"default-win64\",\"requested_arch\":\"win64\",\"effective_arch\":\"win64\",\"windows_version\":\"win10\",\"renderer\":\"auto\",\"locale\":\"tr_TR.UTF-8\",\"sandbox\":true,\"wine_version\":\"wine-11.22\"},{\"profile\":\"classic-win32\",\"template\":\"classic-win32\",\"requested_arch\":\"win32\",\"effective_arch\":\"win64-wow64\",\"windows_version\":\"win7\",\"renderer\":\"wined3d\",\"locale\":\"tr_TR.UTF-8\",\"sandbox\":true,\"wine_version\":\"wine-11.22\"}]}'\n"
                "  fi\n"
                "  exit 0\n"
                "fi\n"
                "if [ \"$cmd\" = \"audit\" ]; then\n"
                "  prof=\"$2\"\n"
                "  if [ \"$prof\" = \"default-win64\" ]; then\n"
                "    printf '%s\\n' '{\"profile\":\"default-win64\",\"prefix_path\":\"/tmp/p1\",\"passed\":true,\"checks\":[{\"id\":\"z_drive_isolated\",\"passed\":true,\"detail\":\"ok\"},{\"id\":\"home_folders_isolated\",\"passed\":true,\"detail\":\"ok\"},{\"id\":\"turkish_locale_registry\",\"passed\":true,\"detail\":\"ok\"},{\"id\":\"font_substitutes_registry\",\"passed\":true,\"detail\":\"ok\"}]}'\n"
                "    exit 0\n"
                "  else\n"
                "    printf '%s\\n' '{\"profile\":\"classic-win32\",\"prefix_path\":\"/tmp/p2\",\"passed\":false,\"checks\":[{\"id\":\"z_drive_isolated\",\"passed\":false,\"detail\":\"z present\"},{\"id\":\"home_folders_isolated\",\"passed\":true,\"detail\":\"ok\"},{\"id\":\"turkish_locale_registry\",\"passed\":true,\"detail\":\"ok\"},{\"id\":\"font_substitutes_registry\",\"passed\":true,\"detail\":\"ok\"}]}'\n"
                "    exit 1\n"
                "  fi\n"
                "fi\n"
                "if [ \"$cmd\" = \"sync-shortcuts\" ]; then\n"
                "  printf '%s\\n' '{\"profile\":\"default-win64\",\"converted\":[\"/tmp/a.desktop\"],\"skipped\":[]}'\n"
                "  exit 0\n"
                "fi\n"
                "if [ \"$cmd\" = \"init\" ]; then\n"
                "  printf '%s\\n' '{\"metadata\":{\"profile\":\"office\"},\"audit\":{\"passed\":true,\"checks\":[]}}'\n"
                "  exit 0\n"
                "fi\n"
                "if [ \"$cmd\" = \"remove\" ]; then\n"
                "  touch \"") + stateFile.toUtf8() + QByteArray("\"\n"
                "  printf '%s\\n' '{\"removed_profile\":\"default-win64\",\"removed_desktop_files\":[]}'\n"
                "  exit 0\n"
                "fi\n"
                "exit 2\n");

        const QString mockCtl = writeExecutable(QDir(tmp.path()), QStringLiteral("mock-prefix-ctl"), mockCtlBody);
        QVERIFY(!mockCtl.isEmpty());

        CompatibilityManager mgr;
        mgr.setPrefixCtlPath(mockCtl);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_DATA_HOME"), tmp.filePath(QStringLiteral("xdg-data")));
        mgr.setProcessEnvironment(env);

        QSignalSpy refreshSpy(&mgr, &CompatibilityManager::refreshFinished);
        mgr.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(refreshSpy.count(), 1, 5000);
        QCOMPARE(refreshSpy.takeFirst().at(0).toBool(), true);

        QVERIFY(mgr.available());
        QVERIFY(!mgr.busy());
        QCOMPARE(mgr.wineVersion(), QStringLiteral("wine-11.22"));
        QCOMPARE(mgr.wineMode(), QStringLiteral("wow64-only"));
        QVERIFY(mgr.vulkanAvailable());
        QVERIFY(mgr.hardwareVulkan13());
        QCOMPARE(mgr.vulkanGpuName(), QStringLiteral("AMD Radeon RX 560 Series (RADV POLARIS11)"));
        QCOMPARE(mgr.vulkanApiVersion(), QStringLiteral("1.4.354"));
        QVERIFY(!mgr.dxvkAvailable());
        QVERIFY(mgr.bwrapAvailable());

        QCOMPARE(mgr.profileCount(), 2);
        const QVariantMap p0 = mgr.profiles().at(0).toMap();
        QCOMPARE(p0.value(QStringLiteral("profile")).toString(), QStringLiteral("default-win64"));
        QVERIFY(p0.value(QStringLiteral("auditPassed")).toBool());
        QVERIFY(p0.value(QStringLiteral("zDriveIsolated")).toBool());
        QVERIFY(p0.value(QStringLiteral("turkishLocaleOk")).toBool());
        QVERIFY(p0.value(QStringLiteral("fontsOk")).toBool());

        const QVariantMap p1 = mgr.profiles().at(1).toMap();
        QCOMPARE(p1.value(QStringLiteral("profile")).toString(), QStringLiteral("classic-win32"));
        QCOMPARE(p1.value(QStringLiteral("effectiveArch")).toString(), QStringLiteral("win64-wow64"));
        QVERIFY(!p1.value(QStringLiteral("auditPassed")).toBool());
        QVERIFY(!p1.value(QStringLiteral("zDriveIsolated")).toBool());

        // sync-shortcuts
        QSignalSpy opSpy(&mgr, &CompatibilityManager::operationFinished);
        mgr.syncShortcuts(QStringLiteral("default-win64"));
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 5000);
        const QList<QVariant> syncEv = opSpy.takeFirst();
        QCOMPARE(syncEv.at(0).toString(), QStringLiteral("sync-shortcuts"));
        QCOMPARE(syncEv.at(1).toBool(), true);
        QVERIFY(syncEv.at(2).toString().contains(QStringLiteral("1 dönüştürüldü")));

        // initProfile triggers refresh cleanly
        mgr.initProfile(QStringLiteral("office"), QStringLiteral("default-win64"), QStringLiteral("auto"), true, false);
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 5000);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(refreshSpy.count(), 1, 5000);
        refreshSpy.clear();

        // removeProfile with confirmation passes --yes and refreshes to empty list
        mgr.removeProfile(QStringLiteral("default-win64"), true);
        QTRY_COMPARE_WITH_TIMEOUT(opSpy.count(), 1, 5000);
        QCOMPARE(opSpy.takeFirst().at(1).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(refreshSpy.count(), 1, 5000);
        QCOMPARE(mgr.profileCount(), 0);

        QFile logFile(argsLog);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        const QString logged = QString::fromUtf8(logFile.readAll());
        QVERIFY(logged.contains(QStringLiteral("remove default-win64 --yes --json")));
    }

    void dryRunAndLaunchWithMockWineRunAndFileUrl()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString sampleMsi = tmp.filePath(QStringLiteral("setup.msi"));
        QFile f(sampleMsi);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("MSI");
        f.close();

        const QString mockRunner = writeExecutable(
            QDir(tmp.path()),
            QStringLiteral("mock-wine-run"),
            QByteArray(
                "if printf '%s' \"$*\" | grep -q -- '--dry-run'; then\n"
                "  if printf '%s' \"$*\" | grep -q -- '--no-sandbox'; then\n"
                "    printf '%s\\n' '{\"profile\":\"default-win64\",\"target_host_path\":\"/tmp/setup.msi\",\"target_windows_path\":\"Y:\\\\setup.msi\",\"target_kind\":\"msi\",\"effective_arch\":\"win64\",\"renderer\":{\"effective\":\"wined3d\",\"fallback_applied\":true,\"reason\":\"DXVK absent\"},\"sandbox_enabled\":false,\"sandbox_tool\":\"disabled\",\"sandbox_warning\":\"sandbox disabled\"}'\n"
                "  else\n"
                "    printf '%s\\n' '{\"profile\":\"default-win64\",\"target_host_path\":\"/tmp/setup.msi\",\"target_windows_path\":\"Y:\\\\setup.msi\",\"target_kind\":\"msi\",\"effective_arch\":\"win64\",\"renderer\":{\"effective\":\"wined3d\",\"fallback_applied\":true,\"reason\":\"DXVK absent\"},\"sandbox_enabled\":true,\"sandbox_tool\":\"/usr/bin/bwrap\",\"sandbox_warning\":\"\"}'\n"
                "  fi\n"
                "  exit 0\n"
                "fi\n"
                "exit 0\n"));
        QVERIFY(!mockRunner.isEmpty());

        CompatibilityManager mgr;
        mgr.setWineRunPath(mockRunner);
        QSignalSpy drySpy(&mgr, &CompatibilityManager::dryRunCompleted);

        // Pass as file:// URL (as returned by QML FileDialog)
        const QString fileUrl = QUrl::fromLocalFile(sampleMsi).toString();
        mgr.previewLaunch(fileUrl, QStringLiteral("default-win64"), QStringLiteral("auto"), true);
        QTRY_COMPARE_WITH_TIMEOUT(drySpy.count(), 1, 5000);
        const QList<QVariant> ev1 = drySpy.takeFirst();
        QCOMPARE(ev1.at(0).toBool(), true);
        const QVariantMap plan1 = ev1.at(1).toMap();
        QCOMPARE(plan1.value(QStringLiteral("targetKind")).toString(), QStringLiteral("msi"));
        QCOMPARE(plan1.value(QStringLiteral("effectiveRenderer")).toString(), QStringLiteral("wined3d"));
        QVERIFY(plan1.value(QStringLiteral("sandboxEnabled")).toBool());

        // --no-sandbox dry-run
        mgr.previewLaunch(sampleMsi, QStringLiteral("default-win64"), QStringLiteral("wined3d"), false);
        QTRY_COMPARE_WITH_TIMEOUT(drySpy.count(), 1, 5000);
        const QVariantMap plan2 = drySpy.takeFirst().at(1).toMap();
        QVERIFY(!plan2.value(QStringLiteral("sandboxEnabled")).toBool());
        QCOMPARE(plan2.value(QStringLiteral("sandboxWarning")).toString(), QStringLiteral("sandbox disabled"));

        // Non-blocking launchTarget completes cleanly
        QSignalSpy launchSpy(&mgr, &CompatibilityManager::launchFinished);
        mgr.launchTarget(sampleMsi, QStringLiteral("default-win64"), QStringLiteral("auto"), true);
        QTRY_COMPARE_WITH_TIMEOUT(launchSpy.count(), 1, 5000);
        QCOMPARE(launchSpy.takeFirst().at(1).toInt(), 0);
        QVERIFY(!mgr.launching());
    }

    void handlesCorruptJsonAndCommandTimeoutSafely()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        const QString badJsonCtl = writeExecutable(
            QDir(tmp.path()), QStringLiteral("bad-json-ctl"), QByteArray("echo '{not-json'\nexit 0\n"));
        CompatibilityManager mgr1;
        mgr1.setPrefixCtlPath(badJsonCtl);
        QSignalSpy spy1(&mgr1, &CompatibilityManager::refreshFinished);
        mgr1.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(spy1.count(), 1, 5000);
        QCOMPARE(spy1.takeFirst().at(0).toBool(), false);
        QVERIFY(!mgr1.busy());
        QVERIFY(!mgr1.lastError().isEmpty());

        const QString slowCtl = writeExecutable(
            QDir(tmp.path()), QStringLiteral("slow-ctl"), QByteArray("sleep 5\nexit 0\n"));
        CompatibilityManager mgr2;
        mgr2.setPrefixCtlPath(slowCtl);
        mgr2.setCommandTimeoutMs(120);
        QSignalSpy spy2(&mgr2, &CompatibilityManager::refreshFinished);
        mgr2.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(spy2.count(), 1, 5000);
        QCOMPARE(spy2.takeFirst().at(0).toBool(), false);
        QVERIFY(!mgr2.busy());
        QVERIFY(mgr2.lastError().contains(QStringLiteral("zaman aşımına")));
    }
};

QTEST_MAIN(CompatibilityManagerTest)
#include "CompatibilityManagerTest.moc"
