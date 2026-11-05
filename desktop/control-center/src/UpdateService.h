#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>
#include <QStringList>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include "core/zethropol_update_client.h"
#include "SourceReview.h"
#include "SourceUpdateRunner.h"
#include <QHash>

class UpdateService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool officialSelected READ officialSelected NOTIFY stateChanged)
    Q_PROPERTY(bool officialTransactionRequired READ officialTransactionRequired NOTIFY stateChanged)
    Q_PROPERTY(QString reviewHash READ reviewHash NOTIFY stateChanged)
    Q_PROPERTY(bool blockedSources READ blockedSources NOTIFY stateChanged)
    Q_PROPERTY(bool applicableSources READ applicableSources NOTIFY stateChanged)
    Q_PROPERTY(QString aurStatus READ aurStatus NOTIFY stateChanged)
    Q_PROPERTY(bool aurInstallReady READ aurInstallReady NOTIFY stateChanged)
    Q_PROPERTY(QString sourceReport READ sourceReport NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap sourceResults READ sourceResults NOTIFY stateChanged)
    Q_PROPERTY(QString batchStatus READ batchStatus NOTIFY stateChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool installing READ installing NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString output READ output NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap executionEvidence READ executionEvidence NOTIFY stateChanged)
    Q_PROPERTY(bool verificationRequired READ verificationRequired NOTIFY stateChanged)
    Q_PROPERTY(QVariantList updates READ updates NOTIFY stateChanged)
    Q_PROPERTY(QStringList orphanPackages READ orphanPackages NOTIFY stateChanged)
    Q_PROPERTY(int orphanCount READ orphanCount NOTIFY stateChanged)
    Q_PROPERTY(bool removingOrphans READ removingOrphans NOTIFY stateChanged)
    Q_PROPERTY(bool aurAvailable READ aurAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString aurScanWarning READ aurScanWarning NOTIFY stateChanged)
    Q_PROPERTY(bool removalApprovalRequired READ removalApprovalRequired NOTIFY stateChanged)
    Q_PROPERTY(QString planHash READ planHash NOTIFY stateChanged)
    Q_PROPERTY(bool previewReady READ previewReady NOTIFY stateChanged)
    Q_PROPERTY(bool flatpakAvailable READ flatpakAvailable NOTIFY stateChanged)
    Q_PROPERTY(int aurCount READ aurCount NOTIFY stateChanged)
    Q_PROPERTY(int flatpakCount READ flatpakCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList aurUpdates READ aurUpdates NOTIFY stateChanged)
    Q_PROPERTY(QVariantList flatpakUpdates READ flatpakUpdates NOTIFY stateChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY stateChanged)
    Q_PROPERTY(QString planPreview READ planPreview NOTIFY stateChanged)

public:
    explicit UpdateService(QObject *parent = nullptr);

    bool officialSelected() const { return m_officialSelected; }
    bool officialTransactionRequired() const { return m_officialSelected && !m_officialPreviewPackages.isEmpty(); }
    QString reviewHash() const { return m_reviewHash; }
    bool blockedSources() const;
    bool applicableSources() const;
    bool aurInstallReady() const;
    QString aurStatus() const { return m_sourceResults.value("aur").toMap().value("detail", "AUR: select targets and Preview Plan; installation remains gated by the broker.").toString(); }
    Q_INVOKABLE void confirmAurInstall(const QString &hash, bool approved);
    Q_INVOKABLE void confirmAurCancellation(const QString &operationId, bool approved);
    Q_INVOKABLE void requestAurInstallConfirmation();
    Q_INVOKABLE void requestAurCancellation();
    Q_INVOKABLE void inspectAurOperation(const QString &operationId);
    Q_INVOKABLE void reserveAurBuild(const QString &approvedReviewSha256);
    Q_INVOKABLE void previewAurBundle(const QString &bundlePath);
    Q_INVOKABLE void startAurBuild();
    Q_INVOKABLE void refreshAurAttestation();
    Q_INVOKABLE void requestLocalAurCancellation();
    Q_INVOKABLE void confirmLocalAurCancellation(const QString &execution,bool approved);
    Q_INVOKABLE void buildAurRequest(const QString &requestPath);
    QString sourceReport() const;
    QVariantMap sourceResults() const { return m_sourceResults; }
    QString batchStatus() const { return m_batchStatus; }
    Q_INVOKABLE void selectOfficialSource(bool selected);
    Q_INVOKABLE void confirmSkippedSources(const QString &hash,bool approved);
    bool previewing() const { return m_previewing; }
    bool checking() const;
    bool installing() const;
    bool available() const;
    bool failed() const;
    int count() const;
    QString status() const;
    QString output() const;
    QVariantMap executionEvidence() const { return m_executionEvidence; }
    bool verificationRequired() const { return m_verificationRequired; }
    QVariantList updates() const;
    QStringList orphanPackages() const;
    int orphanCount() const;
    bool removingOrphans() const;
    bool aurAvailable() const;
    QString aurScanWarning() const;
    bool removalApprovalRequired() const { return !m_planRemovals.isEmpty(); }
    QString planHash() const { return m_planHash; }
    Q_INVOKABLE void confirmRemovals(const QString &hash, bool approved);
    bool previewReady() const;
    bool flatpakAvailable() const;
    int aurCount() const;
    int flatpakCount() const;
    QVariantList aurUpdates() const;
    QVariantList flatpakUpdates() const;
    int selectedCount() const;
    QString planPreview() const;

    Q_INVOKABLE void setPackageSelected(
        const QString &source, const QString &name, bool selected);

public slots:
    void check();
    void previewPlan();
    void install();
    void removeOrphans();

signals:
    void localAurCancellationConfirmationRequested(const QString &execution);
    void aurInstallConfirmationRequested(const QString &hash, const QString &details);
    void aurCancellationConfirmationRequested(const QString &operationId);
    void stateChanged();
    void operationError(const QString &title, const QString &details);

private slots:
    void finishOfficialScan(bool success, const QString &message);
    void finishPlanEffects(const QVariantList &removals, const QString &hash);
    void finishTransactionPreview(bool success, const QVariantList &packages, const QString &message);
    void finishOperationStatus(const QString &operationId, const QString &status, const QString &message);
    void finishExecutionEvidence(const QString &operationId, const QVariantMap &evidence);
    void finishOrphanScan();
    void finishAurScan();
    void finishFlatpakScan();

private:
    friend class UpdateServiceTest;

    UpdateService(QObject *parent, bool runInitialCheck);

    ZethropolUpdateClient m_updateClient;
    QTimer m_operationPollTimer;
    QTimer m_previewExpiryTimer;
    QElapsedTimer m_previewClock;
    QString m_activeOperationId;
    QVariantMap m_executionEvidence;
    bool m_verificationRequired = false;
    QProcess orphanProcess;
    QProcess aurProcess;
    QProcess flatpakProcess;
    QProcess flatpakUserProcess;
    SourceReview m_sourceReview;
    SourceUpdateRunner m_sourceRunner;
    QVariantMap m_sourceResults;
    QVariantList m_reviewPlan, m_applyPlan, m_officialPreviewPackages;
    QHash<QString,bool> m_selections;
    QString m_reviewHash, m_selectionAtReview, m_approvedSkipHash, m_batchStatus = "idle";
    bool m_officialSelected = true, m_aurScanComplete = false;
    bool m_officialPreviewDone = false, m_officialPreviewOk = false, m_sourceReviewDone = true, m_reviewFinalized = false;
    bool m_batchActive = false;
    bool m_aurBrokerReviewDone = true;
    bool m_aurOwnsInstallFlag = false;
    bool m_aurBundlePending = false;
    QVariantMap m_aurBrokerReview;
    QString m_aurOperationId;
    QString m_localAurExecution, m_localAurOperation;
    bool m_aurProvisioningPending=false, m_aurAttestationPending=false;
    void startProvisionedBuild(const QByteArray &bytes);
    QString m_aurRequestSelection, m_aurReceiptSelection, m_aurReceiptHash;
    bool m_aurReservationPending = false;
    bool m_aurCancelPending = false;
    bool m_aurConsentPending = false;
    QElapsedTimer m_aurReceiptClock;
    QTimer m_aurOperationPoll;
    void finishAurBrokerReview(bool success,const QVariantMap &report);
    void finishAurOperation(const QString &operationId,const QVariantMap &state);
    QHash<QString,bool> m_flatpakScans;
    void finishFlatpakScope(QProcess &process,const QString &scope);
    void finishSourceReview(const QVariantList &plan);
    void finalizeReview();
    void startExternalStages();
    void finishSourceBatch(const QVariantList &results);
    QVariantList selectedRows(const QVariantList &rows) const;
    QString selectionKey() const;
    void rememberSelections(const QString &source,const QVariantList &rows);
    void restoreSelections(const QString &source,QVariantList &rows);
    void setSourceState(const QString &source,const QString &state,const QString &detail);
    void setSourceScan(const QString &source,const QString &state,const QString &detail={});
    void invalidateReview();
    bool m_previewing = false;
    bool m_checking = false;
    bool m_installing = false;
    bool m_removingOrphans = false;
    bool m_available = false;
    bool m_failed = false;
    int m_count = 0;
    QString m_status;
    QString m_output;
    QString m_transactionPreview;
    bool m_transactionPreviewReceived = false;
    bool m_previewReady = false;
    QVariantList m_planRemovals;
    QString m_planHash;
    QString m_approvedRemovalHash;
    QVariantList m_updates;
    QStringList m_orphanPackages;
    bool m_aurAvailable = false;
    QString m_aurScanWarning;
    QString m_aurScanDiagnostic;
    bool m_flatpakAvailable = false;
    QVariantList m_aurUpdates;
    QVariantList m_flatpakUpdates;
    QString m_aurHelper;
    int m_pendingChecks = 0;
    QStringList m_scanErrors;
    QStringList m_scanWarnings;
    void handleScanStartFailure(const QString &source, const QString &error);
    void handleAurScanStartFailure(const QString &error);
    void recordAurScanFailure(const QString &diagnostic);
    void finishScanIfComplete();
};
