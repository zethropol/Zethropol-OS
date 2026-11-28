#include "UpdateHelperCore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sstream>

using namespace zethropol::update_helper;

class UpdateHelperTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;

    void writeScript(const QString &name, const QByteArray &body)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("#!/bin/sh\n" + body);
        f.close();
        QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    HelperConfig baseConfig()
    {
        HelperConfig cfg;
        cfg.pacmanLockPath = m_dir.filePath("db.lck").toStdString();
        cfg.snapperBin = m_dir.filePath("mock-snapper").toStdString();
        cfg.pacmanBin = m_dir.filePath("mock-pacman").toStdString();
        cfg.limineSyncBin = m_dir.filePath("mock-limine-sync").toStdString();
        cfg.snapperConfig = "root";
        cfg.requireRoot = false;
        return cfg;
    }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QFile::remove(m_dir.filePath("db.lck"));
        QFile::remove(m_dir.filePath("order.log"));
        QFile::remove(m_dir.filePath("pacman-ran"));
        QFile::remove(m_dir.filePath("snapper-ran"));
    }

    void rejectsInvalidArgumentsAndFlags()
    {
        const char *onlyProg[] = {"zethropol-update-helper"};
        QVERIFY(!validateArgs(1, onlyProg));

        const char *extraArg[] = {"zethropol-update-helper", "apply-official", "--nodeps"};
        QVERIFY(!validateArgs(3, extraArg));

        for (const char *bad : {
                 "",
                 "--noconfirm",
                 "--nodeps",
                 "--overwrite=*",
                 "-Syu",
                 "apply-aur",
                 "../apply-official",
                 "apply-official;id",
             }) {
            const char *argv[] = {"zethropol-update-helper", bad};
            QVERIFY2(!validateArgs(2, argv), bad);
        }

        const char *good[] = {"zethropol-update-helper", "apply-official"};
        QVERIFY(validateArgs(2, good));

        std::uint32_t parsed = 0;
        QVERIFY(!parseSnapshotId("", &parsed));
        QVERIFY(!parseSnapshotId("0", &parsed));
        QVERIFY(!parseSnapshotId("-5", &parsed));
        QVERIFY(!parseSnapshotId("42abc", &parsed));
        QVERIFY(parseSnapshotId("  348 \n", &parsed));
        QCOMPARE(parsed, 348u);
    }

    void blocksWhenPacmanLockPresentWithoutDeletingLock()
    {
        const QString lockPath = m_dir.filePath("db.lck");
        {
            QFile lockFile(lockPath);
            QVERIFY(lockFile.open(QIODevice::WriteOnly));
            lockFile.write("");
        }

        const QString snapperMarker = m_dir.filePath("snapper-ran");
        const QString pacmanMarker = m_dir.filePath("pacman-ran");
        writeScript("mock-snapper", QString("touch '%1'\necho 10\n").arg(snapperMarker).toUtf8());
        writeScript("mock-pacman", QString("touch '%1'\nexit 0\n").arg(pacmanMarker).toUtf8());
        writeScript("mock-limine-sync", "exit 0\n");

        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "apply-official"};
        std::ostringstream out, err;
        const int rc = runHelper(2, argv, cfg, out, err);

        QCOMPARE(rc, kExitPacmanLocked);
        QVERIFY(QFile::exists(lockPath));
        QVERIFY(!QFile::exists(snapperMarker));
        QVERIFY(!QFile::exists(pacmanMarker));
        QVERIFY(QString::fromStdString(out.str()).contains("[ZETHROPOL-UPDATE] STAGE=blocked REASON=pacman-lock"));
    }

    void abortsFailClosedWhenPreSnapshotFails()
    {
        const QString pacmanMarker = m_dir.filePath("pacman-ran");
        writeScript("mock-snapper", "echo 'snapper error' >&2\nexit 1\n");
        writeScript("mock-pacman", QString("touch '%1'\nexit 0\n").arg(pacmanMarker).toUtf8());
        writeScript("mock-limine-sync", "exit 0\n");

        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "apply-official"};
        std::ostringstream out, err;
        const int rc = runHelper(2, argv, cfg, out, err);

        QCOMPARE(rc, kExitPreSnapshotFailed);
        QVERIFY(!QFile::exists(pacmanMarker));
        QVERIFY(QString::fromStdString(out.str()).contains("[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=failed"));

        // Non-numeric snapper output must also fail closed without running pacman
        writeScript("mock-snapper", "echo 'not-a-number'\nexit 0\n");
        std::ostringstream out2, err2;
        const int rc2 = runHelper(2, argv, cfg, out2, err2);
        QCOMPARE(rc2, kExitPreSnapshotFailed);
        QVERIFY(!QFile::exists(pacmanMarker));
    }

    void executesFullPrePacmanPostLimineChainInOrderWithSanitizedEnv()
    {
        qputenv("SKIP_AUTOSNAP", "1");
        qputenv("SNAP_PAC_SKIP", "1");
        const auto envGuard = qScopeGuard([] {
            qunsetenv("SKIP_AUTOSNAP");
            qunsetenv("SNAP_PAC_SKIP");
        });

        const QString orderLog = m_dir.filePath("order.log");
        writeScript("mock-snapper", QString(R"SH(
if [ -n "${SKIP_AUTOSNAP:-}" ] || [ -n "${SNAP_PAC_SKIP:-}" ]; then
    echo "ENV_LEAK" >&2
    exit 99
fi
printf 'snapper:%s\n' "$*" >> '%1'
case "$*" in
    *"-t pre"*) echo "42" ;;
    *"-t post"*"--pre-number 42"*) echo "43" ;;
    *) exit 98 ;;
esac
)SH").arg(orderLog).toUtf8());

        writeScript("mock-pacman", QString(R"SH(
if [ -n "${SKIP_AUTOSNAP:-}" ] || [ -n "${SNAP_PAC_SKIP:-}" ]; then
    echo "ENV_LEAK" >&2
    exit 99
fi
printf 'pacman:%s\n' "$*" >> '%1'
printf ':: Synchronizing package databases...\n'
printf ':: Starting full system upgrade...\n'
printf ':: Processing package changes...\n'
exit 0
)SH").arg(orderLog).toUtf8());

        writeScript("mock-limine-sync", QString(R"SH(
printf 'limine-sync:%s\n' "$*" >> '%1'
printf 'Updated: /boot/limine.conf\n'
exit 0
)SH").arg(orderLog).toUtf8());

        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "apply-official"};
        std::ostringstream out, err;
        const int rc = runHelper(2, argv, cfg, out, err);

        QCOMPARE(rc, kExitOk);
        const QString stdoutText = QString::fromStdString(out.str());
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=preflight STATUS=ok"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=42"));
        QVERIFY(stdoutText.contains(":: Synchronizing package databases..."));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=pacman-upgrade STATUS=ok"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=post-snapshot STATUS=ok SNAPSHOT_ID=43 PRE_SNAPSHOT_ID=42"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=limine-sync STATUS=ok"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=complete STATUS=ok PRE_SNAPSHOT_ID=42 POST_SNAPSHOT_ID=43"));

        QFile f(orderLog);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines[0].startsWith("snapper:-c root create -t pre -p"));
        QCOMPARE(lines[1], QStringLiteral("pacman:-Syu --noconfirm"));
        QVERIFY(!lines[1].contains("--nodeps"));
        QVERIFY(!lines[1].contains("--overwrite"));
        QVERIFY(lines[2].startsWith("snapper:-c root create -t post --pre-number 42 -p"));
        QVERIFY(lines[3].startsWith("limine-sync:"));
    }

    void preservesPreSnapshotAndReturnsFailureWhenPacmanFails()
    {
        const QString orderLog = m_dir.filePath("order.log");
        writeScript("mock-snapper", QString(R"SH(
printf 'snapper:%s\n' "$*" >> '%1'
case "$*" in
    *"-t pre"*) echo "77" ;;
    *"-t post"*"--pre-number 77"*) echo "78" ;;
    *) exit 98 ;;
esac
)SH").arg(orderLog).toUtf8());
        writeScript("mock-pacman", "printf 'error: failed to commit transaction (conflicting files)\\n'\nexit 1\n");
        writeScript("mock-limine-sync", "exit 0\n");

        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "apply-official"};
        std::ostringstream out, err;
        const int rc = runHelper(2, argv, cfg, out, err);

        QCOMPARE(rc, kExitPacmanFailed);
        const QString stdoutText = QString::fromStdString(out.str());
        QVERIFY(stdoutText.contains("STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=77"));
        QVERIFY(stdoutText.contains("STAGE=pacman-upgrade STATUS=failed EXIT_CODE=1"));
        QVERIFY(stdoutText.contains("STAGE=post-snapshot STATUS=ok SNAPSHOT_ID=78 PRE_SNAPSHOT_ID=77"));
        QVERIFY(stdoutText.contains("STAGE=complete STATUS=failed PRE_SNAPSHOT_ID=77"));
    }

    void polkitPolicyEnforcesExecPathAndAuthAdmin()
    {
        QFile f(QStringLiteral(UPDATE_POLKIT_POLICY));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), UPDATE_POLKIT_POLICY);
        const QString xml = QString::fromUtf8(f.readAll());
        QVERIFY(xml.contains(QStringLiteral("<action id=\"org.zethropol.updates.apply\">")));
        QVERIFY(xml.contains(QStringLiteral("<annotate key=\"org.freedesktop.policykit.exec.path\">/usr/lib/zethropol/zethropol-update-helper</annotate>")));
        QVERIFY(xml.contains(QStringLiteral("<allow_any>auth_admin</allow_any>")));
        QVERIFY(xml.contains(QStringLiteral("<allow_inactive>auth_admin</allow_inactive>")));
        QVERIFY(xml.contains(QStringLiteral("<allow_active>auth_admin</allow_active>")));
        QVERIFY(!xml.contains(QStringLiteral("/usr/libexec/")));
    }
};

QTEST_GUILESS_MAIN(UpdateHelperTest)
#include "UpdateHelperTest.moc"
