#include <QFile>
#include <QtTest>

#include "ApplicationService.h"
#include "RecoveryService.h"
#include "ServiceService.h"

class SecurityRemediationTest : public QObject
{
    Q_OBJECT

private slots:
    void serviceValidationBlocksInjectionQuarantinedAndCriticalServices()
    {
        QString reason;
        for (const QString &badUnit : {
                 QStringLiteral(""),
                 QStringLiteral("--now"),
                 QStringLiteral("--root=/tmp"),
                 QStringLiteral("-f.service"),
                 QStringLiteral("/tmp/evil.service"),
                 QStringLiteral("../evil.service"),
                 QStringLiteral("reboot.target"),
                 QStringLiteral("foo@.service"),
                 QStringLiteral("cups.service;id")
             }) {
            QVERIFY2(!ServiceService::validateServiceRequest(badUnit, QStringLiteral("start"), &reason),
                     qPrintable(badUnit));
        }

        // zethropol-performance.service quarantine was lifted after live 0.1.0-2 verification
        QVERIFY(!ServiceService::isQuarantinedService(QStringLiteral("zethropol-performance.service")));
        for (const QString &act : {QStringLiteral("start"), QStringLiteral("stop"),
                                   QStringLiteral("restart"), QStringLiteral("enable"),
                                   QStringLiteral("disable")}) {
            QVERIFY(ServiceService::validateServiceRequest(
                QStringLiteral("zethropol-performance.service"), act, &reason));
        }

        // Critical system services blocked for stop/disable/restart
        for (const QString &unit : {QStringLiteral("dbus-broker.service"),
                                    QStringLiteral("systemd-journald.service"),
                                    QStringLiteral("polkit.service"),
                                    QStringLiteral("zethropol-recovery.service"),
                                    QStringLiteral("plasmalogin.service")}) {
            for (const QString &act : {QStringLiteral("stop"), QStringLiteral("disable"), QStringLiteral("restart")}) {
                QVERIFY(!ServiceService::validateServiceRequest(unit, act, &reason));
            }
            QVERIFY(ServiceService::validateServiceRequest(unit, QStringLiteral("start"), &reason));
        }

        QVERIFY(ServiceService::validateServiceRequest(QStringLiteral("bluetooth.service"), QStringLiteral("restart")));
    }

    void applicationProtectionAndLocaleInvariantOwnerParsing()
    {
        for (const QString &protectedPkg : {
                 QStringLiteral("zethropol-control-center"),
                 QStringLiteral("zethropol-diagnostics"),
                 QStringLiteral("zethropol-recovery"),
                 QStringLiteral("pacman"),
                 QStringLiteral("glibc"),
                 QStringLiteral("systemd"),
                 QStringLiteral("plasma-desktop"),
                 QStringLiteral("kwin"),
                 QStringLiteral("konsole"),
                 QStringLiteral("dolphin"),
                 QStringLiteral("snapper"),
                 QStringLiteral("snap-pac"),
                 QStringLiteral("linux-cachyos")
             }) {
            QVERIFY2(ApplicationService::isProtectedPackage(protectedPkg), qPrintable(protectedPkg));
        }

        QVERIFY(!ApplicationService::isProtectedPackage(QStringLiteral("vlc")));
        QVERIFY(!ApplicationService::isValidPackageName(QStringLiteral("--nodeps")));
        QVERIFY(!ApplicationService::isValidPackageName(QStringLiteral("../evil")));
        QVERIFY(ApplicationService::isValidPackageName(QStringLiteral("vlc")));

        const QString sampleOut = QStringLiteral(
            "/usr/share/applications/vlc.desktop is owned by vlc 3.0.21-1\n"
            "/usr/share/applications/zethropol-control-center.desktop is owned by zethropol-control-center 0.1.0-2\n");
        const auto owners = ApplicationService::parsePacmanOwnerOutput(sampleOut);
        QCOMPARE(owners.value(QStringLiteral("/usr/share/applications/vlc.desktop")), QStringLiteral("vlc"));
        QCOMPARE(owners.value(QStringLiteral("/usr/share/applications/zethropol-control-center.desktop")),
                 QStringLiteral("zethropol-control-center"));

        ApplicationService appSvc;
        // Unconfirmed removal must always be rejected
        appSvc.removeApplication(QStringLiteral("vlc"), false);
        QVERIFY(!appSvc.removing());
        QVERIFY(appSvc.removeStatus().contains(QStringLiteral("onay")));

        // Protected package must be rejected even if userConfirmed == true
        appSvc.removeApplication(QStringLiteral("zethropol-control-center"), true);
        QVERIFY(!appSvc.removing());
        QVERIFY(appSvc.removeStatus().contains(QStringLiteral("kaldırılamaz")));
    }

    void recoveryDeleteProtectionAndTwoStepConfirmation()
    {
        QString reason;
        QVERIFY(RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 0}}, 0, &reason));
        QVERIFY(RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 5}, {QStringLiteral("isCurrent"), true}}, 5, &reason));
        QVERIFY(RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 10}, {QStringLiteral("important"), true}}, 0, &reason));
        QVERIFY(RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 11}, {QStringLiteral("zethropolTag"), QStringLiteral("pre-update")}}, 0, &reason));
        QVERIFY(RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 12}, {QStringLiteral("isPre"), true}}, 0, &reason));
        QVERIFY(RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 13}, {QStringLiteral("pairedSnapshotId"), 12}}, 0, &reason));
        QVERIFY(!RecoveryService::isSnapshotProtectedForDeletion(
            QVariantMap{{QStringLiteral("id"), 14}, {QStringLiteral("description"), QStringLiteral("Manual point")}}, 0, &reason));

        RecoveryService svc;
        svc.applyStateMap(QVariantMap{
            {QStringLiteral("detectionStatus"), QStringLiteral("installed_and_configured")},
            {QStringLiteral("available"), true},
            {QStringLiteral("currentSnapshotId"), 0},
            {QStringLiteral("snapshots"), QVariantList{
                QVariantMap{{QStringLiteral("id"), 10}, {QStringLiteral("important"), true}, {QStringLiteral("description"), QStringLiteral("Important")}},
                QVariantMap{{QStringLiteral("id"), 11}, {QStringLiteral("zethropolTag"), QStringLiteral("pre-update")}, {QStringLiteral("description"), QStringLiteral("Pre-Update")}},
                QVariantMap{{QStringLiteral("id"), 14}, {QStringLiteral("important"), false}, {QStringLiteral("description"), QStringLiteral("Manual")}}
            }},
            {QStringLiteral("recoveryPoints"), QVariantList{
                QVariantMap{{QStringLiteral("id"), 10}, {QStringLiteral("important"), true}, {QStringLiteral("description"), QStringLiteral("Important")}},
                QVariantMap{{QStringLiteral("id"), 11}, {QStringLiteral("zethropolTag"), QStringLiteral("pre-update")}, {QStringLiteral("description"), QStringLiteral("Pre-Update")}},
                QVariantMap{{QStringLiteral("id"), 14}, {QStringLiteral("important"), false}, {QStringLiteral("description"), QStringLiteral("Manual")}}
            }}
        });

        // Protected points marked deletable=false and rejected by requestDeleteRecoveryPoint
        QCOMPARE(svc.recoveryPoints().size(), 3);
        QVERIFY(!svc.recoveryPoints().at(0).toMap().value(QStringLiteral("deletable")).toBool());
        QVERIFY(!svc.recoveryPoints().at(1).toMap().value(QStringLiteral("deletable")).toBool());
        QVERIFY(svc.recoveryPoints().at(2).toMap().value(QStringLiteral("deletable")).toBool());

        svc.requestDeleteRecoveryPoint(10);
        QVERIFY(!svc.deleteConfirmationPending());
        QCOMPARE(svc.lastOperationCode(), QStringLiteral("protected-snapshot"));

        svc.requestDeleteRecoveryPoint(11);
        QVERIFY(!svc.deleteConfirmationPending());
        QCOMPARE(svc.lastOperationCode(), QStringLiteral("protected-snapshot"));

        // Direct deleteRecoveryPoint(14) only opens confirmation; never launches immediate deletion
        svc.deleteRecoveryPoint(14);
        QVERIFY(svc.deleteConfirmationPending());
        QCOMPARE(svc.pendingDeleteSnapshotId(), 14);
        QVERIFY(!svc.actionBusy());

        svc.cancelDeleteRecoveryPoint();
        QVERIFY(!svc.deleteConfirmationPending());
        QCOMPARE(svc.pendingDeleteSnapshotId(), 0);
    }

    void qmlFilesHaveZeroUnescapedRichTextInjection()
    {
        for (const QString &qmlPath : {QStringLiteral(PROCESSES_PAGE_QML), QStringLiteral(MAIN_QML)}) {
            QFile f(qmlPath);
            QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(qmlPath));
            const QString content = QString::fromUtf8(f.readAll());
            QVERIFY2(!content.contains(QStringLiteral("Text.RichText")),
                     qPrintable(QStringLiteral("Found unescaped Text.RichText in %1").arg(qmlPath)));
        }
    }
};

QTEST_GUILESS_MAIN(SecurityRemediationTest)
#include "SecurityRemediationTest.moc"
