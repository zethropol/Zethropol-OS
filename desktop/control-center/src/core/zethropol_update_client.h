#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>

class ZethropolUpdateClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap updateState READ updateState NOTIFY updateStateChanged)
    Q_PROPERTY(QVariantList pendingUpdates READ pendingUpdates NOTIFY pendingUpdatesChanged)
public:
    explicit ZethropolUpdateClient(QObject *parent = nullptr);
    QVariantMap updateState() const { return m_updateState; }
    QVariantList pendingUpdates() const { return m_pendingUpdates; }

public slots:
    void refreshUpdateState();
    void requestTransactionPreview();
    void triggerSystemUpdate(const QString &planHash, bool approveRemovals);
    void checkOperationStatus(const QString &operationId);
    void reviewAurSelection(const QVariantList &selection);
    void prepareAurSelection(const QVariantList &selection);
    void requestAurProvisioning(const QString &operationId);
    void submitAurBuild(const QString &operationId,const QVariantList &archives);
    void fetchAurAttestation(const QString &operationId);
    void releaseAurBuild(const QString &operationId);
    void reserveAurBuild(const QString &approvedReviewSha256);
    void previewAurBundle(const QString &bundlePath);
    void previewAurReceipt(const QString &operationId, const QDBusUnixFileDescriptor &receipt,
        const QDBusUnixFileDescriptor &receiptSignature, const QDBusUnixFileDescriptor &manifest,
        const QDBusUnixFileDescriptor &manifestSignature, const QList<QDBusUnixFileDescriptor> &archives,
        const QList<QDBusUnixFileDescriptor> &signatures);
    void installAur(const QString &planHash, const QList<QDBusUnixFileDescriptor> &archives,
        const QList<QDBusUnixFileDescriptor> &signatures);
    void discardAurPreview();
    void installPreparedAur(const QString &planHash);
    void checkAurOperationStatus(const QString &operationId);
    void cancelAur(const QString &operationId);

signals:
    void aurProvisioningReceived(const QString &operationId,const QVariantMap &delivery);
    void aurBuildSubmitted(const QString &operationId);
    void aurAttestationUnavailable(const QString &operationId,const QString &reason);
    void aurReservationReleased(const QString &operationId,bool success,const QString &reason);
    void aurCancellationAcknowledged(const QString &operationId);
    void aurSelectionReviewed(bool success, const QVariantMap &report);
    void aurReservationCreated(const QString &operationId);
    void aurBuildContextReceived(const QVariantMap &context);
    void aurReceiptPreviewFinished(bool success, const QString &operationId,
        const QVariantList &packages, const QString &planHash, const QString &error);
    void aurOperationStarted(const QString &operationId);
    void aurOperationStateReceived(const QString &operationId, const QVariantMap &state);
    void aurRequestFailed(const QString &operationId, const QString &message, bool uncertain);
    void updateStateChanged();
    void pendingUpdatesChanged();
    void refreshFinished(bool success, const QString &message);
    void transactionPlanReceived(const QVariantList &removals, const QString &planHash);
    void transactionPreviewFinished(bool success, const QVariantList &packages, const QString &message);
    void operationInitiated(const QString &operationId);
    void operationStatusReceived(const QString &operationId, const QString &status, const QString &message);
    void operationEvidenceReceived(const QString &operationId, const QVariantMap &evidence);
    void operationStatusQueryFailed(const QString &operationId, const QString &message);
    void triggerUpdateFailed(const QString &message);
    void triggerUpdateUncertain(const QString &message, const QString &operationId);
    void errorOccurred(const QString &message);

private slots:
    void handleTriggerReply(const QDBusMessage &reply);
    void handleUpdateSignal(const QVariant &newState);

private:
    void aurStringRequest(const QString &method, const QVariantList &arguments, const QString &operationId);
    QString m_aurPreparedId, m_aurPreparedHash;
    QElapsedTimer m_aurPreparedClock;
    QList<QDBusUnixFileDescriptor> m_aurPreparedArchives, m_aurPreparedSignatures;
    bool m_aurBundleBusy = false;
    QDBusInterface *m_interface = nullptr;
    QVariantMap m_updateState;
    QVariantList m_pendingUpdates;
};
