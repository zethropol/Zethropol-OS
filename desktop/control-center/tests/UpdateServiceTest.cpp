#include "UpdateService.h"

#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>

class UpdateServiceTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_busGuard;

private slots:
    void aurReceiptConsentIsSeparateBoundAndSingleUse() {
        UpdateService service(nullptr,false);
        service.m_aurBrokerReview={{"state","ready"},{"installation","ready"},{"system_acceptance","complete"}};
        service.m_aurUpdates={QVariantMap{{"name","demo"},{"newVersion","2-1"},{"selected",true},{"source","aur"}}};
        service.m_aurBundlePending=true;service.m_previewing=true;service.m_aurRequestSelection=service.selectionKey();
        const QString id="11111111-1111-4111-8111-111111111111",hash(64,'a');
        service.m_updateClient.aurReceiptPreviewFinished(true,id,{QVariantMap{{"package_name","demo"},{"new_version","2-1"}}},hash,{});
        QCOMPARE(service.m_aurReceiptHash,hash);QVERIFY(!service.installing());QVERIFY(!service.previewing());
        QSignalSpy consent(&service,&UpdateService::aurInstallConfirmationRequested);
        service.requestAurInstallConfirmation();QCOMPARE(consent.count(),1);QCOMPARE(consent[0][0].toString(),hash);
        service.confirmAurInstall(hash,false);QVERIFY(!service.installing());
        service.requestAurInstallConfirmation();QCOMPARE(consent.count(),2);
        service.setPackageSelected("aur","demo",false);
        QSignalSpy errors(&service,&UpdateService::operationError);
        service.confirmAurInstall(hash,true);QCOMPARE(errors.count(),1);QVERIFY(!service.installing());
        QVERIFY(service.m_aurReceiptHash.isEmpty());
    }
    void aurReadinessCannotBeReplacedByReceiptOrStaleConsent() {
        UpdateService service(nullptr,false);
        service.m_aurReceiptHash=QString(64,'a');
        service.m_aurReceiptSelection=service.selectionKey();
        service.m_aurReceiptClock.start();
        QSignalSpy consent(&service,&UpdateService::aurInstallConfirmationRequested);
        QSignalSpy errors(&service,&UpdateService::operationError);
        for(const auto &report:QList<QVariantMap>{ {},
            {{"state","blocked"},{"installation","closed"},{"system_acceptance","not-complete"}},
            {{"state","ready"},{"installation","ready"}},
            {{"state","ready"},{"installation","closed"},{"system_acceptance","complete"}} }) {
            service.m_aurBrokerReview=report;
            QVERIFY(!service.aurInstallReady());
            service.requestAurInstallConfirmation();
            QCOMPARE(consent.count(),0);
            service.m_aurConsentPending=true;
            service.confirmAurInstall(service.m_aurReceiptHash,true);
            QVERIFY(!service.installing());
            QVERIFY(!service.m_aurConsentPending);
        }
        QCOMPARE(errors.count(),8);
        service.m_aurBrokerReview={{"state","ready"},{"installation","ready"},{"system_acceptance","complete"}};
        service.requestAurInstallConfirmation();QCOMPARE(consent.count(),1);
        service.finishAurBrokerReview(false,service.m_aurBrokerReview);
        service.confirmAurInstall(service.m_aurReceiptHash,true);
        QVERIFY(!service.installing());QVERIFY(!service.aurInstallReady());
    }
    void aurReceiptRejectsSelectionDriftDuringHandoff() {
        UpdateService service(nullptr,false);
        service.m_aurUpdates={QVariantMap{{"name","demo"},{"newVersion","3-1"},{"selected",true},{"source","aur"}}};
        service.m_aurBundlePending=true;service.m_previewing=true;service.m_aurRequestSelection="old-selection";
        service.m_updateClient.aurReceiptPreviewFinished(true,"11111111-1111-4111-8111-111111111111",
            {QVariantMap{{"package_name","demo"},{"new_version","3-1"}}},QString(64,'a'),{});
        QVERIFY(service.m_aurReceiptHash.isEmpty());QVERIFY(!service.previewing());
        QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("blocked"));
    }
    void aurReservationFailureUnlocksUiWithoutChangingOfficialResult() {
        UpdateService service(nullptr,false);service.m_aurReservationPending=true;service.m_previewing=true;
        service.setSourceState("official","completed","retained");
        service.m_updateClient.aurRequestFailed({},"Missing approved review",false);
        QVERIFY(!service.previewing());QVERIFY(!service.m_aurReservationPending);
        QCOMPARE(service.sourceResults().value("official").toMap().value("state").toString(),QString("completed"));
    }
    void aurPartialCommitAndTransportUncertaintyBlockDuplicateInstall() {
        UpdateService service(nullptr,false);service.m_aurOperationId="11111111-1111-4111-8111-111111111111";
        service.m_aurOwnsInstallFlag=true;service.m_installing=true;
        service.m_updateClient.aurRequestFailed(service.m_aurOperationId,"No reply",true);
        QVERIFY(service.installing());
        service.finishAurOperation(service.m_aurOperationId,{{"status","failed"},{"package_commit","failed-may-be-partial"}});
        QVERIFY(service.installing());
        QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("unknown"));
        service.finishAurOperation(service.m_aurOperationId,{{"status","failed"},{"package_commit","not-started"}});
        QVERIFY(!service.installing());
    }
    void aurLocalBuildCannotCancelAnOlderBrokerOperation() {
        UpdateService service(nullptr,false);service.m_aurOperationId="11111111-1111-4111-8111-111111111111";
        service.m_installing=true;service.m_batchStatus="aur-build";
        QSignalSpy consent(&service,&UpdateService::aurCancellationConfirmationRequested);
        QSignalSpy errors(&service,&UpdateService::operationError);
        service.requestAurCancellation();QCOMPARE(consent.count(),0);QCOMPARE(errors.count(),1);
        QVERIFY(!service.m_aurCancelPending);
    }
    void aurCancellationConsentRejectsReplacedOperation() {
        UpdateService service(nullptr,false);service.m_aurOperationId="11111111-1111-4111-8111-111111111111";
        QSignalSpy consent(&service,&UpdateService::aurCancellationConfirmationRequested);
        service.requestAurCancellation();QCOMPARE(consent.count(),1);
        service.m_aurOperationId="22222222-2222-4222-8222-222222222222";
        service.confirmAurCancellation(consent[0][0].toString(),true);QVERIFY(!service.m_aurCancelPending);
    }
    void localAurProgressIsSeparateAndStaleCancellationCannotReachBroker() {
        UpdateService service(nullptr,false);service.m_batchStatus="aur-build";service.m_installing=true;
        service.m_aurOperationId="11111111-1111-4111-8111-111111111111";
        service.m_sourceRunner.localBuildState({{"execution","local"},{"stage","starting"}});
        service.m_sourceRunner.localBuildState({{"execution","local"},{"operation","journal"},{"stage","building"}});
        QCOMPARE(service.m_localAurExecution,QString("local"));QVERIFY(service.aurStatus().contains("building"));
        QSignalSpy consent(&service,&UpdateService::localAurCancellationConfirmationRequested);
        service.requestLocalAurCancellation();QCOMPARE(consent.count(),1);
        service.confirmLocalAurCancellation("older-local",true);QVERIFY(!service.m_aurCancelPending);
        service.m_sourceRunner.localBuildState({{"execution","foreign"},{"stage","building"}});
        QCOMPARE(service.m_localAurExecution,QString("local"));
    }
    void localAurUncertainCleanupCannotBecomeSuccess() {
        UpdateService service(nullptr,false);service.m_batchStatus="aur-build";service.m_installing=true;
        service.finishSourceBatch({QVariantMap{{"source","aur"},{"state","unknown"},{"reason","Cleanup unverified"}}});
        QVERIFY(service.installing());QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("unknown"));
    }
    void aurBuildRequiresCurrentSelectionReview() {
        UpdateService service(nullptr,false);
        service.m_aurUpdates={QVariantMap{{"name","demo"},{"newVersion","2-1"},{"selected",true},{"source","aur"}}};
        QSignalSpy errors(&service,&UpdateService::operationError);
        service.startAurBuild();
        QCOMPARE(errors.count(),1);
        QCOMPARE(errors.last()[0].toString(),QString("Current AUR review required"));
        service.m_previewReady=true;service.m_reviewHash="review";service.m_previewClock.start();
        service.m_selectionAtReview="stale-selection";
        service.startAurBuild();
        QCOMPARE(errors.count(),2);
        QCOMPARE(errors.last()[0].toString(),QString("Current AUR review required"));
        QVERIFY(!service.installing());
    }
    void aurBuildRejectsPublicRequestBeforeExecution() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile request(directory.filePath("request.json"));
        QVERIFY(request.open(QIODevice::WriteOnly));
        request.write("{}");request.close();
        QVERIFY(request.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ReadOther));
        UpdateService service(nullptr,false);
        service.m_aurUpdates={QVariantMap{{"name","demo"},{"newVersion","2-1"},{"selected",true},{"source","aur"}}};
        service.m_previewReady=true;service.m_reviewHash="review";service.m_previewClock.start();
        service.m_selectionAtReview=service.selectionKey();
        QSignalSpy errors(&service,&UpdateService::operationError);
        service.buildAurRequest(request.fileName());
        QCOMPARE(errors.count(),1);
        QCOMPARE(errors.last()[0].toString(),QString("Manual AUR delivery disabled"));
        QVERIFY(!service.installing());
    }
    void selectionsSurviveRescanAndInvalidateConsent() {
        UpdateService service(nullptr,false);
        QVariantList rows{QVariantMap{{"name","demo"},{"selected",true},{"source","aur"}}};
        service.m_aurUpdates=rows;service.rememberSelections("aur",rows);
        QVariantList next{QVariantMap{{"name","demo"},{"newVersion","3-1"}}};
        service.restoreSelections("aur",next);QVERIFY(next[0].toMap().value("selected").toBool());
        service.m_previewReady=true;service.m_reviewHash="review";
        service.setPackageSelected("aur","demo",false);
        QVERIFY(!service.previewReady());QVERIFY(service.reviewHash().isEmpty());
    }
    void verifiedOfficialCommitThenBlockedAurPreservesCommitOutcome() {
        UpdateService service(nullptr,false);service.m_activeOperationId="fake-operation";
        service.m_batchActive=true;service.m_installing=true;
        service.m_applyPlan={QVariantMap{{"source","aur"},{"name","demo"},{"state","blocked"},{"reason","No isolated build worker"}}};
        service.m_executionEvidence={{"package_commit","committed"},{"hooks","verified"},{"boot_preparation","verified"},{"new_kernel_boot","unverified"}};
        service.finishOperationStatus("fake-operation","ok_with_warnings","Official commit completed");
        QCOMPARE(service.sourceResults().value("official").toMap().value("state").toString(),QString("completed-with-warnings"));
        QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("incomplete"));
        QVERIFY(!service.failed());QVERIFY(!service.installing());QVERIFY(service.verificationRequired());
    }
    void unverifiedOfficialCommitBlocksLaterSources() {
        UpdateService service(nullptr,false);service.m_activeOperationId="fake-operation";
        service.m_batchActive=true;service.m_installing=true;
        service.m_applyPlan={QVariantMap{{"source","aur"},{"name","demo"},{"state","blocked"}}};
        service.m_executionEvidence={{"package_commit","committed"},{"hooks","unknown"},{"boot_preparation","verified"}};
        service.finishOperationStatus("fake-operation","ok_with_warnings","Post-commit checks uncertain");
        QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("blocked"));
        QVERIFY(!service.m_batchActive);QCOMPARE(service.batchStatus(),QString("blocked"));
    }
    void officialSuccessAndAurFailureAreIndependent() {
        UpdateService service(nullptr,false);
        service.setSourceState("official","completed","Commit verified");
        service.m_batchActive=true;service.m_installing=true;
        service.finishSourceBatch({QVariantMap{{"source","aur"},{"name","demo"},{"state","failed"},{"reason","Build failed"}}});
        QCOMPARE(service.sourceResults().value("official").toMap().value("state").toString(),QString("completed"));
        QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("incomplete"));
        QCOMPARE(service.batchStatus(),QString("incomplete"));QVERIFY(!service.installing());
        QVERIFY(!service.failed()); // Official transaction result was not rewritten.
        QVERIFY(service.sourceReport().contains("Build failed"));
    }
    void blockedSelectionRequiresExplicitAcknowledgement() {
        UpdateService service(nullptr,false);
        service.m_reviewPlan={QVariantMap{{"source","aur"},{"state","blocked"},{"reason","Build worker unavailable"}}};
        service.m_reviewHash="review";service.m_previewReady=true;
        service.m_transactionPreviewReceived=true;service.m_selectionAtReview=service.selectionKey();
        QSignalSpy errors(&service,&UpdateService::operationError);service.install();
        QCOMPARE(errors.count(),1);QVERIFY(!service.installing());QVERIFY(!service.m_batchActive);
        service.confirmSkippedSources("stale",true);QVERIFY(service.m_approvedSkipHash.isEmpty());
        service.confirmSkippedSources("review",true);QCOMPARE(service.m_approvedSkipHash,QString("review"));
    }
    void aurBrokerBlockerIsPartOfPreviewAndNothingApplicableCannotRun() {
        UpdateService service(nullptr,false);service.m_officialSelected=false;
        service.m_officialPreviewDone=true;service.m_officialPreviewOk=true;service.m_sourceReviewDone=true;
        service.m_aurBrokerReviewDone=false;service.m_reviewPlan={QVariantMap{{"source","aur"},{"name","demo"},{"state","blocked"}}};
        service.m_transactionPreviewReceived=true;service.m_selectionAtReview=service.selectionKey();
        service.finishAurBrokerReview(true,{{"state","blocked"},{"reason","Approved project keyring missing"}});
        QVERIFY(service.previewReady());QVERIFY(!service.applicableSources());QVERIFY(service.blockedSources());
        QVERIFY(service.planPreview().contains("Approved project keyring missing"));
        service.confirmSkippedSources(service.reviewHash(),true);service.install();
        QVERIFY(!service.installing());QCOMPARE(service.batchStatus(),QString("idle"));
        QVERIFY(service.sourceReport().contains("No update request was sent"));
    }
    void aurKernelUpdateReportsRebootWithoutChangingOfficialConsent() {
        UpdateService service(nullptr,false);service.m_aurOperationId="11111111-1111-4111-8111-111111111111";
        service.m_verificationRequired=false;
        service.finishAurOperation(service.m_aurOperationId,{{"status","ok_with_warnings"},{"package_commit","committed"},
            {"updated_kernels","6.18.55"},{"new_kernel_boot","unverified"}});
        QVERIFY(service.aurStatus().contains("Reboot required"));
        QVERIFY(!service.m_verificationRequired);QVERIFY(!service.installing());
        service.finishAurOperation(service.m_aurOperationId,{{"status","ok"},{"package_commit","committed"},
            {"updated_kernels","6.18.55"},{"new_kernel_boot","verified"}});
        QVERIFY(!service.aurStatus().contains("Reboot required"));
    }
    void aurHistoricalUnknownDoesNotOverwriteOfficialOrFlatpakEvidence() {
        UpdateService service(nullptr,false);service.m_aurOperationId="11111111-1111-4111-8111-111111111111";
        service.m_executionEvidence={{"package_commit","committed"}};
        service.setSourceState("official","completed","official evidence");service.setSourceState("flatpak","completed","flatpak evidence");
        service.finishAurOperation(service.m_aurOperationId,{{"status","failed"},{"package_commit","unknown"},{"pre_snapshot","17"},{"message","No automatic retry"}});
        QCOMPARE(service.sourceResults().value("aur").toMap().value("state").toString(),QString("unknown"));
        QCOMPARE(service.sourceResults().value("official").toMap().value("state").toString(),QString("completed"));
        QCOMPARE(service.sourceResults().value("flatpak").toMap().value("state").toString(),QString("completed"));
        QCOMPARE(service.executionEvidence().value("package_commit").toString(),QString("committed"));
        QVERIFY(!service.installing());
    }
    void initTestCase()
    {
        QVERIFY(m_busGuard.isValid());
        // Even QDBusInterface construction must never auto-activate the real
        // broker from these unit tests.
        qputenv("DBUS_SYSTEM_BUS_ADDRESS",
                (QStringLiteral("unix:path=") + m_busGuard.filePath(QStringLiteral("missing-bus"))).toUtf8());
    }

    void removalConsentIsExplicitAndBoundToCurrentPreview()
    {
        UpdateService service(nullptr, false);
        const QVariantList effects{QVariantMap{
            {QStringLiteral("package_name"), QStringLiteral("old-demo")},
            {QStringLiteral("current_version"), QStringLiteral("1-1")},
            {QStringLiteral("reason"), QStringLiteral("replacement")},
            {QStringLiteral("replacement"), QStringLiteral("new-demo")}}};
        const QVariantList packages{QVariantMap{
            {QStringLiteral("package_name"), QStringLiteral("new-demo")},
            {QStringLiteral("current_version"), QStringLiteral("not installed (new dependency)")},
            {QStringLiteral("new_version"), QStringLiteral("2-1")},
            {QStringLiteral("repository"), QStringLiteral("core")}}};
        service.finishPlanEffects(effects, QString(64, QLatin1Char('a')));
        service.finishTransactionPreview(true, packages, {});
        QVERIFY(service.removalApprovalRequired());
        QVERIFY(service.m_approvedRemovalHash.isEmpty());
        QVERIFY(service.planPreview().contains(QStringLiteral("REMOVE: old-demo 1-1")));
        QVERIFY(service.planPreview().contains(QStringLiteral("REPLACED BY: new-demo")));
        QSignalSpy errors(&service, &UpdateService::operationError);
        service.install();
        QCOMPARE(errors.count(), 1);
        QVERIFY(!service.installing());
        service.confirmRemovals(QString(64, QLatin1Char('b')), true);
        QVERIFY(service.m_approvedRemovalHash.isEmpty());
        service.confirmRemovals(service.planHash(), true);
        QCOMPARE(service.m_approvedRemovalHash, service.planHash());
        service.confirmRemovals(service.planHash(), false);
        QVERIFY(service.m_approvedRemovalHash.isEmpty());
        service.confirmRemovals(service.planHash(), true);
        service.finishPlanEffects(effects, QString(64, QLatin1Char('b')));
        QVERIFY(service.m_approvedRemovalHash.isEmpty());
        service.install();
        QVERIFY(!service.installing());
    }

    void legacyPreviewCannotAuthorizeAHashlessUpdate()
    {
        UpdateService service(nullptr, false);
        service.m_previewReady = true;
        service.m_transactionPreviewReceived = true;
        QSignalSpy errors(&service, &UpdateService::operationError);
        service.install();
        QVERIFY(!service.installing());
        QCOMPARE(errors.count(), 1);
    }

    void completedCommitDoesNotClaimHookOrBootSuccess()
    {
        UpdateService service(nullptr, false);
        service.m_activeOperationId = QStringLiteral("fixture");
        service.m_installing = true;
        service.finishExecutionEvidence(QStringLiteral("fixture"), {
            {QStringLiteral("package_commit"), QStringLiteral("committed")},
            {QStringLiteral("hooks"), QStringLiteral("unknown")},
            {QStringLiteral("boot_preparation"), QStringLiteral("unknown")},
            {QStringLiteral("pre_snapshot"), QStringLiteral("301")},
        });
        service.finishOperationStatus(QStringLiteral("fixture"), QStringLiteral("ok_with_warnings"),
                                      QStringLiteral("Package commit completed."));
        QVERIFY(!service.installing());
        QVERIFY(!service.failed());
        QVERIFY(service.verificationRequired());
        QVERIFY(service.status().contains(QStringLiteral("verification")));
        QCOMPARE(service.executionEvidence().value(QStringLiteral("hooks")).toString(), QStringLiteral("unknown"));
        QCOMPARE(service.executionEvidence().value(QStringLiteral("pre_snapshot")).toString(), QStringLiteral("301"));
        QVERIFY(service.output().contains(QStringLiteral("does not prove")));
        QVERIFY(service.output().contains(QStringLiteral("Packages: commit completed.")));
        QVERIFY(service.output().contains(QStringLiteral("Package scripts: unverified.")));
        QVERIFY(service.output().contains(QStringLiteral("Boot preparation: unverified.")));
    }

    void cleanupWarningsAreDisplayedWithoutChangingPackageCommitToFailure() {
        UpdateService service(nullptr, false);
        service.m_activeOperationId = QStringLiteral("fixture");
        service.finishExecutionEvidence(QStringLiteral("fixture"), {
            {QStringLiteral("package_commit"), QStringLiteral("committed")},
            {QStringLiteral("cleanup_status"), QStringLiteral("warnings")},
            {QStringLiteral("cleanup_removed_files"), QStringLiteral("3")},
            {QStringLiteral("cleanup_removed_bytes"), QStringLiteral("8000")},
            {QStringLiteral("cleanup_reclaimed_bytes"), QStringLiteral("4096")},
            {QStringLiteral("cleanup_details"), QStringLiteral("Busy archive retained")}});
        service.finishOperationStatus(QStringLiteral("fixture"), QStringLiteral("ok_with_warnings"), QStringLiteral("Package commit completed"));
        QVERIFY(!service.failed());
        QVERIFY(!service.installing());
        QCOMPARE(service.executionEvidence().value(QStringLiteral("package_commit")).toString(), QStringLiteral("committed"));
        QVERIFY(service.output().contains(QStringLiteral("Cache cleanup: warnings")));
        QVERIFY(service.output().contains(QStringLiteral("Removed files: 3")));
        QVERIFY(service.output().contains(QStringLiteral("4096 bytes")));
        QVERIFY(service.output().contains(QStringLiteral("Busy archive retained")));
    }

    void legacySuccessWithoutEvidenceRequiresVerification()
    {
        UpdateService service(nullptr, false);
        service.m_activeOperationId = QStringLiteral("fixture");
        service.finishExecutionEvidence(QStringLiteral("fixture"), {});
        service.finishOperationStatus(QStringLiteral("fixture"), QStringLiteral("ok"), QStringLiteral("old broker success"));
        QVERIFY(service.verificationRequired());
        QVERIFY(!service.failed());
        QVERIFY(!service.installing());
        QCOMPARE(service.executionEvidence().value(QStringLiteral("package_commit")).toString(), QStringLiteral("unknown"));
    }

    void artifactEvidenceDoesNotClaimAReboot_data()
    {
        QTest::addColumn<QString>("artifactState");
        QTest::newRow("consistent") << QStringLiteral("verified");
        QTest::newRow("broken") << QStringLiteral("failed");
        QTest::newRow("unobservable") << QStringLiteral("unknown");
    }

    void artifactEvidenceDoesNotClaimAReboot()
    {
        QFETCH(QString, artifactState);
        UpdateService service(nullptr, false);
        service.m_activeOperationId = QStringLiteral("fixture");
        service.finishExecutionEvidence(QStringLiteral("fixture"), {
            {QStringLiteral("package_commit"), QStringLiteral("committed")},
            {QStringLiteral("hooks"), QStringLiteral("unknown")},
            {QStringLiteral("boot_preparation"), artifactState},
            {QStringLiteral("kernel_artifacts"), artifactState},
            {QStringLiteral("dkms_artifacts"), artifactState},
            {QStringLiteral("initramfs_artifacts"), artifactState},
            {QStringLiteral("limine_artifacts"), artifactState},
            {QStringLiteral("new_kernel_boot"), QStringLiteral("unknown")},
            {QStringLiteral("running_kernel"), QStringLiteral("old-running")},
            {QStringLiteral("installed_kernels"), QStringLiteral("new-installed")},
            {QStringLiteral("updated_kernels"), QStringLiteral("new-installed")},
            {QStringLiteral("boot_details"), QStringLiteral("fixture inspection details")},
            {QStringLiteral("hook_details"), QStringLiteral("returned (exit status unavailable)")},
        });
        service.finishOperationStatus(QStringLiteral("fixture"), QStringLiteral("ok_with_warnings"), QStringLiteral("Commit completed."));
        QVERIFY(service.verificationRequired());
        QVERIFY(service.output().contains(QStringLiteral("Running kernel: old-running")));
        QVERIFY(service.output().contains(QStringLiteral("Installed kernels (on disk): new-installed")));
        QVERIFY(service.output().contains(QStringLiteral("has not been observed")));
        QVERIFY(service.output().contains(QStringLiteral("fixture inspection details")));
        QVERIFY(service.output().contains(QStringLiteral("exit status unavailable")));
        QCOMPARE(service.executionEvidence().value(QStringLiteral("kernel_artifacts")).toString(), artifactState);
        QVERIFY(!service.installing());
    }

    void unrelatedEvidenceCannotOverwriteActiveOperation()
    {
        UpdateService service(nullptr, false);
        service.m_activeOperationId = QStringLiteral("active");
        service.finishExecutionEvidence(QStringLiteral("other"), {
            {QStringLiteral("hooks"), QStringLiteral("verified")},
        });
        QVERIFY(service.executionEvidence().isEmpty());
    }

    void aurProcessFailureIsNonModalAndPreservesSystemState()
    {
        UpdateService service(nullptr, false);
        service.m_checking = true;
        service.m_pendingChecks = 1;
        service.m_available = true;
        service.m_count = 1;
        service.m_updates = {QVariantMap{
            {QStringLiteral("name"), QStringLiteral("system-package")},
            {QStringLiteral("newVersion"), QStringLiteral("2.0")},
        }};
        service.m_transactionPreview = QStringLiteral("verified preview");
        service.m_transactionPreviewReceived = true;

        QSignalSpy operationErrors(&service, &UpdateService::operationError);
        service.aurProcess.start(QStringLiteral("/bin/sh"), {
            QStringLiteral("-c"),
            QStringLiteral("printf '%s' 'synthetic AUR failure' >&2; exit 1"),
        });

        QVERIFY(service.aurProcess.waitForStarted(1000));
        QTRY_VERIFY_WITH_TIMEOUT(!service.checking(), 3000);

        QCOMPARE(operationErrors.count(), 0);
        QVERIFY(!service.failed());
        QVERIFY(!service.aurScanWarning().isEmpty());
        QVERIFY(service.m_aurScanDiagnostic.contains(QStringLiteral("code 1")));
        QVERIFY(service.m_aurScanDiagnostic.contains(
            QStringLiteral("synthetic AUR failure")));
        QVERIFY(service.available());
        QCOMPARE(service.count(), 1);
        QCOMPARE(service.updates().size(), 1);
        QCOMPARE(service.planPreview(), QStringLiteral("verified preview"));
    }

    void emptyMalformedAndWarningAurScan_data() {
        QTest::addColumn<QString>("command");QTest::addColumn<bool>("complete");
        QTest::newRow("no-updates")<<QString("exit 0")<<true;
        QTest::newRow("malformed-output")<<QString("printf 'unexpected\\n'")<<false;
        QTest::newRow("warning-with-zero-exit")<<QString("printf 'network incomplete\\n' >&2")<<false;
    }
    void emptyMalformedAndWarningAurScan() {
        QFETCH(QString,command);QFETCH(bool,complete);
        UpdateService service(nullptr,false);service.m_checking=true;service.m_pendingChecks=1;
        service.aurProcess.start("/bin/sh",{"-c",command});
        QVERIFY(service.aurProcess.waitForStarted(1000));QTRY_VERIFY_WITH_TIMEOUT(!service.checking(),3000);
        QCOMPARE(service.m_aurScanComplete,complete);QCOMPARE(service.aurScanWarning().isEmpty(),complete);
        QVERIFY(service.aurUpdates().isEmpty());
    }
    void successfulAurScanStillReportsUpdates()
    {
        UpdateService service(nullptr, false);
        service.m_checking = true;
        service.m_pendingChecks = 1;

        QSignalSpy operationErrors(&service, &UpdateService::operationError);
        service.aurProcess.start(QStringLiteral("/bin/sh"), {
            QStringLiteral("-c"),
            QStringLiteral("printf '%s\\n' 'aur-package 1.0 -> 2.0'"),
        });

        QVERIFY(service.aurProcess.waitForStarted(1000));
        QTRY_VERIFY_WITH_TIMEOUT(!service.checking(), 3000);

        QCOMPARE(operationErrors.count(), 0);
        QVERIFY(service.aurAvailable());
        QCOMPARE(service.aurCount(), 1);
        QVERIFY(service.aurScanWarning().isEmpty());
    }

    void officialUpdateFailureStillRaisesOperationError()
    {
        UpdateService service(nullptr, false);
        service.m_checking = true;
        service.m_pendingChecks = 1;

        QSignalSpy operationErrors(&service, &UpdateService::operationError);
        service.finishOfficialScan(false, QStringLiteral("synthetic D-Bus failure"));

        QCOMPARE(operationErrors.count(), 1);
        QCOMPARE(operationErrors.constFirst().at(0).toString(),
                 QStringLiteral("Update scan failed"));
        QVERIFY(operationErrors.constFirst().at(1).toString().contains(
            QStringLiteral("synthetic D-Bus failure")));
        QVERIFY(service.failed());
    }

    void transactionPreviewFailureStillRaisesOperationError()
    {
        UpdateService service(nullptr, false);
        QSignalSpy operationErrors(&service, &UpdateService::operationError);

        service.finishTransactionPreview(
            false, {}, QStringLiteral("synthetic transaction preflight failure"));

        QCOMPARE(operationErrors.count(), 1);
        QCOMPARE(operationErrors.constFirst().at(0).toString(),
                 QStringLiteral("Transaction preview failed"));
        QVERIFY(operationErrors.constFirst().at(1).toString().contains(
            QStringLiteral("synthetic transaction preflight failure")));
    }

    void previewCannotInterruptAnActiveTransactionOrScan() {
        UpdateService service(nullptr, false);
        service.m_planHash = QStringLiteral("preserve-current-plan");
        service.m_installing = true;
        service.previewPlan();
        QCOMPARE(service.planHash(), QStringLiteral("preserve-current-plan"));
        QVERIFY(!service.previewing());
        service.m_installing = false;
        service.m_checking = true;
        service.previewPlan();
        QCOMPARE(service.planHash(), QStringLiteral("preserve-current-plan"));
        QVERIFY(!service.previewing());
    }

    void validatedPreviewShowsRecoveryAndAuthorizationLimits() {
        UpdateService service(nullptr, false);
        service.m_previewing = true;
        service.finishPlanEffects({}, QString(64, QLatin1Char('a')));
        service.finishTransactionPreview(true, {QVariantMap{
            {QStringLiteral("package_name"), QStringLiteral("demo")},
            {QStringLiteral("current_version"), QStringLiteral("1")},
            {QStringLiteral("new_version"), QStringLiteral("2")},
            {QStringLiteral("repository"), QStringLiteral("core")}}}, {});
        QVERIFY(!service.previewing());
        QVERIFY(service.previewReady());
        QVERIFY(service.planPreview().contains(QStringLiteral("RECOVERY NOT YET VERIFIED")));
        QVERIFY(service.planPreview().contains(QStringLiteral("five minutes")));
        QVERIFY(!service.installing());
    }

    void expiredPreviewRequiresNewPlanAndClearsConsent() {
        UpdateService service(nullptr, false);
        service.finishPlanEffects({}, QString(64, QLatin1Char('a')));
        service.finishTransactionPreview(true, {QVariantMap{{"package_name", "demo"}}}, {});
        QVERIFY(service.previewReady());
        service.m_previewExpiryTimer.start(1);
        QTRY_VERIFY(!service.previewReady());
        QVERIFY(service.planHash().isEmpty());
        QSignalSpy errors(&service, &UpdateService::operationError);
        service.install();
        QCOMPARE(errors.size(), 1);
        QVERIFY(!service.installing());
    }

    void uncertainStartBlocksDuplicateAndKeepsOperationForPolling() {
        UpdateService service(nullptr, false);
        emit service.m_updateClient.triggerUpdateUncertain(QStringLiteral("worker_start_failed"), QStringLiteral("fixture"));
        QVERIFY(service.installing());
        QVERIFY(service.verificationRequired());
        QVERIFY(!service.previewReady());
        QCOMPARE(service.m_activeOperationId, QStringLiteral("fixture"));
        QVERIFY(service.m_operationPollTimer.isActive());
        service.finishOperationStatus("fixture", "failed", "archive_verification_failed: identity mismatch");
        QVERIFY(!service.installing());
        QVERIFY(service.failed());
        QVERIFY(service.status().contains("after start"));
        QVERIFY(service.output().contains("archive_verification_failed"));
    }

    void updateCannotStartWithoutAValidPreview()
    {
        UpdateService service(nullptr, false);
        QSignalSpy operationErrors(&service, &UpdateService::operationError);

        service.install();

        QVERIFY(!service.installing());
        QVERIFY(!service.m_previewReady);
        QCOMPARE(operationErrors.count(), 1);
        QCOMPARE(operationErrors.constFirst().at(0).toString(),
                 QStringLiteral("Update preview required"));
    }

    void changedPlanShowsTheSpecificFieldAndRequiresNewPreview() {
        UpdateService service(nullptr,false);
        service.m_activeOperationId = QStringLiteral("fixture");
        service.m_installing = true;
        service.m_previewReady = true;
        service.finishExecutionEvidence(QStringLiteral("fixture"), {{"package_commit","not-started"}});
        QSignalSpy errors(&service,&UpdateService::operationError);
        service.finishOperationStatus(QStringLiteral("fixture"),QStringLiteral("failed"),
            QStringLiteral("plan_changed: Changed canonical fields: demo.sha256: old-digest -> new-digest. Create a new Preview Plan."));
        QVERIFY(service.failed()); QVERIFY(!service.installing()); QVERIFY(!service.previewReady());
        QVERIFY(service.status().contains(QStringLiteral("new Preview Plan")));
        QVERIFY(service.output().contains(QStringLiteral("demo.sha256: old-digest -> new-digest")));
        QVERIFY(service.output().contains(QStringLiteral("commit not started")));
        QCOMPARE(errors.size(),1);
        QCOMPARE(errors.first()[0].toString(),QStringLiteral("Update plan changed"));
        QVERIFY(errors.first()[1].toString().contains(QStringLiteral("demo.sha256")));
    }

    void operationStatusUpdatesOnlyTheActiveTransaction()
    {
        UpdateService service(nullptr, false);
        service.m_activeOperationId = QStringLiteral("active-operation");
        service.m_installing = true;

        service.finishOperationStatus(
            QStringLiteral("other-operation"), QStringLiteral("failed"),
            QStringLiteral("unrelated failure"));
        QVERIFY(service.installing());
        QVERIFY(!service.failed());

        service.finishOperationStatus(
            QStringLiteral("active-operation"), QStringLiteral("failed"),
            QStringLiteral("signature_error: invalid package signature"));
        QVERIFY(!service.installing());
        QVERIFY(service.failed());
        QVERIFY(service.output().contains(QStringLiteral("signature_error")));
    }
};

QTEST_GUILESS_MAIN(UpdateServiceTest)
#include "UpdateServiceTest.moc"
