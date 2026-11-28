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

        const char *extraOrphanPkg[] = {"zethropol-update-helper", "remove-orphans", "lua54"};
        QVERIFY(!validateArgs(3, extraOrphanPkg));

        const char *extraOrphanFlag[] = {"zethropol-update-helper", "remove-orphans", "--nodeps"};
        QVERIFY(!validateArgs(3, extraOrphanFlag));

        for (const char *bad : {
                 "",
                 "--noconfirm",
                 "--nodeps",
                 "--overwrite=*",
                 "-Syu",
                 "-Rns",
                 "apply-aur",
                 "../apply-official",
                 "apply-official;id",
                 "remove-orphans;id",
             }) {
            const char *argv[] = {"zethropol-update-helper", bad};
            QVERIFY2(!validateArgs(2, argv), bad);
        }

        const char *goodOfficial[] = {"zethropol-update-helper", "apply-official"};
        QVERIFY(validateArgs(2, goodOfficial));

        const char *goodOrphans[] = {"zethropol-update-helper", "remove-orphans"};
        QVERIFY(validateArgs(2, goodOrphans));

        QVERIFY(isValidPackageName("lua54"));
        QVERIFY(isValidPackageName("lib32-vulkan-icd-loader"));
        QVERIFY(!isValidPackageName(""));
        QVERIFY(!isValidPackageName("--nodeps"));
        QVERIFY(!isValidPackageName(".hidden"));
        QVERIFY(!isValidPackageName("../evil"));
        QVERIFY(!isValidPackageName("foo;id"));

        for (const char *protectedPkg : {
                 "",
                 "zethropol",
                 "zethropol-control-center",
                 "zethropol-recovery",
                 "zethropol-compatibility",
                 "linux-cachyos",
                 "systemd-libs",
                 "plasma-desktop",
                 "kwin",
                 "qt6-base",
                 "limine-snapper-sync",
                 "pacman",
                 "glibc",
                 "snapper",
                 "snap-pac",
                 "amd-ucode",
                 "cryptsetup",
             }) {
            QVERIFY2(isProtectedOrphanPackage(protectedPkg), protectedPkg);
        }
        QVERIFY(!isProtectedOrphanPackage("lua54"));
        QVERIFY(!isProtectedOrphanPackage("orphan-lib-a"));

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

    void removeOrphansFullChainFiltersProtectedAndUsesSanitizedEnv()
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
    *"-t pre"*"zethropol=pre-orphan-cleanup"*) echo "51" ;;
    *"-t post"*"--pre-number 51"*"zethropol=post-orphan-cleanup"*) echo "52" ;;
    *) exit 98 ;;
esac
)SH").arg(orderLog).toUtf8());

        writeScript("mock-pacman", QString(R"SH(
if [ -n "${SKIP_AUTOSNAP:-}" ] || [ -n "${SNAP_PAC_SKIP:-}" ]; then
    echo "ENV_LEAK" >&2
    exit 99
fi
printf 'pacman:%s\n' "$*" >> '%1'
if [ "$1" = "-Qdtq" ]; then
    printf 'orphan-a\nzethropol-recovery\npacman\norphan-b\nlinux-cachyos\n'
    exit 0
fi
if [ "$1" = "-Rs" ] && [ "$2" = "--print" ]; then
    printf 'orphan-a\norphan-b\n'
    exit 0
fi
if [ "$1" = "-Rns" ]; then
    printf 'checking dependencies...\n'
    printf ':: Processing package changes...\n'
    printf 'removing orphan-a...\n'
    printf 'removing orphan-b...\n'
    exit 0
fi
exit 97
)SH").arg(orderLog).toUtf8());

        writeScript("mock-limine-sync", QString(R"SH(
printf 'limine-sync:%s\n' "$*" >> '%1'
printf 'Updated: /boot/limine.conf\n'
exit 0
)SH").arg(orderLog).toUtf8());

        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "remove-orphans"};
        std::ostringstream out, err;
        const int rc = runHelper(2, argv, cfg, out, err);

        QCOMPARE(rc, kExitOk);
        const QString stdoutText = QString::fromStdString(out.str());
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=preflight STATUS=ok"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=orphan-filter STATUS=protected-skipped PACKAGES=zethropol-recovery,pacman,linux-cachyos"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=orphan-discovery STATUS=ok COUNT=2 PACKAGES=orphan-a,orphan-b"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=51"));
        QVERIFY(stdoutText.contains("removing orphan-a..."));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=orphan-cleanup STATUS=ok"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=post-snapshot STATUS=ok SNAPSHOT_ID=52 PRE_SNAPSHOT_ID=51"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=limine-sync STATUS=ok"));
        QVERIFY(stdoutText.contains("[ZETHROPOL-UPDATE] STAGE=complete STATUS=ok PRE_SNAPSHOT_ID=51 POST_SNAPSHOT_ID=52"));

        QFile f(orderLog);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 6);
        QCOMPARE(lines[0], QStringLiteral("pacman:-Qdtq"));
        QCOMPARE(lines[1], QStringLiteral("pacman:-Rs --print --print-format %n -- orphan-a orphan-b"));
        QVERIFY(lines[2].startsWith("snapper:-c root create -t pre -p"));
        QVERIFY(lines[2].contains("zethropol=pre-orphan-cleanup"));
        QCOMPARE(lines[3], QStringLiteral("pacman:-Rns --noconfirm -- orphan-a orphan-b"));
        QVERIFY(!lines[3].contains("--nodeps"));
        QVERIFY(!lines[3].contains("--overwrite"));
        QVERIFY(!lines[3].contains("--force"));
        QVERIFY(!lines[3].contains("zethropol-recovery"));
        QVERIFY(!lines[3].contains("linux-cachyos"));
        QVERIFY(lines[4].startsWith("snapper:-c root create -t post --pre-number 51 -p"));
        QVERIFY(lines[4].contains("zethropol=post-orphan-cleanup"));
        QVERIFY(lines[5].startsWith("limine-sync:"));
    }

    void removeOrphansBlocksRecursiveProtectedDependencyAndNoopsCleanly()
    {
        // 1. Recursive closure pulls in zethropol-diagnostics for risky-orphan, but safe-orphan stays clean
        const QString orderLog = m_dir.filePath("order.log");
        writeScript("mock-snapper", QString(R"SH(
printf 'snapper:%s\n' "$*" >> '%1'
case "$*" in
    *"-t pre"*) echo "61" ;;
    *"-t post"*"--pre-number 61"*) echo "62" ;;
    *) exit 98 ;;
esac
)SH").arg(orderLog).toUtf8());
        writeScript("mock-pacman", QString(R"SH(
printf 'pacman:%s\n' "$*" >> '%1'
if [ "$1" = "-Qdtq" ]; then
    printf 'risky-orphan\nsafe-orphan\n'
    exit 0
fi
if [ "$1" = "-Rs" ] && [ "$2" = "--print" ]; then
    case "$*" in
        *"risky-orphan"*)
            printf 'risky-orphan\nzethropol-diagnostics\n'
            exit 0
            ;;
        *"safe-orphan"*)
            printf 'safe-orphan\n'
            exit 0
            ;;
    esac
fi
if [ "$1" = "-Rns" ]; then
    printf 'removing safe-orphan...\n'
    exit 0
fi
exit 97
)SH").arg(orderLog).toUtf8());
        writeScript("mock-limine-sync", "exit 0\n");

        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "remove-orphans"};
        std::ostringstream out, err;
        const int rc = runHelper(2, argv, cfg, out, err);
        QCOMPARE(rc, kExitOk);
        const QString stdoutText = QString::fromStdString(out.str());
        QVERIFY(stdoutText.contains("STAGE=orphan-filter STATUS=protected-skipped REASON=recursive-dependency PACKAGES=risky-orphan"));
        QVERIFY(stdoutText.contains("STAGE=orphan-discovery STATUS=ok COUNT=1 PACKAGES=safe-orphan"));

        QFile f(orderLog);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString logContent = QString::fromUtf8(f.readAll());
        QVERIFY(logContent.contains("pacman:-Rns --noconfirm -- safe-orphan\n"));
        QVERIFY(!logContent.contains("pacman:-Rns --noconfirm -- risky-orphan"));

        // 2. Empty pacman -Qdtq (exit 1) -> noop without snapshot or removal
        const QString snapperMarker = m_dir.filePath("snapper-ran");
        const QString pacmanRemoveMarker = m_dir.filePath("pacman-ran");
        QFile::remove(snapperMarker);
        QFile::remove(pacmanRemoveMarker);
        writeScript("mock-snapper", QString("touch '%1'\necho 90\n").arg(snapperMarker).toUtf8());
        writeScript("mock-pacman", QString(R"SH(
if [ "$1" = "-Qdtq" ]; then
    exit 1
fi
touch '%1'
exit 0
)SH").arg(pacmanRemoveMarker).toUtf8());
        std::ostringstream outEmpty, errEmpty;
        QCOMPARE(runHelper(2, argv, cfg, outEmpty, errEmpty), kExitOk);
        QVERIFY(QString::fromStdString(outEmpty.str()).contains("STAGE=complete STATUS=noop"));
        QVERIFY(!QFile::exists(snapperMarker));
        QVERIFY(!QFile::exists(pacmanRemoveMarker));

        // 3. Protected-only orphans -> noop without snapshot or removal
        writeScript("mock-pacman", QString(R"SH(
if [ "$1" = "-Qdtq" ]; then
    printf 'zethropol-tv\nglibc\n'
    exit 0
fi
touch '%1'
exit 0
)SH").arg(pacmanRemoveMarker).toUtf8());
        std::ostringstream outProt, errProt;
        QCOMPARE(runHelper(2, argv, cfg, outProt, errProt), kExitOk);
        const QString protText = QString::fromStdString(outProt.str());
        QVERIFY(protText.contains("STAGE=orphan-filter STATUS=protected-skipped PACKAGES=zethropol-tv,glibc"));
        QVERIFY(protText.contains("STAGE=complete STATUS=noop"));
        QVERIFY(!QFile::exists(snapperMarker));
        QVERIFY(!QFile::exists(pacmanRemoveMarker));
    }

    void removeOrphansEnforcesDbLockFailClosedPreSnapshotAndPacmanFailure()
    {
        const HelperConfig cfg = baseConfig();
        const char *argv[] = {"zethropol-update-helper", "remove-orphans"};
        const QString snapperMarker = m_dir.filePath("snapper-ran");
        const QString pacmanMarker = m_dir.filePath("pacman-ran");

        // 1. db.lck blocks without deleting lock or touching snapper/pacman
        const QString lockPath = m_dir.filePath("db.lck");
        {
            QFile lockFile(lockPath);
            QVERIFY(lockFile.open(QIODevice::WriteOnly));
        }
        writeScript("mock-snapper", QString("touch '%1'\necho 10\n").arg(snapperMarker).toUtf8());
        writeScript("mock-pacman", QString("touch '%1'\nexit 0\n").arg(pacmanMarker).toUtf8());
        writeScript("mock-limine-sync", "exit 0\n");
        std::ostringstream outLock, errLock;
        QCOMPARE(runHelper(2, argv, cfg, outLock, errLock), kExitPacmanLocked);
        QVERIFY(QFile::exists(lockPath));
        QVERIFY(!QFile::exists(snapperMarker));
        QVERIFY(!QFile::exists(pacmanMarker));
        QFile::remove(lockPath);

        // 2. Fail-closed pre-snapshot abort never calls pacman -Rns
        writeScript("mock-snapper", "echo 'snapper failed' >&2\nexit 1\n");
        writeScript("mock-pacman", QString(R"SH(
if [ "$1" = "-Qdtq" ]; then
    printf 'orphan-x\n'
    exit 0
fi
if [ "$1" = "-Rs" ]; then
    printf 'orphan-x\n'
    exit 0
fi
touch '%1'
exit 0
)SH").arg(pacmanMarker).toUtf8());
        std::ostringstream outPre, errPre;
        QCOMPARE(runHelper(2, argv, cfg, outPre, errPre), kExitPreSnapshotFailed);
        QVERIFY(!QFile::exists(pacmanMarker));
        QVERIFY(QString::fromStdString(outPre.str()).contains("STAGE=pre-snapshot STATUS=failed"));

        // 3. pacman -Rns failure still creates paired post-snapshot and returns kExitPacmanFailed
        writeScript("mock-snapper", R"SH(
case "$*" in
    *"-t pre"*) echo "88" ;;
    *"-t post"*"--pre-number 88"*"(failed)"*) echo "89" ;;
    *) exit 98 ;;
esac
)SH");
        writeScript("mock-pacman", R"SH(
if [ "$1" = "-Qdtq" ]; then
    printf 'orphan-x\n'
    exit 0
fi
if [ "$1" = "-Rs" ]; then
    printf 'orphan-x\n'
    exit 0
fi
printf 'error: failed to commit transaction\n'
exit 1
)SH");
        std::ostringstream outFail, errFail;
        QCOMPARE(runHelper(2, argv, cfg, outFail, errFail), kExitPacmanFailed);
        const QString failText = QString::fromStdString(outFail.str());
        QVERIFY(failText.contains("STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=88"));
        QVERIFY(failText.contains("STAGE=orphan-cleanup STATUS=failed EXIT_CODE=1"));
        QVERIFY(failText.contains("STAGE=post-snapshot STATUS=ok SNAPSHOT_ID=89 PRE_SNAPSHOT_ID=88"));
        QVERIFY(failText.contains("STAGE=complete STATUS=failed PRE_SNAPSHOT_ID=88"));
    }

    void polkitPolicyEnforcesExecPathAndAuthAdmin()
    {
        QFile f(QStringLiteral(UPDATE_POLKIT_POLICY));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), UPDATE_POLKIT_POLICY);
        const QString xml = QString::fromUtf8(f.readAll());
        QVERIFY(xml.contains(QStringLiteral("<action id=\"org.zethropol.updates.apply\">")));
        QVERIFY(xml.contains(QStringLiteral("<annotate key=\"org.freedesktop.policykit.exec.path\">/usr/lib/zethropol/zethropol-update-helper</annotate>")));
        QVERIFY(xml.contains(QStringLiteral("orphan packages")));
        QVERIFY(xml.contains(QStringLiteral("<allow_any>auth_admin</allow_any>")));
        QVERIFY(xml.contains(QStringLiteral("<allow_inactive>auth_admin</allow_inactive>")));
        QVERIFY(xml.contains(QStringLiteral("<allow_active>auth_admin</allow_active>")));
        QVERIFY(!xml.contains(QStringLiteral("/usr/libexec/")));
    }
};

QTEST_GUILESS_MAIN(UpdateHelperTest)
#include "UpdateHelperTest.moc"
