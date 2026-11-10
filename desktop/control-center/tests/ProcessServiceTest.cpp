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
};

QTEST_GUILESS_MAIN(ProcessServiceTest)
#include "ProcessServiceTest.moc"
