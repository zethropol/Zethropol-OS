#include "HardwareServiceClient.h"
#include "UpdateScanner.h"
#include "UpdateInstaller.h"
#include "RecoveryService.h"
#include "SecurityService.h"
#include "ServiceService.h"
#include "ApplicationService.h"
#include "ProcessService.h"
#include "NotificationService.h"
#include "core/zethropol_power_client.h"
#include "core/zethropol_monitoring_client.h"
#include "core/zethropol_recovery_client.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFile>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QVariant unwrapVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return unwrapVariant(value.value<QDBusVariant>().variant());
    if (value.userType() == QMetaType::QVariantMap) {
        const QVariantMap src = value.toMap();
        QVariantMap out;
        for (auto it = src.cbegin(); it != src.cend(); ++it)
            out.insert(it.key(), unwrapVariant(it.value()));
        return out;
    }
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;
    const QDBusArgument arg = value.value<QDBusArgument>();
    if (arg.currentType() == QDBusArgument::MapType) {
        const bool isStringMap = arg.currentSignature() == QStringLiteral("a{ss}");
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QString k;
            if (isStringMap) {
                QString s;
                arg >> k >> s;
                map.insert(k, s);
            } else {
                QDBusVariant v;
                arg >> k >> v;
                map.insert(k, unwrapVariant(v.variant()));
            }
            arg.endMapEntry();
        }
        arg.endMap();
        return map;
    }
    if (arg.currentType() == QDBusArgument::StructureType) {
        bool success = false;
        QString code;
        QString message;
        QString opId;
        QVariantMap dataMap;
        arg.beginStructure();
        arg >> success >> code >> message >> opId;
        if (!arg.atEnd() && arg.currentType() == QDBusArgument::MapType) {
            arg.beginMap();
            while (!arg.atEnd()) {
                arg.beginMapEntry();
                QString k;
                QDBusVariant v;
                arg >> k >> v;
                dataMap.insert(k, unwrapVariant(v.variant()));
                arg.endMapEntry();
            }
            arg.endMap();
        }
        arg.endStructure();
        return QVariantMap{
            {QStringLiteral("success"), success},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message},
            {QStringLiteral("operation_id"), opId},
            {QStringLiteral("data"), dataMap}
        };
    }
    return value;
}
} // namespace

class RecoveryTest : public QObject
{
    Q_OBJECT

private:
    QProcess serviceProcess;

    QString resolveRecoveryBinary() const
    {
        const QString envBin = qEnvironmentVariable("ZETHROPOL_RECOVERY_SERVICE_BINARY");
        if (!envBin.isEmpty() && QFile::exists(envBin))
            return envBin;
        const QString tmpBin = QStringLiteral("/tmp/zethropol-recovery-target/debug/recovery");
        if (QFile::exists(tmpBin))
            return tmpBin;
        return QStringLiteral(RECOVERY_BINARY);
    }

    QString writeFile(const QTemporaryDir &dir, const QString &relPath, const QByteArray &content)
    {
        const QString fullPath = dir.filePath(relPath);
        QDir().mkpath(QFileInfo(fullPath).absolutePath());
        QFile file(fullPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(content);
            file.close();
        }
        return fullPath;
    }

    QString writeScript(const QTemporaryDir &dir, const QString &relPath, const QByteArray &body)
    {
        const QString fullPath = writeFile(dir, relPath, "#!/bin/sh\n" + body);
        QFile::setPermissions(fullPath,
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                              | QFile::ReadGroup | QFile::ExeGroup
                              | QFile::ReadOther | QFile::ExeOther);
        return fullPath;
    }

    struct EnvPaths {
        QString snapperBin;
        QString pkcheckBin;
        QString sysconfFile;
        QString configsDir;
        QString mountinfoFile;
        QString fstabFile;
        QString snapshotsDir;
        QString envFile;
        QString createdStateFile;
        QString createLogFile;
        int timeoutMs = 4000;
    };

    EnvPaths setupStandardFixture(const QTemporaryDir &dir)
    {
        EnvPaths paths;
        paths.createdStateFile = dir.filePath(QStringLiteral("created_snapshot.txt"));
        paths.createLogFile = dir.filePath(QStringLiteral("create_calls.log"));
        paths.mountinfoFile = writeFile(
            dir,
            QStringLiteral("mountinfo"),
            QByteArrayLiteral("36 1 0:36 /@ / rw,noatime - btrfs /dev/nvme0n1p2 rw,subvolid=256,subvol=/@\n"));
        paths.fstabFile = writeFile(
            dir,
            QStringLiteral("fstab"),
            QByteArrayLiteral("UUID=1111-2222 / btrfs subvol=/@,defaults 0 0\n"));
        paths.sysconfFile = writeFile(
            dir,
            QStringLiteral("etc/conf.d/snapper"),
            QByteArrayLiteral("SNAPPER_CONFIGS=\"root\"\n"));
        paths.configsDir = dir.filePath(QStringLiteral("etc/snapper/configs"));
        QDir().mkpath(paths.configsDir);
        writeFile(
            dir,
            QStringLiteral("etc/snapper/configs/root"),
            QByteArrayLiteral("SUBVOLUME=\"/\"\nFSTYPE=\"btrfs\"\nALLOW_USERS=\"\"\n"));
        paths.snapshotsDir = dir.filePath(QStringLiteral(".snapshots"));
        QDir().mkpath(paths.snapshotsDir);
        paths.pkcheckBin = writeScript(
            dir,
            QStringLiteral("bin/pkcheck"),
            QByteArrayLiteral("exit 0\n"));

        const QByteArray snapperScript =
            "CREATED_FILE='" + paths.createdStateFile.toUtf8() + "'\n"
            "CREATE_LOG='" + paths.createLogFile.toUtf8() + "'\n"
            "for arg in \"$@\"; do\n"
            "  if [ \"$arg\" = \"create\" ]; then\n"
            "    echo \"$*\" >> \"$CREATE_LOG\"\n"
            "    echo \"26\" > \"$CREATED_FILE\"\n"
            "    echo \"26\"\n"
            "    exit 0\n"
            "  fi\n"
            "done\n"
            "EXTRA=\"\"\n"
            "if [ -f \"$CREATED_FILE\" ]; then\n"
            "  EXTRA=',{\"number\": 26, \"type\": \"single\", \"date\": \"2026-11-17 00:10:00\", \"user\": \"root\", \"cleanup\": \"number\", \"description\": \"Created recovery point\", \"userdata\": {\"zethropol\": \"manual\"}}'\n"
            "fi\n"
            "cat << EOF\n"
            "{\n"
            "  \"root\": [\n"
            "    {\"number\": 0, \"type\": \"single\", \"date\": \"\", \"user\": \"root\", \"cleanup\": \"\", \"description\": \"current\", \"userdata\": {}},\n"
            "    {\"number\": 21, \"type\": \"pre\", \"date\": \"2026-11-16 14:00:00\", \"user\": \"root\", \"cleanup\": \"number\", \"description\": \"pacman -Syu\", \"userdata\": {\"important\": \"yes\", \"zethropol\": \"system-upgrade\"}},\n"
            "    {\"number\": 22, \"type\": \"post\", \"pre-number\": 21, \"date\": \"2026-11-16 14:02:00\", \"user\": \"root\", \"cleanup\": \"number\", \"description\": \"pacman -Syu\", \"userdata\": {\"important\": \"yes\", \"zethropol\": \"system-upgrade\"}},\n"
            "    {\"number\": 25, \"type\": \"single\", \"date\": \"2026-11-16 15:30:00\", \"user\": \"root\", \"cleanup\": \"\", \"description\": \"Manual recovery point\", \"userdata\": {\"important\": \"no\"}}\n"
            "    $EXTRA\n"
            "  ]\n"
            "}\n"
            "EOF\n";

        paths.snapperBin = writeScript(dir, QStringLiteral("bin/snapper"), snapperScript);
        paths.envFile = dir.filePath(QStringLiteral("recovery.env"));
        updateEnvFile(paths);
        return paths;
    }

    void updateEnvFile(const EnvPaths &paths)
    {
        QByteArray content;
        content += "ZETHROPOL_RECOVERY_SNAPPER_BIN=" + paths.snapperBin.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_PKCHECK_BIN=" + paths.pkcheckBin.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_SYSCONF_FILE=" + paths.sysconfFile.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_CONFIGS_DIR=" + paths.configsDir.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_MOUNTINFO_FILE=" + paths.mountinfoFile.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_FSTAB_FILE=" + paths.fstabFile.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_SNAPSHOTS_DIR=" + paths.snapshotsDir.toUtf8() + "\n";
        content += "ZETHROPOL_RECOVERY_TIMEOUT_MS=" + QByteArray::number(paths.timeoutMs) + "\n";
        QFile file(paths.envFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(content);
            file.close();
        }
    }

    void startRecoveryDaemon(const QString &envFile)
    {
        stopRecoveryDaemon();
        const QString bin = resolveRecoveryBinary();
        QVERIFY2(QFile::exists(bin), qPrintable(QStringLiteral("Missing recovery binary: %1").arg(bin)));
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("ZETHROPOL_RECOVERY_ENV_FILE"), envFile);
        serviceProcess.setProcessEnvironment(env);
        serviceProcess.setProgram(bin);
        serviceProcess.setArguments({});
        serviceProcess.start();
        QVERIFY(serviceProcess.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(
            QDBusConnection::systemBus().interface()->isServiceRegistered(QStringLiteral("org.zethropol.Recovery")).value(),
            5000);
    }

    void stopRecoveryDaemon()
    {
        if (serviceProcess.state() != QProcess::NotRunning) {
            serviceProcess.terminate();
            if (!serviceProcess.waitForFinished(3000)) {
                serviceProcess.kill();
                serviceProcess.waitForFinished();
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            !QDBusConnection::systemBus().interface()->isServiceRegistered(QStringLiteral("org.zethropol.Recovery")).value(),
            3000);
    }

    QVariantMap callDbusCreate(const QString &name, const QString &description)
    {
        const QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.zethropol.Recovery"),
            QStringLiteral("/org/zethropol/Recovery"),
            QStringLiteral("org.zethropol.Recovery.v1"),
            QStringLiteral("CreateRecoveryPoint"))
            << name << description;
        const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 3000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return {};
        const QVariantList args = reply.arguments();
        if (args.size() >= 4 && args.at(0).userType() == QMetaType::Bool) {
            return QVariantMap{
                {QStringLiteral("success"), args.at(0).toBool()},
                {QStringLiteral("code"), args.at(1).toString()},
                {QStringLiteral("message"), args.at(2).toString()},
                {QStringLiteral("operation_id"), args.at(3).toString()}
            };
        }
        return unwrapVariant(args.constFirst()).toMap();
    }

    QVariantMap callDbusGetOperationState(const QString &opId)
    {
        const QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.zethropol.Recovery"),
            QStringLiteral("/org/zethropol/Recovery"),
            QStringLiteral("org.zethropol.Recovery.v1"),
            QStringLiteral("GetOperationState"))
            << opId;
        const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 3000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return {};
        return unwrapVariant(reply.arguments().constFirst()).toMap();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(QDBusConnection::systemBus().isConnected());
        QCOMPARE(qgetenv("DBUS_SYSTEM_BUS_ADDRESS"), qgetenv("DBUS_SESSION_BUS_ADDRESS"));
        QVERIFY(!qgetenv("DBUS_SYSTEM_BUS_ADDRESS").isEmpty());
        qmlRegisterSingletonType(
            QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/SystemState.qml")),
            "Zethropol.ControlCenter", 1, 0, "SystemState");
    }

    void cleanup()
    {
        stopRecoveryDaemon();
    }

    // --- 03.2.02 Snapshot Creation Verification Scenarios (1..10) ---

    void createScenario1_validConfigurationSucceeds()
    {
        QTemporaryDir dir;
        const EnvPaths paths = setupStandardFixture(dir);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_and_configured"));
        QCOMPARE(recovery.snapshotCount(), 4);

        // Require explicit confirmation before starting
        recovery.confirmCreateRecoveryPoint();
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("confirmation-required"));
        QVERIFY(!recovery.actionBusy());

        recovery.requestCreateRecoveryPoint(QStringLiteral("Before kernel upgrade"));
        QVERIFY(recovery.confirmationPending());
        QCOMPARE(recovery.pendingDescription(), QStringLiteral("Before kernel upgrade"));

        recovery.confirmCreateRecoveryPoint();
        QVERIFY(!recovery.confirmationPending());
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("ok"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 26);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("#26")));

        // Verify post-creation refresh updated the snapshot count and list
        QTRY_COMPARE_WITH_TIMEOUT(recovery.snapshotCount(), 5, 5000);
        QCOMPARE(recovery.recoveryPoints().size(), 2);

        // Verify exact target config (-c root) and --print-number were used
        QFile createLog(paths.createLogFile);
        QVERIFY(createLog.open(QIODevice::ReadOnly));
        const QByteArray loggedArgs = createLog.readAll();
        QVERIFY(loggedArgs.contains("-c root create --type single --print-number"));
        QVERIFY(loggedArgs.contains("--description=Before kernel upgrade"));
    }

    void createScenario2_snapperNotInstalledRejected()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = dir.filePath(QStringLiteral("bin/missing-snapper"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("not_installed"));

        recovery.createRecoveryPoint(QStringLiteral("Should fail"));
        QVERIFY(!recovery.actionBusy());
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("not-found"));
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("not installed")));

        // Also verify direct D-Bus rejection at service boundary
        const QVariantMap dbusRes = callDbusCreate(QStringLiteral("Manual"), QStringLiteral("Should fail"));
        QVERIFY(!dbusRes.value(QStringLiteral("success")).toBool());
        QCOMPARE(dbusRes.value(QStringLiteral("code")).toString(), QStringLiteral("not-found"));
        QVERIFY(!QFile::exists(paths.createLogFile));
    }

    void createScenario3_missingOrInvalidConfigurationRejected()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        // 3a: Missing config
        QFile::remove(dir.filePath(QStringLiteral("etc/snapper/configs/root")));
        writeFile(dir, QStringLiteral("etc/conf.d/snapper"), QByteArrayLiteral("SNAPPER_CONFIGS=\"\"\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_not_configured"));

        recovery.createRecoveryPoint(QStringLiteral("No config point"));
        QVERIFY(!recovery.actionBusy());
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("not-available"));

        const QVariantMap dbusNoCfg = callDbusCreate(QStringLiteral("Point"), QStringLiteral("No config"));
        QVERIFY(!dbusNoCfg.value(QStringLiteral("success")).toBool());
        QCOMPARE(dbusNoCfg.value(QStringLiteral("code")).toString(), QStringLiteral("not-available"));

        // 3b: Invalid config (wrong SUBVOLUME="/home")
        writeFile(dir, QStringLiteral("etc/conf.d/snapper"), QByteArrayLiteral("SNAPPER_CONFIGS=\"root\"\n"));
        writeFile(dir,
                  QStringLiteral("etc/snapper/configs/root"),
                  QByteArrayLiteral("SUBVOLUME=\"/home\"\nFSTYPE=\"btrfs\"\n"));
        recovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("invalid_configuration"));

        recovery.createRecoveryPoint(QStringLiteral("Invalid config point"));
        QVERIFY(!recovery.actionBusy());
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("failed"));

        const QVariantMap dbusInvalid = callDbusCreate(QStringLiteral("Point"), QStringLiteral("Invalid config"));
        QVERIFY(!dbusInvalid.value(QStringLiteral("success")).toBool());
        QCOMPARE(dbusInvalid.value(QStringLiteral("code")).toString(), QStringLiteral("failed"));
        QVERIFY(!QFile::exists(paths.createLogFile));
    }

    void createScenario4_permissionDeniedAndNoAutoPrivilege()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        // Polkit denies authorization
        paths.pkcheckBin = writeScript(
            dir,
            QStringLiteral("bin/pkcheck"),
            QByteArrayLiteral("echo 'Not authorized.' >&2\nexit 1\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        // Read-only detection still succeeds without root privilege escalation
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_and_configured"));

        // Creation requires polkit authorization and must be rejected
        recovery.createRecoveryPoint(QStringLiteral("Unauthorized attempt"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("permission-denied"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 0);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("Polkit")));
        // Snapper create was never executed
        QVERIFY(!QFile::exists(paths.createLogFile));
    }

    void createScenario5_backendUnavailableRejected()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral("echo 'Failure (org.freedesktop.DBus.Error.ServiceUnknown).' >&2\nexit 1\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("backend_unavailable"));

        recovery.createRecoveryPoint(QStringLiteral("Backend down"));
        QVERIFY(!recovery.actionBusy());
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("not-available"));

        const QVariantMap dbusRes = callDbusCreate(QStringLiteral("Point"), QStringLiteral("Backend down"));
        QVERIFY(!dbusRes.value(QStringLiteral("success")).toBool());
        QCOMPARE(dbusRes.value(QStringLiteral("code")).toString(), QStringLiteral("not-available"));

        // Also when org.zethropol.Recovery service itself is stopped
        stopRecoveryDaemon();
        recovery.createRecoveryPoint(QStringLiteral("Service stopped"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("not-available"));
    }

    void createScenario6_snapperNonZeroExitCodeFails()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral(R"(for arg in "$@"; do
  if [ "$arg" = "create" ]; then
    echo "Creating snapshot failed: No space left on Btrfs filesystem." >&2
    exit 1
  fi
done
echo '{"root":[{"number":0,"type":"single","description":"current"}]}'
)"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());

        recovery.createRecoveryPoint(QStringLiteral("Failing create"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QVERIFY(recovery.lastOperationCode() != QStringLiteral("ok"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 0);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("No space left")));
    }

    void createScenario7_unverifiedSnapshotIdOrUnlistedFails()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        // 7a: snapper exits 0 but prints no valid number
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral(R"(for arg in "$@"; do
  if [ "$arg" = "create" ]; then
    echo "done-without-number"
    exit 0
  fi
done
echo '{"root":[{"number":0,"type":"single","description":"current"}]}'
)"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());

        recovery.createRecoveryPoint(QStringLiteral("No ID output"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("verification-failed"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 0);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("valid snapshot ID")));

        // 7b: snapper exits 0 and prints "77", but #77 is missing from `snapper list`
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral(R"(for arg in "$@"; do
  if [ "$arg" = "create" ]; then
    echo "77"
    exit 0
  fi
done
echo '{"root":[{"number":0,"type":"single","description":"current"}]}'
)"));
        updateEnvFile(paths);
        recovery.createRecoveryPoint(QStringLiteral("Unlisted ID output"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("verification-failed"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 0);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("#77")));
    }

    void createScenario8_timeoutAndCancellation()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        // 8a: Timeout during snapshot creation
        paths.timeoutMs = 350;
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral(R"(for arg in "$@"; do
  if [ "$arg" = "create" ]; then
    sleep 3
    echo "88"
    exit 0
  fi
done
echo '{"root":[{"number":0,"type":"single","description":"current"}]}'
)"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());

        recovery.createRecoveryPoint(QStringLiteral("Slow snapshot"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("timeout"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 0);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("timed out")));

        // 8b: User cancellation while operation is running
        paths.timeoutMs = 5000;
        updateEnvFile(paths);
        recovery.createRecoveryPoint(QStringLiteral("Cancellable snapshot"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.lastOperationId().isEmpty(), 2000);
        const QString opId = recovery.lastOperationId();
        QVERIFY(recovery.actionBusy());

        recovery.cancelActiveOperation();
        QVERIFY(!recovery.actionBusy());
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("cancelled"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 0);
        QVERIFY(recovery.actionStatus().contains(QStringLiteral("cancelled")));

        QTRY_COMPARE_WITH_TIMEOUT(
            callDbusGetOperationState(opId).value(QStringLiteral("status")).toString(),
            QStringLiteral("cancelled"),
            2000);
    }

    void createScenario9_concurrentAndDuplicateRequestsBlocked()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            "CREATED_FILE='" + paths.createdStateFile.toUtf8() + "'\n"
            "for arg in \"$@\"; do\n"
            "  if [ \"$arg\" = \"create\" ]; then\n"
            "    sleep 0.4\n"
            "    echo \"30\" > \"$CREATED_FILE\"\n"
            "    echo \"30\"\n"
            "    exit 0\n"
            "  fi\n"
            "done\n"
            "if [ -f \"$CREATED_FILE\" ]; then\n"
            "  echo '{\"root\":[{\"number\":0,\"type\":\"single\",\"description\":\"current\"},{\"number\":30,\"type\":\"single\",\"description\":\"First\"}]}'\n"
            "else\n"
            "  echo '{\"root\":[{\"number\":0,\"type\":\"single\",\"description\":\"current\"}]}'\n"
            "fi\n");
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());

        recovery.createRecoveryPoint(QStringLiteral("First request"));
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.lastOperationId().isEmpty(), 2000);
        QVERIFY(recovery.actionBusy());

        // Concurrent D-Bus request while first is running must be rejected with "busy"
        const QVariantMap secondDbus = callDbusCreate(QStringLiteral("Second"), QStringLiteral("Duplicate"));
        QVERIFY(!secondDbus.value(QStringLiteral("success")).toBool());
        QCOMPARE(secondDbus.value(QStringLiteral("code")).toString(), QStringLiteral("busy"));

        // First request completes normally
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("ok"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 30);
    }

    void createScenario10_dbusAndControlCenterUiConsistency()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        startRecoveryDaemon(paths.envFile);

        QTemporaryDir emptyPathDir;
        const QString pkexecLog = dir.filePath(QStringLiteral("pkexec.log"));
        writeScript(emptyPathDir,
                    QStringLiteral("pkexec"),
                    "echo called >> '" + pkexecLog.toUtf8() + "'\nexit 1\n");
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", emptyPathDir.path().toUtf8());
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });

        HardwareServiceClient hardware;
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

        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());

        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {QStringLiteral("HardwareBridge"), &hardware},
                 {QStringLiteral("UpdateBridge"), &scanner},
                 {QStringLiteral("UpdateInstallBridge"), &installer},
                 {QStringLiteral("RecoveryBridge"), &recovery},
                 {QStringLiteral("SecurityBridge"), &security},
                 {QStringLiteral("ServiceBridge"), &services},
                 {QStringLiteral("ApplicationBridge"), &applications},
                 {QStringLiteral("ProcessBridge"), &processes},
                 {QStringLiteral("NotificationBridge"), &notifications},
                 {QStringLiteral("PowerBridge"), &power},
                 {QStringLiteral("MonitoringBridge"), &monitoring}}) {
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        }

        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);

        auto *pages = window->findChild<QQuickItem *>(QStringLiteral("controlCenterPages"));
        QVERIFY(pages);
        QCOMPARE(pages->property("count").toInt(), 12);
        QVERIFY(pages->setProperty("currentIndex", 7));
        QTest::qWait(40);

        auto *descInput = window->findChild<QQuickItem *>(QStringLiteral("recoveryDescriptionInput"));
        auto *createBtn = window->findChild<QQuickItem *>(QStringLiteral("createRecoveryPointButton"));
        auto *confirmDialog = window->findChild<QObject *>(QStringLiteral("recoveryCreateConfirmation"));
        auto *confirmBtn = window->findChild<QQuickItem *>(QStringLiteral("confirmCreateRecoveryPoint"));
        auto *cancelConfirmBtn = window->findChild<QQuickItem *>(QStringLiteral("cancelCreateRecoveryPoint"));
        auto *actionStatusLabel = window->findChild<QQuickItem *>(QStringLiteral("recoveryActionStatusLabel"));

        QVERIFY(descInput);
        QVERIFY(createBtn);
        QVERIFY(confirmDialog);
        QVERIFY(confirmBtn);
        QVERIFY(cancelConfirmBtn);
        QVERIFY(actionStatusLabel);

        // Step 1: Request creation -> confirmation dialog opens, operation not started yet
        QVERIFY(descInput->setProperty("text", QStringLiteral("UI verified snapshot")));
        QMetaObject::invokeMethod(createBtn, "clicked");
        QTest::qWait(20);
        QVERIFY(recovery.confirmationPending());
        QVERIFY(confirmDialog->property("visible").toBool());
        QVERIFY(!recovery.actionBusy());
        QVERIFY(!QFile::exists(paths.createLogFile));

        // Step 2: Cancel confirmation -> dialog closes, status shows cancelled
        QMetaObject::invokeMethod(cancelConfirmBtn, "clicked");
        QTest::qWait(20);
        QVERIFY(!recovery.confirmationPending());
        QVERIFY(!confirmDialog->property("visible").toBool());
        QCOMPARE(actionStatusLabel->property("text").toString(),
                 QStringLiteral("Recovery point creation cancelled."));

        // Step 3: Request again and confirm -> operation runs over D-Bus and updates UI
        QMetaObject::invokeMethod(createBtn, "clicked");
        QTest::qWait(20);
        QVERIFY(recovery.confirmationPending());
        QMetaObject::invokeMethod(confirmBtn, "clicked");

        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QTest::qWait(30);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("ok"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 26);
        QVERIFY(actionStatusLabel->property("text").toString().contains(QStringLiteral("#26")));
        QVERIFY(createBtn->property("enabled").toBool());

        // Verify D-Bus GetOperationState matches UI state
        const QVariantMap opState = callDbusGetOperationState(recovery.lastOperationId());
        QCOMPARE(opState.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(opState.value(QStringLiteral("code")).toString(), QStringLiteral("ok"));
        QCOMPARE(opState.value(QStringLiteral("snapshot_id")).toString(), QStringLiteral("26"));

        // Verify pkexec was never invoked by the UI
        QVERIFY(!QFile::exists(pkexecLog));

        QStringList qmlErrors;
        for (const auto &signal : warnings)
            for (const auto &warning : qvariant_cast<QList<QQmlError>>(signal.first()))
                qmlErrors.append(warning.toString());
        QVERIFY2(qmlErrors.isEmpty(), qPrintable(qmlErrors.join(QLatin1Char('\n'))));
    }

    // --- 03.2.03 Snapshot Listing Verification Scenarios (1..11) ---

    void listScenario1_multipleValidSnapshotsListed()
    {
        QTemporaryDir dir;
        const EnvPaths paths = setupStandardFixture(dir);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_and_configured"));
        QCOMPARE(recovery.snapshotCount(), 4);
        QCOMPARE(recovery.currentSnapshotId(), 0);
        QCOMPARE(recovery.recoveryEvents().size(), 1);
        QCOMPARE(recovery.recoveryPoints().size(), 1);
        QCOMPARE(recovery.recoveryPoints().first().toMap().value(QStringLiteral("id")).toInt(), 25);

        ZethropolRecoveryClient client;
        QVERIFY(client.lastError().isEmpty());
        QCOMPARE(client.recoveryPoints().size(), 3); // IDs 21, 22, 25
    }

    void listScenario2_emptySnapshotListNotFake()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral("echo '{\"root\":[]}'\nexit 0\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_and_configured"));
        QCOMPARE(recovery.snapshotCount(), 0);
        QVERIFY(recovery.snapshots().isEmpty());
        QVERIFY(recovery.recoveryEvents().isEmpty());
        QVERIFY(recovery.recoveryPoints().isEmpty());

        ZethropolRecoveryClient client;
        QVERIFY(client.lastError().isEmpty());
        QVERIFY(client.recoveryPoints().isEmpty());
    }

    void listScenario3_malformedOrMissingJsonErrorsNotEmpty()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral("printf '{\"wrong_key\": []}'\nexit 0\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(!recovery.available());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("invalid_configuration"));
        QVERIFY(recovery.snapshots().isEmpty());

        ZethropolRecoveryClient client;
        QVERIFY(!client.lastError().isEmpty());
        QVERIFY(client.recoveryPoints().isEmpty());
    }

    void listScenario4_snapperNonZeroExitCodeNotTreatedAsEmpty()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral("echo 'IO error (query default id failed).' >&2\nexit 1\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(!recovery.available());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("invalid_configuration"));
        QVERIFY(!recovery.statusMessage().isEmpty());
    }

    void listScenario5_permissionDeniedDistinctFromEmptyAndNoPkexec()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral("echo 'Failure (error.no_permissions).' >&2\nexit 1\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        QTemporaryDir emptyPathDir;
        const QString pkexecLog = dir.filePath(QStringLiteral("pkexec.log"));
        writeScript(emptyPathDir,
                    QStringLiteral("pkexec"),
                    "echo called >> '" + pkexecLog.toUtf8() + "'\nexit 1\n");
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", emptyPathDir.path().toUtf8());
        const auto restorePath = qScopeGuard([&] { qputenv("PATH", oldPath); });

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(!recovery.available());
        QVERIFY(recovery.permissionRequired());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("permission_denied"));

        ZethropolRecoveryClient client;
        QVERIFY(!client.lastError().isEmpty());
        QVERIFY(!QFile::exists(pkexecLog));
    }

    void listScenario6_missingSnapperConfigDistinctFromEmpty()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        QFile::remove(dir.filePath(QStringLiteral("etc/snapper/configs/root")));
        writeFile(dir, QStringLiteral("etc/conf.d/snapper"), QByteArrayLiteral("SNAPPER_CONFIGS=\"\"\n"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(!recovery.available());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_not_configured"));

        ZethropolRecoveryClient client;
        QVERIFY(!client.lastError().isEmpty());
    }

    void listScenario7_backendUnavailableDistinctFromEmpty()
    {
        stopRecoveryDaemon();
        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(!recovery.available());
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("backend_unavailable"));
        QVERIFY(recovery.statusMessage().contains(QStringLiteral("org.zethropol.Recovery")));

        ZethropolRecoveryClient client;
        QVERIFY(!client.lastError().isEmpty());
    }

    void listScenario8_duplicateInvalidOrMissingFieldsHandledSafely()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral(R"(cat << 'EOF'
{
  "root": [
    {"number": 0, "description": "current"},
    {"number": 41, "type": 999, "date": null, "userdata": "invalid"},
    {"number": 41, "type": "single", "description": "Duplicate ID 41"},
    {"number": 42, "type": "single", "description": "Valid entry"}
  ]
}
EOF
)"));
        updateEnvFile(paths);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());
        QCOMPARE(recovery.snapshotCount(), 3); // 0, 41, 42 (duplicate 41 dropped)
        QCOMPARE(recovery.recoveryPoints().size(), 2);
        QCOMPARE(recovery.recoveryPoints().at(0).toMap().value(QStringLiteral("id")).toInt(), 41);
        QCOMPARE(recovery.recoveryPoints().at(1).toMap().value(QStringLiteral("id")).toInt(), 42);
    }

    void listScenario9_consecutiveRefreshesAndAsyncErrorRecovery()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.snapshotCount(), 4);

        // Consecutive refresh calls must not hang or duplicate entries
        recovery.refresh();
        recovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.snapshotCount(), 4);
        QCOMPARE(recovery.recoveryPoints().size(), 1);

        // Simulate transient backend failure then recovery
        stopRecoveryDaemon();
        recovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("backend_unavailable"));
        QVERIFY(!recovery.available());
        QVERIFY(recovery.snapshots().isEmpty());

        startRecoveryDaemon(paths.envFile);
        recovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.detectionStatus(), QStringLiteral("installed_and_configured"));
        QVERIFY(recovery.available());
        QCOMPARE(recovery.snapshotCount(), 4);
    }

    void listScenario10_dbusAndQmlListConsistency()
    {
        QTemporaryDir dir;
        EnvPaths paths = setupStandardFixture(dir);
        startRecoveryDaemon(paths.envFile);

        HardwareServiceClient hardware;
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

        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QVERIFY(recovery.available());

        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        for (const auto &pair : QList<QPair<QString, QObject *>>{
                 {QStringLiteral("HardwareBridge"), &hardware},
                 {QStringLiteral("UpdateBridge"), &scanner},
                 {QStringLiteral("UpdateInstallBridge"), &installer},
                 {QStringLiteral("RecoveryBridge"), &recovery},
                 {QStringLiteral("SecurityBridge"), &security},
                 {QStringLiteral("ServiceBridge"), &services},
                 {QStringLiteral("ApplicationBridge"), &applications},
                 {QStringLiteral("ProcessBridge"), &processes},
                 {QStringLiteral("NotificationBridge"), &notifications},
                 {QStringLiteral("PowerBridge"), &power},
                 {QStringLiteral("MonitoringBridge"), &monitoring}}) {
            engine.rootContext()->setContextProperty(pair.first, pair.second);
        }

        engine.load(QUrl(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml")));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        auto *pages = window->findChild<QQuickItem *>(QStringLiteral("controlCenterPages"));
        QVERIFY(pages);
        QVERIFY(pages->setProperty("currentIndex", 7));
        QTest::qWait(40);

        auto *eventsList = window->findChild<QQuickItem *>(QStringLiteral("recoveryEventsList"));
        auto *pointsList = window->findChild<QQuickItem *>(QStringLiteral("recoveryPointsList"));
        auto *pointsEmpty = window->findChild<QQuickItem *>(QStringLiteral("recoveryPointsEmptyLabel"));
        auto *listError = window->findChild<QQuickItem *>(QStringLiteral("recoveryListErrorLabel"));
        QVERIFY(eventsList);
        QVERIFY(pointsList);
        QVERIFY(pointsEmpty);
        QVERIFY(listError);

        QCOMPARE(eventsList->property("count").toInt(), 1);
        QCOMPARE(pointsList->property("count").toInt(), 1);
        QVERIFY(pointsList->isVisible());
        QVERIFY(!pointsEmpty->isVisible());
        QVERIFY(!listError->isVisible());

        // Switch to empty snapshot list -> empty label visible, list hidden
        paths.snapperBin = writeScript(
            dir,
            QStringLiteral("bin/snapper"),
            QByteArrayLiteral("echo '{\"root\":[]}'\nexit 0\n"));
        updateEnvFile(paths);
        recovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QTest::qWait(30);
        QVERIFY(recovery.available());
        QCOMPARE(pointsList->property("count").toInt(), 0);
        QVERIFY(pointsEmpty->isVisible());
        QVERIFY(!listError->isVisible());

        // Switch to error state -> error label visible, empty label hidden
        stopRecoveryDaemon();
        recovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QTest::qWait(30);
        QVERIFY(!recovery.available());
        QVERIFY(listError->isVisible());
        QVERIFY(!pointsEmpty->isVisible());

        QStringList qmlErrors;
        for (const auto &signal : warnings)
            for (const auto &warning : qvariant_cast<QList<QQmlError>>(signal.first()))
                qmlErrors.append(warning.toString());
        QVERIFY2(qmlErrors.isEmpty(), qPrintable(qmlErrors.join(QLatin1Char('\n'))));
    }

    void listScenario11_refreshAfterSnapshotCreationRegression()
    {
        QTemporaryDir dir;
        const EnvPaths paths = setupStandardFixture(dir);
        startRecoveryDaemon(paths.envFile);

        RecoveryService recovery;
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.checking(), 5000);
        QCOMPARE(recovery.snapshotCount(), 4);
        QCOMPARE(recovery.recoveryPoints().size(), 1);

        recovery.requestCreateRecoveryPoint(QStringLiteral("Post-create list check"));
        recovery.confirmCreateRecoveryPoint();
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.actionBusy(), 5000);
        QCOMPARE(recovery.lastOperationCode(), QStringLiteral("ok"));
        QCOMPARE(recovery.lastCreatedSnapshotId(), 26);

        QTRY_COMPARE_WITH_TIMEOUT(recovery.snapshotCount(), 5, 5000);
        QCOMPARE(recovery.recoveryPoints().size(), 2);
        QCOMPARE(recovery.recoveryPoints().at(0).toMap().value(QStringLiteral("id")).toInt(), 25);
        QCOMPARE(recovery.recoveryPoints().at(1).toMap().value(QStringLiteral("id")).toInt(), 26);
    }
};

QTEST_MAIN(RecoveryTest)
#include "RecoveryTest.moc"
