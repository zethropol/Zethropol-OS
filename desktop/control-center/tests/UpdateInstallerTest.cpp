#include "UpdateInstaller.h"
#include "UpdateScanner.h"

#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class UpdateInstallerTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    QByteArray previousPath;
    QByteArray previousConfig;
    bool hadConfig = false;

    void script(const QString &name, const QByteArray &body)
    {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n" + body);
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    void setup(UpdateInstaller &installer)
    {
        installer.m_updater = directory.filePath("updater");
        // Existing PTY mock tests opt out of the Recovery D-Bus pre-gate and enable mock updater execution.
        installer.m_recoveryGateEnabled = false;
        installer.m_liveExecutionEnabled = true;
    }

private slots:
    void init()
    {
        previousPath = qgetenv("PATH");
        previousConfig = qgetenv("XDG_CONFIG_HOME");
        hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("PATH", directory.path().toUtf8() + ':' + previousPath);
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        script("checkupdates", "printf 'pkg 1 -> 2\\n'\n");
        script("paru", "exit 0\n");
        script("flatpak", "exit 0\n");
        script("updater", "printf 'Paket kontrolü tamamlandı.\\n'\nexit 0\n");
    }

    void cleanup()
    {
        qputenv("PATH", previousPath);
        if (hadConfig) qputenv("XDG_CONFIG_HOME", previousConfig);
        else qunsetenv("XDG_CONFIG_HOME");
    }

    void embeddedSessionProvidesATtyAndProtectsEnteredPasswords()
    {
        script("updater", R"SH(
[ -t 0 ] && [ -t 1 ] || exit 90
printf '\033[31mParola:\033[0m '
IFS= read -r password
[ "$password" = 'test-private-password' ] || exit 91
printf '\nDevam [y/N]: '
IFS= read -r answer
[ "$answer" = y ] || exit 92
printf '\nPaket kontrolü tamamlandı.\n'
)SH");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QVERIFY(installer.busy());
        installer.start();
        scanner.scan();
        QCOMPARE(scanned.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Parola:"), 5000);
        installer.sendInput("test-private-password");
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Devam [y/N]:"), 5000);
        installer.sendInput("y");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QCOMPARE(installer.status(), "finished");
        QVERIFY(installer.output().contains("Paket kontrolü tamamlandı."));
        QVERIFY(!installer.output().contains("test-private-password"));
        QVERIFY(!installer.output().contains(QChar(0x1b)));
        QVERIFY(scanner.hasUpdates()); // A zero exit is not proof that all sources installed.
    }

    void singleKeyDoesNotAnswerTheNextLinePrompt()
    {
        script("updater", R"SH(
/usr/bin/stty -icanon min 1 time 0
printf 'Tek tuş: '
key=$(/usr/bin/dd bs=1 count=1 status=none)
[ "$key" = q ] || exit 94
/usr/bin/stty icanon
printf '\nSatır yanıtı: '
IFS= read -r answer
[ "$answer" = confirm ] || exit 95
)SH");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Tek tuş:"), 5000);
        installer.sendKey("q");
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Satır yanıtı:"), 5000);
        QCOMPARE(finished.count(), 0);
        installer.sendInput("confirm");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "finished");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
    }

    void cancellationAndFailureRemainVisible_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("state");
        QTest::newRow("cancelled") << 4 << QString("cancelled");
        QTest::newRow("official-failure") << 5 << QString("error");
        QTest::newRow("auth-failure") << 2 << QString("error");
        QTest::newRow("reboot-failure") << 6 << QString("error");
        QTest::newRow("interrupted") << 130 << QString("error");
    }

    void cancellationAndFailureRemainVisible()
    {
        QFETCH(int, code);
        QFETCH(QString, state);
        script("updater", "exit " + QByteArray::number(code) + "\n");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QCOMPARE(installer.status(), state);
        QVERIFY(!installer.busy());
    }

    void launchFailureDoesNotLockScanning()
    {
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        installer.m_sessionTool = "/missing/program";
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QCOMPARE(installer.status(), "error");
        QVERIFY(!installer.busy());
    }

    void qmlRequiresConfirmationAndRunsLiveOfficialHelperAndRecoveryRedirect()
    {
        script("pkexec", "exec \"$@\"\n");
        script("mock-update-helper", R"SH(
printf '[ZETHROPOL-UPDATE] STAGE=preflight STATUS=ok\n'
printf '[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=starting\n'
printf '[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=101\n'
printf '[ZETHROPOL-UPDATE] STAGE=pacman-upgrade STATUS=starting\n'
printf ':: Synchronizing package databases...\n'
printf ':: Starting full system upgrade...\n'
printf ':: Processing package changes...\n'
printf '[ZETHROPOL-UPDATE] STAGE=pacman-upgrade STATUS=ok\n'
printf '[ZETHROPOL-UPDATE] STAGE=post-snapshot STATUS=starting PRE_SNAPSHOT_ID=101\n'
printf '[ZETHROPOL-UPDATE] STAGE=post-snapshot STATUS=ok SNAPSHOT_ID=102 PRE_SNAPSHOT_ID=101\n'
printf '[ZETHROPOL-UPDATE] STAGE=limine-sync STATUS=starting\n'
printf '[ZETHROPOL-UPDATE] STAGE=limine-sync STATUS=ok\n'
printf '[ZETHROPOL-UPDATE] STAGE=complete STATUS=ok PRE_SNAPSHOT_ID=101 POST_SNAPSHOT_ID=102\n'
exit 0
)SH");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        QVERIFY(installer.liveExecutionEnabled());
        installer.m_updateHelperPath = directory.filePath("mock-update-helper");
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.pacmanDbChecked = true;
            ev.pacmanDbOk = true;
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = true;
            ev.recoverySnapshotVerified = true;
            ev.recoveryRecommendedLevel = QStringLiteral("none");
            ev.recoveryAssessmentStatus = QStringLiteral("healthy");
            return ev;
        };

        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(UPDATE_REPORT_QML)));
        std::unique_ptr<QObject> root(component.createWithInitialProperties({
            {"scanner", QVariant::fromValue<QObject *>(&scanner)},
            {"installer", QVariant::fromValue<QObject *>(&installer)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        item->setSize(QSizeF(1000, 760));
        window.resize(1000, 760);
        window.show();

        auto *start = root->findChild<QObject *>("startUpdateInstallation");
        auto *confirmation = root->findChild<QObject *>("updateInstallationConfirmation");
        auto *cancel = root->findChild<QObject *>("cancelUpdateInstallation");
        auto *confirm = root->findChild<QObject *>("confirmUpdateInstallation");
        auto *progressPanel = root->findChild<QObject *>("liveUpdateProgressPanel");
        auto *preBadge = root->findChild<QObject *>("preSnapshotBadge");
        auto *postBadge = root->findChild<QObject *>("postSnapshotBadge");
        auto *output = root->findChild<QObject *>("updateInstallationOutput");
        auto *openRecoveryBtn = root->findChild<QObject *>("openRecoveryButton");
        QVERIFY(start && confirmation && cancel && confirm && progressPanel && preBadge && postBadge && output && openRecoveryBtn);

        QCOMPARE(installer.status(), "idle");
        QVERIFY(!confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(installer.status(), "idle");
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_VERIFY(!confirmation->property("visible").toBool());
        QCOMPARE(installer.status(), "idle");

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);

        QCOMPARE(installer.status(), "finished");
        QCOMPARE(installer.progressStage(), QStringLiteral("done"));
        QCOMPARE(installer.progressPercent(), 100);
        QCOMPARE(installer.preSnapshotId(), 101u);
        QCOMPARE(installer.postSnapshotId(), 102u);
        QCOMPARE(installer.healthStatus(), QStringLiteral("healthy"));
        QVERIFY(output->property("text").toString().contains(":: Processing package changes..."));
        QVERIFY(preBadge->property("text").toString().contains("#101"));
        QVERIFY(postBadge->property("text").toString().contains("#102"));
        QVERIFY(installer.recoveryGuidance().contains("#101"));
        QVERIFY(installer.recoveryGuidance().contains("#102"));

        QSignalSpy recoveryRequested(root.get(), SIGNAL(openRecoveryRequested()));
        QVERIFY(QMetaObject::invokeMethod(openRecoveryBtn, "clicked"));
        QCOMPARE(recoveryRequested.count(), 1);

        // Also verify interactive input bar works during interactive TTY sessions (e.g. start())
        script("updater", "printf 'Yanıt bekleniyor: '\nIFS= read -r answer\n[ \"$answer\" = continue ] || exit 93\n");
        installer.m_updater = directory.filePath("updater");
        installer.m_recoveryGateEnabled = false;
        auto *input = root->findChild<QObject *>("updateInstallationInput");
        auto *send = root->findChild<QObject *>("sendUpdateInstallationInput");
        QVERIFY(input && send);
        installer.start();
        QVERIFY(installer.busy());
        QVERIFY(installer.interactiveSession());
        QTRY_VERIFY_WITH_TIMEOUT(output->property("text").toString().contains("Yanıt bekleniyor:"), 5000);
        QCOMPARE(input->property("echoMode").toInt(), 2); // Password input.
        input->setProperty("text", "continue");
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        QVERIFY(input->property("text").toString().isEmpty());
        input->setProperty("text", "unsubmitted-private-input");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 3, 5000);
        QVERIFY(!installer.output().contains("continue"));
        QVERIFY(!installer.output().contains("unsubmitted-private-input"));
        QVERIFY(input->property("text").toString().isEmpty());
        QCOMPARE(installer.status(), "finished");
    }

    void progressStageAndRebootMessageAreReported()
    {
        script("updater", R"SH(
printf '==> Paketler Güncelleniyor...\n'
/bin/sleep 0.2
printf '==> Yeniden başlatma gerekiyor:\nŞimdi yeniden başlatmak ister misiniz? [e/H] '
IFS= read -r ans
printf 'Yeniden başlatma gerçekleştirilmedi.\n'
)SH");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_VERIFY_WITH_TIMEOUT(installer.message().contains("yeniden başlatma", Qt::CaseInsensitive), 5000);
        installer.sendInput("h");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "finished");
        QVERIFY(installer.message().contains("yeniden başlatmanız gerekir"));
    }

    void successfulUpdateReportsCleanCompletion()
    {
        script("updater", "printf '==> Güncelleme uygulandı\\n'\nexit 0\n");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "finished");
        QVERIFY(installer.message().contains("başarıyla"));
    }

    void liveExecutionDefaultsEnabledAndExplicitDisableBlocksUpdater()
    {
        const QString marker = directory.filePath("updater-ran");
        script("updater", QString("touch '%1'\nexit 0\n").arg(marker).toUtf8());

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller installer(&scanner);
        installer.m_updater = directory.filePath("updater");
        QVERIFY(installer.liveExecutionEnabled());
        installer.m_liveExecutionEnabled = false;

        QString recordedOutcome;
        installer.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 15};
        };
        installer.m_recoveryComplete = [&](const QString &, const QString &outcome) {
            recordedOutcome = outcome;
        };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(installer.status(), "blocked");
        QCOMPARE(installer.m_verifiedSnapshotId, 15u);
        QCOMPARE(recordedOutcome, "aborted");
        QVERIFY(!QFile::exists(marker));
    }

    void preUpdateGateFailureBlocksUpdaterAndClassifiesCriticalAndIncompleteScans()
    {
        const QString marker = directory.filePath("updater-ran-fail");
        script("updater", QString("touch '%1'\nexit 0\n").arg(marker).toUtf8());
        script("checkupdates", "printf 'linux-cachyos 7.3.4 -> 7.4.0\\n'\n");

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        QVERIFY(UpdateInstaller::isCriticalBootUpdate(&scanner));

        // Also incomplete/error scan must default to critical=true
        script("checkupdates", "printf 'mesa 1 -> 2\\n'\n");
        script("paru", "echo 'AUR error' >&2\nexit 2\n");
        UpdateScanner incompleteScanner;
        QSignalSpy incompleteScanned(&incompleteScanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(incompleteScanned.count(), 1, 5000);
        QVERIFY(UpdateInstaller::isCriticalBootUpdate(&incompleteScanner));

        UpdateInstaller installer(&scanner);
        installer.m_updater = directory.filePath("updater");
        bool sawCriticalFlag = false;
        installer.m_recoveryGate = [&](const QString &, bool criticalBoot) {
            sawCriticalFlag = criticalBoot;
            return PreUpdateGateResult{false, QStringLiteral("blocked"),
                                       QStringLiteral("Critical boot update blocked: /boot unreadable."), 0};
        };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QCOMPARE(finished.count(), 1);
        QVERIFY(sawCriticalFlag);
        QCOMPARE(installer.status(), "error");
        QVERIFY(installer.message().contains("/boot"));
        QVERIFY(!QFile::exists(marker));
    }

    void failedIsolatedUpdatePreservesSnapshotAndReportsFailedOutcome()
    {
        script("updater", "exit 5\n");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller installer(&scanner);
        installer.m_updater = directory.filePath("updater");
        installer.m_liveExecutionEnabled = true;

        QString completedOutcome;
        installer.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 21};
        };
        installer.m_recoveryComplete = [&](const QString &, const QString &outcome) {
            completedOutcome = outcome;
        };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "error");
        QCOMPARE(installer.m_verifiedSnapshotId, 21u);
        QCOMPARE(completedOutcome, "failed");
        QVERIFY(installer.recoveryGuidance().contains("#21"));
        QVERIFY(installer.recoveryGuidance().contains("/home"));
        QVERIFY(installer.recoveryGuidance().contains("Otomatik rollback"));
    }

    void dbusUnavailablePreGateFailsClosedAndNeverRunsUpdater()
    {
        const QString marker = directory.filePath("updater-ran-dbus-down");
        script("updater", QString("touch '%1'\nexit 0\n").arg(marker).toUtf8());

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller installer(&scanner);
        installer.m_updater = directory.filePath("updater");
        QVERIFY(installer.liveExecutionEnabled());
        installer.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{
                false, QStringLiteral("not-available"),
                QStringLiteral("Recovery servisi erişilemez; güncelleme engellendi."), 0};
        };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(installer.status(), "error");
        QCOMPARE(installer.m_verifiedSnapshotId, 0u);
        QVERIFY(!QFile::exists(marker));
    }

    void cancelledAndUnknownIsolatedUpdatePreserveSnapshotAndGuidance()
    {
        // 1. Cancelled (exit 4)
        script("updater", "exit 4\n");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller cancelledInstaller(&scanner);
        cancelledInstaller.m_updater = directory.filePath("updater");
        cancelledInstaller.m_liveExecutionEnabled = true;

        QString cancelledOutcome;
        cancelledInstaller.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 33};
        };
        cancelledInstaller.m_recoveryComplete = [&](const QString &, const QString &outcome) {
            cancelledOutcome = outcome;
        };

        QSignalSpy cancelledFinished(&cancelledInstaller, &UpdateInstaller::sessionFinished);
        cancelledInstaller.start();
        QTRY_COMPARE_WITH_TIMEOUT(cancelledFinished.count(), 1, 5000);
        QCOMPARE(cancelledInstaller.status(), "cancelled");
        QCOMPARE(cancelledInstaller.m_verifiedSnapshotId, 33u);
        QCOMPARE(cancelledOutcome, "failed");
        QVERIFY(cancelledInstaller.recoveryGuidance().contains("#33"));
        QVERIFY(cancelledInstaller.recoveryGuidance().contains("Otomatik rollback"));
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);

        // 2. Concurrent start() while scanner is busy is ignored
        script("checkupdates", "sleep 1\nprintf 'pkg 1 -> 2\\n'\n");
        scanner.scan();
        UpdateInstaller busyCheckInstaller(&scanner);
        busyCheckInstaller.m_updater = directory.filePath("updater");
        QVERIFY(scanner.busy());
        busyCheckInstaller.start();
        QCOMPARE(busyCheckInstaller.status(), "idle");
    }

    void officialRepoScanIntegrityGatesInstallAndUi()
    {
        const QString marker = directory.filePath("updater-ran-bad-official");
        script("updater", QString("touch '%1'\nexit 0\n").arg(marker).toUtf8());

        // 1. Official repo error + AUR/Flatpak updates present -> must block
        script("checkupdates", "printf 'sync db failed\\n' >&2\nexit 1\n");
        script("paru", "printf 'aurpkg 1 -> 2\\n'\n");
        script("flatpak", "printf 'org.app.Test\\tTest\\t1.2\\n'\n");

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        QVERIFY(scanner.hasUpdates());

        UpdateInstaller installer(&scanner);
        installer.m_updater = directory.filePath("updater");
        installer.m_liveExecutionEnabled = true;
        QVERIFY(!installer.officialRepoReady());

        bool gateCalled = false;
        installer.m_recoveryGate = [&](const QString &, bool) {
            gateCalled = true;
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 41};
        };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QCOMPARE(finished.count(), 1);
        QCOMPARE(installer.status(), "error");
        QVERIFY(installer.message().contains("Resmî depo"));
        QVERIFY(!gateCalled);
        QVERIFY(!QFile::exists(marker));

        // Verify QML button is disabled and notice is visible when official repo is not ready
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(UPDATE_REPORT_QML));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"scanner", QVariant::fromValue(&scanner)}, {"installer", QVariant::fromValue(&installer)}}));
        QVERIFY(root);
        auto *startButton = root->findChild<QObject *>("startUpdateInstallation");
        auto *noticeLabel = root->findChild<QObject *>("officialRepoGateNotice");
        QVERIFY(startButton);
        QVERIFY(noticeLabel);
        QVERIFY(!startButton->property("enabled").toBool());
        QVERIFY(noticeLabel->property("visible").toBool());

        // 2. Official repo warning -> must block
        script("checkupdates", "printf 'malformed line\\n'\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QVERIFY(!installer.officialRepoReady());
        installer.start();
        QCOMPARE(finished.count(), 2);
        QCOMPARE(installer.status(), "error");
        QVERIFY(!gateCalled);
        QVERIFY(!QFile::exists(marker));

        // 3. Official repo pending (before scan finishes) -> officialRepoReady is false and updater never runs
        script("checkupdates", "exit 2\n");
        UpdateScanner pendingScanner;
        UpdateInstaller pendingInstaller(&pendingScanner);
        pendingInstaller.m_updater = directory.filePath("updater");
        pendingInstaller.m_liveExecutionEnabled = true;
        QVERIFY(!pendingInstaller.officialRepoReady());
        QVERIFY(!UpdateInstaller::isOfficialRepoScanComplete(&pendingScanner));
        pendingInstaller.start();
        QCOMPARE(pendingInstaller.status(), "idle");
        QVERIFY(!QFile::exists(marker));
    }

    void skipAutosnapIsStrippedAndExitZeroErrorsNeverReportOk()
    {
        // 1. SKIP_AUTOSNAP=1, SNAP_PAC_SKIP=1, and rogue ARCH_UPDATE_LIBDIR must be neutralized; exit 0 + verified error pattern -> error/failed
        qputenv("SKIP_AUTOSNAP", "1");
        qputenv("SNAP_PAC_SKIP", "1");
        qputenv("ARCH_UPDATE_LIBDIR", "/tmp/evil-libdir");
        const auto envGuard = qScopeGuard([] {
            qunsetenv("SKIP_AUTOSNAP");
            qunsetenv("SNAP_PAC_SKIP");
            qunsetenv("ARCH_UPDATE_LIBDIR");
        });

        script("updater", R"SH(
if [ -n "${SKIP_AUTOSNAP:-}" ]; then
    echo "LEAKED_SKIP_AUTOSNAP"
    exit 0
fi
if [ -n "${SNAP_PAC_SKIP:-}" ]; then
    echo "LEAKED_SNAP_PAC_SKIP"
    exit 0
fi
if [ "${ARCH_UPDATE_LIBDIR:-}" != "/usr/share/arch-update/lib" ]; then
    echo "LEAKED_ARCH_UPDATE_LIBDIR=${ARCH_UPDATE_LIBDIR:-}"
    exit 0
fi
printf 'Updating Packages...\n'
printf 'error: failed to commit transaction (conflicting files)\n'
printf 'An error has occurred during the update process\n'
exit 0
)SH");

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller installer(&scanner);
        QVERIFY(installer.liveExecutionEnabled());
        installer.m_updater = directory.filePath("updater");
        installer.m_liveExecutionEnabled = true;
        QString outcome;
        installer.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 51};
        };
        installer.m_recoveryComplete = [&](const QString &, const QString &o) { outcome = o; };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(!installer.output().contains("LEAKED_SKIP_AUTOSNAP"));
        QVERIFY(!installer.output().contains("LEAKED_SNAP_PAC_SKIP"));
        QVERIFY(!installer.output().contains("LEAKED_ARCH_UPDATE_LIBDIR"));
        QCOMPARE(installer.status(), "error");
        QCOMPARE(outcome, "failed");

        // 2. Normal exit 0 with benign words containing "error" -> finished/ok (no false positive)
        script("updater", R"SH(
printf 'Upgrading liberror 1.0 -> 1.1\n'
printf 'The update has been applied\n'
exit 0
)SH");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QString okOutcome;
        installer.m_recoveryComplete = [&](const QString &, const QString &o) { okOutcome = o; };
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(installer.status(), "finished");
        QCOMPARE(okOutcome, "ok");

        // 3. Crash / non-normal exit of session tool -> unknown (never ok)
        script("crash-session", "kill -9 $$\n");
        installer.m_sessionTool = directory.filePath("crash-session");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 3, 5000);
        QString unknownOutcome;
        installer.m_recoveryComplete = [&](const QString &, const QString &o) { unknownOutcome = o; };
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QCOMPARE(installer.status(), "unknown");
        QCOMPARE(unknownOutcome, "unknown");
    }

    void aurAndFlatpakErrorsAndIncompleteScansAreClassifiedSafely()
    {
        // 1. Official repo succeeds, AUR build error with exit 0 -> error, Recovery failed
        script("checkupdates", "printf 'corepkg 1.0 -> 1.1\\n'\n");
        script("paru", "printf 'aurpkg 1.0 -> 2.0\\n'\n");
        script("flatpak", "exit 0\n");
        script("updater", R"SH(
printf 'Updating Packages...\n'
printf 'Updating AUR Packages...\n'
printf '==> ERROR: A failure occurred in build().\n'
exit 0
)SH");

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller installer(&scanner);
        QVERIFY(installer.liveExecutionEnabled());
        installer.m_updater = directory.filePath("updater");
        installer.m_liveExecutionEnabled = true;

        QString aurOutcome;
        installer.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 61};
        };
        installer.m_recoveryComplete = [&](const QString &, const QString &o) { aurOutcome = o; };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "error");
        QCOMPARE(aurOutcome, "failed");

        // 2. Official repo succeeds, Flatpak update error with exit 0 -> error, Recovery failed
        script("updater", R"SH(
printf 'Updating Packages...\n'
printf 'Updating Flatpak Packages...\n'
printf 'error: Failed to update org.kde.Platform: remote failure\n'
exit 0
)SH");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QString flatpakOutcome;
        installer.m_recoveryComplete = [&](const QString &, const QString &o) { flatpakOutcome = o; };
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(installer.status(), "error");
        QCOMPARE(flatpakOutcome, "failed");

        // 3. Clean exit 0 with benign "error" in package/app name -> finished/ok (no false positive)
        script("updater", R"SH(
printf 'Updating Packages...\n'
printf 'Upgrading liberror-tools 1.0 -> 1.1\n'
printf 'Updating org.example.ErrorViewer\n'
printf 'The update has been applied\n'
exit 0
)SH");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 3, 5000);
        QString cleanOutcome;
        installer.m_recoveryComplete = [&](const QString &, const QString &o) { cleanOutcome = o; };
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QCOMPARE(installer.status(), "finished");
        QCOMPARE(cleanOutcome, "ok");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 4, 5000);

        // 4. Official repo complete, AUR/Flatpak scan error or warning -> conservative critical classification preserved
        script("paru", "printf 'aur unreachable\\n' >&2\nexit 1\n");
        script("flatpak", "printf 'malformed-line\\n'\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 5, 5000);
        QVERIFY(installer.officialRepoReady());
        QVERIFY(UpdateInstaller::isCriticalBootUpdate(&scanner));
    }

    void postUpdateHealthCleanupAndRebootSafetyBoundaries()
    {
        script("checkupdates", "printf 'corepkg 1.0 -> 1.1\\n'\n");
        script("paru", "exit 0\n");
        script("flatpak", "exit 0\n");
        script("updater", "printf 'The update has been applied\\n'\nexit 0\n");

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);

        UpdateInstaller installer(&scanner);
        QVERIFY(installer.liveExecutionEnabled());
        installer.m_updater = directory.filePath("updater");
        installer.m_liveExecutionEnabled = true;
        installer.m_recoveryGate = [](const QString &, bool) {
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 71};
        };

        // 1. exit 0 + empty systemctl alone without boot/Recovery evidence -> unknown (never healthy)
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = false;
            ev.recoveryAssessmentStatus = QStringLiteral("unknown");
            return ev;
        };
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "finished");
        QCOMPARE(installer.healthStatus(), "unknown");
        QCOMPARE(installer.cleanupStatus(), "unknown");
        QVERIFY(installer.cleanupSummary().contains("doğrulanmamıştır"));
        QVERIFY(!installer.rebootRequired());
        QVERIFY(installer.rebootSafetyNotice().contains("güvenli yeniden başlatma garantisi verilemez"));

        // Verify QML shows separate session, health, cleanup, and reboot fields
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(UPDATE_REPORT_QML));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{"scanner", QVariant::fromValue(&scanner)}, {"installer", QVariant::fromValue(&installer)}}));
        QVERIFY(root);
        auto *panel = root->findChild<QObject *>("postUpdateAssessmentPanel");
        auto *healthLbl = root->findChild<QObject *>("postUpdateHealthSummary");
        auto *cleanupLbl = root->findChild<QObject *>("postUpdateCleanupSummary");
        auto *rebootLbl = root->findChild<QObject *>("postUpdateRebootNotice");
        QVERIFY(panel && healthLbl && cleanupLbl && rebootLbl);
        QVERIFY(panel->property("visible").toBool());
        QVERIFY(healthLbl->property("text").toString().contains("Doğrulanamadı"));
        QVERIFY(cleanupLbl->property("text").toString().contains("unknown"));
        QVERIFY(rebootLbl->property("text").toString().contains("garantisi verilemez"));

        // 2. Failed systemd units -> degraded
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 2;
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = true;
            ev.recoveryAssessmentStatus = QStringLiteral("healthy");
            return ev;
        };
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(installer.healthStatus(), "degraded");
        QVERIFY(installer.healthSummary().contains("2 başarısız systemd"));

        // 3. systemctl unreachable -> unknown
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = false;
            ev.failedSystemdUnits = -1;
            ev.systemdDetail = QStringLiteral("systemctl erişilemez");
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = true;
            ev.recoveryAssessmentStatus = QStringLiteral("healthy");
            return ev;
        };
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 3, 5000);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QCOMPARE(installer.healthStatus(), "unknown");
        QVERIFY(installer.healthSummary().contains("systemctl erişilemez"));

        // 4. Active pacman lock or Recovery blocked -> blocked
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.pacmanLockPresent = true;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.recoveryAssessmentStatus = QStringLiteral("blocked-lock");
            return ev;
        };
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 4, 5000);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 5000);
        QCOMPARE(installer.healthStatus(), "blocked");

        // 5. Kernel/Limine verification failed -> never healthy (degraded)
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = false;
            ev.bootVerified = false;
            ev.bootDegraded = true;
            ev.bootDetail = QStringLiteral("Limine doğrulaması başarısız");
            ev.recoveryAssessmentStatus = QStringLiteral("external-recovery-required");
            return ev;
        };
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 5, 5000);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, 5000);
        QCOMPARE(installer.healthStatus(), "degraded");
        QVERIFY(installer.rebootRequired());
        QVERIFY(installer.rebootSafetyNotice().contains("garantisi verilemez"));

        // 6. Failed update -> no cleanup (blocked)
        script("updater", "exit 5\n");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 6, 5000);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, 5000);
        QCOMPARE(installer.status(), "error");
        QCOMPARE(installer.cleanupStatus(), "blocked");
        QVERIFY(installer.cleanupSummary().contains("otomatik temizlik yapılmadı"));

        // 7. Full verified healthy + reboot required -> healthy and reboot recommended without auto-reboot
        script("updater", "printf 'Reboot required to finalize kernel update\\n'\nexit 0\n");
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = true;
            ev.recoveryAssessmentStatus = QStringLiteral("healthy");
            return ev;
        };
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 7, 5000);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 7, 5000);
        QCOMPARE(installer.healthStatus(), "healthy");
        QVERIFY(installer.rebootRequired());
        QCOMPARE(installer.rebootStatus(), "recommended");
        QVERIFY(installer.rebootSafetyNotice().contains("açık kullanıcı onayıyla"));
    }

    void scopedSeparationDbLockAndRealHealthEvidence()
    {
        // 1. db.lck preflight blocks before Recovery gate is even called
        script("checkupdates", "printf 'amd-ucode 1.0 -> 1.1\\n'\n");
        script("paru", "printf 'aurpkg 1.0 -> 2.0\\n'\n");
        script("flatpak", "printf 'org.app.Demo\\tDemo\\t1.0\\n'\n");

        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        QVERIFY(UpdateInstaller::isCriticalBootUpdate(&scanner));

        const QString fakeLock = directory.filePath("fake-db.lck");
        {
            QFile lockFile(fakeLock);
            QVERIFY(lockFile.open(QIODevice::WriteOnly));
            lockFile.write("");
        }

        UpdateInstaller installer(&scanner);
        installer.m_pacmanLockPath = fakeLock;
        installer.m_liveExecutionEnabled = true;
        bool gateCalled = false;
        installer.m_recoveryGate = [&](const QString &, bool) {
            gateCalled = true;
            return PreUpdateGateResult{true, QStringLiteral("ok"), QStringLiteral("Verified"), 81};
        };

        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.startScoped(QStringLiteral("official"));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(installer.status(), "blocked");
        QVERIFY(!gateCalled);
        QFile::remove(fakeLock);

        // 2. AUR scope is blocked while official updates are pending (no partial upgrade)
        installer.startScoped(QStringLiteral("aur"));
        QCOMPARE(finished.count(), 2);
        QCOMPARE(installer.status(), "error");
        QVERIFY(installer.message().contains("resmî depo", Qt::CaseInsensitive));
        QVERIFY(!gateCalled);

        // 3. Flatpak scope runs unprivileged without invoking root Recovery pre-gate
        const QString flatpakLog = directory.filePath("flatpak-args.log");
        script("flatpak", QString("if [ \"$1\" = \"update\" ]; then printf '%s\\n' \"$*\" > '%1'; printf 'Güncelleme uygulandı\\n'; exit 0; fi\nprintf 'org.app.Demo\\tDemo\\t1.0\\n'\n").arg(flatpakLog).toUtf8());
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.pacmanDbChecked = true;
            ev.pacmanDbOk = true;
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = true;
            ev.recoverySnapshotVerified = true;
            ev.recoveryRecommendedLevel = QStringLiteral("none");
            ev.recoveryAssessmentStatus = QStringLiteral("unknown");
            return ev;
        };
        installer.startScoped(QStringLiteral("flatpak"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QVERIFY(!gateCalled);
        QCOMPARE(installer.status(), "finished");
        QCOMPARE(installer.healthStatus(), "healthy");
        QFile fpLog(flatpakLog);
        QVERIFY(fpLog.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(fpLog.readAll()).trimmed(), QStringLiteral("update"));

        // 4. Official scope invokes pkexec <update-helper> apply-official (single Polkit prompt, no --nodeps/--overwrite)
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        const QString pkexecLog = directory.filePath("pkexec-args.log");
        script("mock-update-helper", R"SH(
printf '[ZETHROPOL-UPDATE] STAGE=preflight STATUS=ok\n'
printf '[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=81\n'
printf '[ZETHROPOL-UPDATE] STAGE=pacman-upgrade STATUS=starting\n'
printf '[ZETHROPOL-UPDATE] STAGE=pacman-upgrade STATUS=ok\n'
printf '[ZETHROPOL-UPDATE] STAGE=post-snapshot STATUS=ok SNAPSHOT_ID=82 PRE_SNAPSHOT_ID=81\n'
printf '[ZETHROPOL-UPDATE] STAGE=limine-sync STATUS=ok\n'
printf '[ZETHROPOL-UPDATE] STAGE=complete STATUS=ok PRE_SNAPSHOT_ID=81 POST_SNAPSHOT_ID=82\n'
exit 0
)SH");
        script("pkexec", QString("printf '%s\\n' \"$*\" > '%1'\nexec \"$@\"\n").arg(pkexecLog).toUtf8());
        installer.m_updateHelperPath = directory.filePath("mock-update-helper");
        installer.startScoped(QStringLiteral("official"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 5000);
        QVERIFY(!gateCalled); // Helper itself owns the fail-closed pre-snapshot under single Polkit auth
        QCOMPARE(installer.status(), "finished");
        QCOMPARE(installer.preSnapshotId(), 81u);
        QCOMPARE(installer.postSnapshotId(), 82u);
        QCOMPARE(installer.healthStatus(), "healthy");
        QFile pkLog(pkexecLog);
        QVERIFY(pkLog.open(QIODevice::ReadOnly));
        const QString pkCmd = QString::fromUtf8(pkLog.readAll()).trimmed();
        QVERIFY(pkCmd.contains("mock-update-helper"));
        QVERIFY(pkCmd.contains("apply-official"));
        QVERIFY(!pkCmd.contains("--nodeps"));
        QVERIFY(!pkCmd.contains("--overwrite"));

        // 5. pacman -Dk DB integrity failure degrades health status honestly
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 3, 5000);
        installer.m_healthProbe = [](const QString &, quint32, const QString &) {
            PostUpdateHealthEvidence ev;
            ev.systemdChecked = true;
            ev.failedSystemdUnits = 0;
            ev.pacmanDbChecked = true;
            ev.pacmanDbOk = false;
            ev.pacmanDbDetail = QStringLiteral("missing dependency detected");
            ev.activeKernelCheckDone = true;
            ev.activeKernelImagePresent = true;
            ev.bootVerified = true;
            ev.recoverySnapshotVerified = true;
            ev.recoveryRecommendedLevel = QStringLiteral("none");
            ev.recoveryAssessmentStatus = QStringLiteral("unknown");
            return ev;
        };
        installer.startScoped(QStringLiteral("official"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, 5000);
        QCOMPARE(installer.healthStatus(), "degraded");
        QVERIFY(installer.healthSummary().contains("missing dependency"));
        QVERIFY(installer.recoveryGuidance().contains("#81"));

        // 6. Helper returning exit 17 (db.lck) defers safely with pacmanLockBlocked=true; exit 5 preserves pre-snapshot
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 4, 5000);
        script("mock-update-helper", "printf '[ZETHROPOL-UPDATE] STAGE=blocked REASON=pacman-lock\\n'\nexit 17\n");
        installer.startScoped(QStringLiteral("official"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, 5000);
        QCOMPARE(installer.status(), "blocked");
        QVERIFY(installer.pacmanLockBlocked());

        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 5, 5000);
        script("mock-update-helper", R"SH(
printf '[ZETHROPOL-UPDATE] STAGE=pre-snapshot STATUS=ok SNAPSHOT_ID=205\n'
printf '[ZETHROPOL-UPDATE] STAGE=pacman-upgrade STATUS=failed EXIT_CODE=1\n'
exit 5
)SH");
        installer.startScoped(QStringLiteral("official"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 7, 5000);
        QCOMPARE(installer.status(), "error");
        QCOMPARE(installer.preSnapshotId(), 205u);
        QVERIFY(installer.recoveryGuidance().contains("#205"));
    }
};

QTEST_MAIN(UpdateInstallerTest)
#include "UpdateInstallerTest.moc"
