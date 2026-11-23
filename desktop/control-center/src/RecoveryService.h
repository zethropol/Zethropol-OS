#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class QDBusPendingCallWatcher;

class RecoveryService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool permissionRequired READ permissionRequired NOTIFY stateChanged)
    Q_PROPERTY(QString detectionStatus READ detectionStatus NOTIFY stateChanged)
    Q_PROPERTY(QString statusTitle READ statusTitle NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString filesystem READ filesystem NOTIFY stateChanged)
    Q_PROPERTY(QString rootSubvolume READ rootSubvolume NOTIFY stateChanged)
    Q_PROPERTY(QString snapshotTool READ snapshotTool NOTIFY stateChanged)
    Q_PROPERTY(QString configName READ configName NOTIFY stateChanged)
    Q_PROPERTY(QStringList availableConfigs READ availableConfigs NOTIFY stateChanged)
    Q_PROPERTY(bool snapperInstalled READ snapperInstalled NOTIFY stateChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY stateChanged)
    Q_PROPERTY(bool snapshotsDirExists READ snapshotsDirExists NOTIFY stateChanged)
    Q_PROPERTY(bool snapshotsDirAccessible READ snapshotsDirAccessible NOTIFY stateChanged)
    Q_PROPERTY(int snapshotCount READ snapshotCount NOTIFY stateChanged)
    Q_PROPERTY(int currentSnapshotId READ currentSnapshotId NOTIFY stateChanged)
    Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recoveryEvents READ recoveryEvents NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recoveryPoints READ recoveryPoints NOTIFY stateChanged)
    Q_PROPERTY(bool actionBusy READ actionBusy NOTIFY stateChanged)
    Q_PROPERTY(QString actionStatus READ actionStatus NOTIFY stateChanged)
    Q_PROPERTY(QString lastOperationId READ lastOperationId NOTIFY stateChanged)
    Q_PROPERTY(QString lastOperationCode READ lastOperationCode NOTIFY stateChanged)
    Q_PROPERTY(int lastCreatedSnapshotId READ lastCreatedSnapshotId NOTIFY stateChanged)
    Q_PROPERTY(bool confirmationPending READ confirmationPending NOTIFY stateChanged)
    Q_PROPERTY(QString pendingDescription READ pendingDescription NOTIFY stateChanged)
    Q_PROPERTY(bool deleteConfirmationPending READ deleteConfirmationPending NOTIFY stateChanged)
    Q_PROPERTY(int pendingDeleteSnapshotId READ pendingDeleteSnapshotId NOTIFY stateChanged)
    Q_PROPERTY(QString pendingDeleteDescription READ pendingDeleteDescription NOTIFY stateChanged)
    Q_PROPERTY(bool preflightBusy READ preflightBusy NOTIFY stateChanged)
    Q_PROPERTY(int selectedRollbackPointId READ selectedRollbackPointId NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap preflightResult READ preflightResult NOTIFY stateChanged)

public:
    explicit RecoveryService(QObject *parent = nullptr);

    bool checking() const;
    bool available() const;
    bool permissionRequired() const;
    QString detectionStatus() const;
    QString statusTitle() const;
    QString statusMessage() const;
    QString filesystem() const;
    QString rootSubvolume() const;
    QString snapshotTool() const;
    QString configName() const;
    QStringList availableConfigs() const;
    bool snapperInstalled() const;
    bool configured() const;
    bool snapshotsDirExists() const;
    bool snapshotsDirAccessible() const;
    int snapshotCount() const;
    int currentSnapshotId() const;
    QVariantList snapshots() const;
    QVariantList recoveryEvents() const;
    QVariantList recoveryPoints() const;
    bool actionBusy() const;
    QString actionStatus() const;
    QString lastOperationId() const;
    QString lastOperationCode() const;
    int lastCreatedSnapshotId() const;
    bool confirmationPending() const;
    QString pendingDescription() const;
    bool deleteConfirmationPending() const;
    int pendingDeleteSnapshotId() const;
    QString pendingDeleteDescription() const;
    bool preflightBusy() const;
    int selectedRollbackPointId() const;
    QVariantMap preflightResult() const;

    static bool isSnapshotProtectedForDeletion(const QVariantMap &point, int currentSnapshotId, QString *reason = nullptr);

    void setLocalFallbackEnabled(bool enabled);
    void setOperationTimeoutMs(int timeoutMs);
    void setDetectionPaths(const QString &snapperBinary,
                           const QString &sysconfFile,
                           const QString &configsDir,
                           const QString &mountinfoFile,
                           const QString &fstabFile,
                           const QString &snapshotsDir);
    void applyStateMap(const QVariantMap &data);

public slots:
    void check();
    void refresh();
    void requestCreateRecoveryPoint(const QString &description);
    void confirmCreateRecoveryPoint();
    void cancelCreateRecoveryPoint();
    void cancelActiveOperation();
    void preflightRollback(int pointId);
    void createRecoveryPoint(const QString &description);
    void requestDeleteRecoveryPoint(int snapshotId);
    void confirmDeleteRecoveryPoint();
    void cancelDeleteRecoveryPoint();
    void deleteRecoveryPoint(int snapshotId);

signals:
    void stateChanged();

private slots:
    void handleDbusReply(QDBusPendingCallWatcher *watcher);
    void handleDbusStateChanged(const QVariantMap &state);
    void handleCreateReply(QDBusPendingCallWatcher *watcher);
    void handleDeleteReply(QDBusPendingCallWatcher *watcher);
    void handlePreflightReply(QDBusPendingCallWatcher *watcher);
    void pollOperationState();
    void handleOperationStateReply(QDBusPendingCallWatcher *watcher);
    void readOutput();
    void readError();

private:
    bool validatePreconditionsForCreate(const QString &description, QString &outTrimmed);
    void startCreateOperation(const QString &trimmedDescription);
    void startLocalDetection();
    void setDisconnectedState(const QString &status,
                              const QString &title,
                              const QString &message,
                              bool permissionReq = false);
    bool parseSnapperJsonOutput(const QString &output, const QString &configName, QString &errorReason);
    static QPair<QString, QString> detectRootFilesystem(const QString &mountinfoPath,
                                                        const QString &fstabPath);

    QProcess process;
    QProcess actionProcess;
    QTimer processTimeoutTimer;
    QTimer operationPollTimer;
    QElapsedTimer operationElapsed;
    bool m_pollInFlight = false;
    int m_operationTimeoutMs = 8000;

    bool m_checking = false;
    bool m_available = false;
    bool m_permissionRequired = false;
    QString m_detectionStatus = QStringLiteral("backend_unavailable");
    QString m_statusTitle = QStringLiteral("Checking recovery information...");
    QString m_statusMessage = QStringLiteral("Inspecting Snapper configuration and Btrfs state...");
    QString m_filesystem = QStringLiteral("Unknown");
    QString m_rootSubvolume = QStringLiteral("/");
    QString m_snapshotTool = QStringLiteral("Unknown");
    QString m_configName;
    QStringList m_availableConfigs;
    bool m_snapperInstalled = false;
    bool m_configured = false;
    bool m_snapshotsDirExists = false;
    bool m_snapshotsDirAccessible = false;
    int m_snapshotCount = 0;
    int m_currentSnapshotId = 0;
    QVariantList m_snapshots;
    QVariantList m_recoveryEvents;
    QVariantList m_recoveryPoints;
    bool m_actionBusy = false;
    QString m_actionStatus;
    QString m_actionOperation;
    QString m_lastOperationId;
    QString m_lastOperationCode;
    int m_lastCreatedSnapshotId = 0;
    bool m_confirmationPending = false;
    QString m_pendingDescription;
    bool m_deleteConfirmationPending = false;
    int m_pendingDeleteSnapshotId = 0;
    QString m_pendingDeleteDescription;
    bool m_preflightBusy = false;
    int m_selectedRollbackPointId = 0;
    QVariantMap m_preflightResult;

    bool m_localFallbackEnabled = false;
    QString m_snapperBinary = QStringLiteral("/usr/bin/snapper");
    QString m_sysconfFile = QStringLiteral("/etc/conf.d/snapper");
    QString m_configsDir = QStringLiteral("/etc/snapper/configs");
    QString m_mountinfoFile = QStringLiteral("/proc/self/mountinfo");
    QString m_fstabFile = QStringLiteral("/etc/fstab");
    QString m_snapshotsDir = QStringLiteral("/.snapshots");
};
