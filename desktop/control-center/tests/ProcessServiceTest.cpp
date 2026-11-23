#include <QtTest>
#include <QProcess>
#include <QCoreApplication>
#include "ProcessService.h"

class ProcessServiceTest : public QObject {
    Q_OBJECT
private slots:
    void criticalProcessProtected() {
        ProcessService service;
        int pid = static_cast<int>(QCoreApplication::applicationPid());
        QVERIFY(service.isCritical(pid));
        QVERIFY(!service.canTerminate(pid));
        QVERIFY(!service.terminateProcess(pid, 0));
        QVERIFY(!service.forceKillProcess(pid, 0, true));
    }

    void invalidPidRejected() {
        ProcessService service;
        QVERIFY(!service.terminateProcess(-1, 0));
        QVERIFY(!service.forceKillProcess(-1, 0, true));
    }

    void forceKillNeedsConfirmation() {
        ProcessService service;
        QVERIFY(!service.forceKillProcess(-1, 0, false));
        QVERIFY(service.statusMessage().contains("onaylanmalıdır"));
    }

    void processIdentityProtected() {
        QProcess child;
        child.start("/usr/bin/sleep", {"30"});
        QVERIFY(child.waitForStarted());

        ProcessService service;
        int pid = static_cast<int>(child.processId());
        quint64 start = service.processStartTime(pid);
        QVERIFY(start > 0);

        QVERIFY(!service.terminateProcess(pid, 0));
        QVERIFY(!service.terminateProcess(pid, start + 1));
        QVERIFY(!service.forceKillProcess(pid, start + 1, true));
        QCOMPARE(child.state(), QProcess::Running);

        child.kill();
        QVERIFY(child.waitForFinished());
    }

    void memoryAndProcfsParsingAreAccurate() {
        ProcessService service;
        QVERIFY(service.totalMemoryBytes() > 0);
        QVERIFY(service.usedMemoryBytes() > 0);
        QVERIFY(service.usedMemoryFormatted() != QStringLiteral("0 KB"));

        const int selfPid = static_cast<int>(QCoreApplication::applicationPid());
        const QVariantMap details = service.getProcessDetails(selfPid);
        QCOMPARE(details.value(QStringLiteral("pid")).toInt(), selfPid);
        const QString selfMemFmt = details.value(QStringLiteral("memoryFormatted")).toString();
        QVERIFY(selfMemFmt != QStringLiteral("0 KB"));
        QVERIFY(selfMemFmt.endsWith(QStringLiteral(" KB"))
                || selfMemFmt.endsWith(QStringLiteral(" MB"))
                || selfMemFmt.endsWith(QStringLiteral(" GB")));
        QVERIFY(details.value(QStringLiteral("memoryPercent")).toDouble() >= 0.0);

        qint64 selfRssKb = 0;
        for (const QVariant &v : service.processes()) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("pid")).toInt() == selfPid) {
                selfRssKb = m.value(QStringLiteral("memoryRaw")).toLongLong();
                break;
            }
        }
        QVERIFY(selfRssKb > 0);

        // Kernel thread (PID 2) has 0 KB RSS safely; invalid PID returns empty details safely
        const QVariantMap kthread = service.getProcessDetails(2);
        if (!kthread.isEmpty()) {
            QCOMPARE(kthread.value(QStringLiteral("memoryFormatted")).toString(), QStringLiteral("0 KB"));
        }
        QVERIFY(service.getProcessDetails(-1).isEmpty());
    }

    void truncatedCommAndDesktopSessionProcessesProtected() {
        // 15-byte kernel TASK_COMM_LEN truncated names MUST match protection
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("systemd-journal")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("systemd-resolve")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("polkit-kde-auth")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("startplasma-way")));
        // Full names and exe/argv0 basenames MUST match protection
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("kwin_wayland")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("plasmashell")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("pipewire")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("wireplumber")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("unknown"), QStringLiteral("/usr/lib/systemd/systemd-journald")));
        QVERIFY(ProcessService::isCriticalProcessIdentity(QStringLiteral("unknown"), QStringLiteral("/usr/bin/kwin_wayland (deleted)")));
        // Ordinary user tools remain manageable
        QVERIFY(!ProcessService::isCriticalProcessIdentity(QStringLiteral("sleep"), QStringLiteral("/usr/bin/sleep")));
    }

    void treeProcessesHasNoDuplicatePids() {
        ProcessService service;
        const QVariantList tree = service.treeProcesses();
        QVERIFY(!tree.isEmpty());
        QSet<int> seen;
        for (const QVariant &v : tree) {
            const int pid = v.toMap().value(QStringLiteral("pid")).toInt();
            QVERIFY2(!seen.contains(pid), qPrintable(QStringLiteral("Duplicate PID in tree: %1").arg(pid)));
            seen.insert(pid);
        }
    }

    void isolatedTerminateAndForceKillFlow() {
        // 1. Clean SIGTERM on normal sleep process
        QProcess normalChild;
        normalChild.start(QStringLiteral("/usr/bin/sleep"), {QStringLiteral("30")});
        QVERIFY(normalChild.waitForStarted());
        ProcessService service;
        const int normalPid = static_cast<int>(normalChild.processId());
        const quint64 normalStart = service.processStartTime(normalPid);
        QVERIFY(service.canTerminate(normalPid));
        QVERIFY(service.terminateProcess(normalPid, normalStart));
        QVERIFY(normalChild.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!service.actionBusy(), 3000);

        // 2. Child ignoring SIGTERM triggers processRequiresForceKill and allows confirmed SIGKILL
        QProcess stubbornChild;
        stubbornChild.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("trap '' TERM; while true; do sleep 1; done")});
        QVERIFY(stubbornChild.waitForStarted());
        QTest::qWait(150);
        const int stubbornPid = static_cast<int>(stubbornChild.processId());
        const quint64 stubbornStart = service.processStartTime(stubbornPid);
        QSignalSpy forceKillSpy(&service, &ProcessService::processRequiresForceKill);
        QVERIFY(service.terminateProcess(stubbornPid, stubbornStart));
        QTRY_COMPARE_WITH_TIMEOUT(forceKillSpy.count(), 1, 4000);
        QVERIFY(!service.forceKillProcess(stubbornPid, stubbornStart, false));
        QVERIFY(service.forceKillProcess(stubbornPid, stubbornStart, true));
        QVERIFY(stubbornChild.waitForFinished(3000));
    }
};

QTEST_GUILESS_MAIN(ProcessServiceTest)
#include "ProcessServiceTest.moc"
