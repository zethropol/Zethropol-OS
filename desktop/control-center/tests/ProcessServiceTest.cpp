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
};

QTEST_GUILESS_MAIN(ProcessServiceTest)
#include "ProcessServiceTest.moc"
