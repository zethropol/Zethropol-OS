#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QProcess>
#include <QStringDecoder>
#include <QVariantMap>
#include <functional>

class UpdateScanner;
class UpdateInstallerTest;

struct PreUpdateGateResult {
    bool ok = false;
    QString code;
    QString message;
    quint32 snapshotId = 0;
};

struct PostUpdateHealthEvidence {
    bool pacmanLockPresent = false;
    bool pacmanDbChecked = false;
    bool pacmanDbOk = false;
    QString pacmanDbDetail;
    bool systemdChecked = false;
    int failedSystemdUnits = -1;
    QString systemdDetail;
    bool bootVerified = false;
    bool bootDegraded = false;
    bool activeKernelImagePresent = false;
    bool activeKernelCheckDone = false;
    QString bootDetail;
    bool recoverySnapshotVerified = false;
    QString recoveryRecommendedLevel;
    QString recoveryAssessmentStatus;
    QString recoveryAssessmentSummary;
};

class UpdateInstaller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString recoveryGuidance READ recoveryGuidance NOTIFY changed)
    Q_PROPERTY(bool liveExecutionEnabled READ liveExecutionEnabled CONSTANT)
    Q_PROPERTY(bool officialRepoReady READ officialRepoReady NOTIFY changed)
    Q_PROPERTY(bool officialUpdatesAvailable READ officialUpdatesAvailable NOTIFY changed)
    Q_PROPERTY(bool aurUpdatesAvailable READ aurUpdatesAvailable NOTIFY changed)
    Q_PROPERTY(bool flatpakUpdatesAvailable READ flatpakUpdatesAvailable NOTIFY changed)
    Q_PROPERTY(QString activeScope READ activeScope NOTIFY changed)
    Q_PROPERTY(QString healthStatus READ healthStatus NOTIFY changed)
    Q_PROPERTY(QString healthSummary READ healthSummary NOTIFY changed)
    Q_PROPERTY(QString cleanupStatus READ cleanupStatus NOTIFY changed)
    Q_PROPERTY(QString cleanupSummary READ cleanupSummary NOTIFY changed)
    Q_PROPERTY(QString rebootStatus READ rebootStatus NOTIFY changed)
    Q_PROPERTY(bool rebootRequired READ rebootRequired NOTIFY changed)
    Q_PROPERTY(QString rebootSafetyNotice READ rebootSafetyNotice NOTIFY changed)

public:
    explicit UpdateInstaller(UpdateScanner *scanner, QObject *parent = nullptr);
    ~UpdateInstaller() override;

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString message() const { return m_message; }
    QString output() const { return m_output; }
    QString recoveryGuidance() const { return m_recoveryGuidance; }
    bool liveExecutionEnabled() const { return m_liveExecutionEnabled; }
    bool officialRepoReady() const;
    bool officialUpdatesAvailable() const;
    bool aurUpdatesAvailable() const;
    bool flatpakUpdatesAvailable() const;
    QString activeScope() const { return m_activeScope; }
    QString healthStatus() const { return m_healthStatus; }
    QString healthSummary() const { return m_healthSummary; }
    QString cleanupStatus() const { return m_cleanupStatus; }
    QString cleanupSummary() const { return m_cleanupSummary; }
    QString rebootStatus() const { return m_rebootStatus; }
    bool rebootRequired() const { return m_rebootRequired; }
    QString rebootSafetyNotice() const { return m_rebootSafetyNotice; }

    static bool isOfficialRepoScanComplete(const UpdateScanner *scanner, QString *reason = nullptr);
    static bool isCriticalBootUpdate(const UpdateScanner *scanner);
    static bool decodeRecoveryOperationResult(const QDBusMessage &reply,
                                              bool &ok,
                                              QString &code,
                                              QString &message,
                                              QString &operationId);
    static QVariantMap decodeRecoveryStringMap(const QDBusMessage &reply);
    static QVariantMap decodeRecoveryEnvelopeData(const QDBusMessage &reply);

    Q_INVOKABLE void start();
    Q_INVOKABLE void startScoped(const QString &scope);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void sendInput(const QString &text);
    Q_INVOKABLE void sendKey(const QString &text);

signals:
    void changed();
    void outputChanged();
    void sessionFinished();

private:
    friend class UpdateInstallerTest;
    void readOutput();
    void finish(const QString &status, const QString &message);
    void evaluatePostUpdateState();

    UpdateScanner *m_scanner;
    QProcess m_process;
    QString m_sessionTool;
    QString m_updater;
    QString m_pacmanLockPath = QStringLiteral("/var/lib/pacman/db.lck");
    QString m_activeScope = QStringLiteral("official");
    QString m_status = "idle";
    QString m_message;
    QString m_rawOutput;
    QString m_output;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_busy = false;
    bool m_recoveryGateEnabled = true;
    bool m_liveExecutionEnabled = false;
    QString m_updateOperationId;
    quint32 m_verifiedSnapshotId = 0;
    QString m_recoveryGuidance;
    QString m_healthStatus;
    QString m_healthSummary;
    QString m_cleanupStatus;
    QString m_cleanupSummary;
    QString m_rebootStatus;
    bool m_rebootRequired = false;
    QString m_rebootSafetyNotice;
    std::function<PreUpdateGateResult(const QString &updateOpId, bool criticalBootUpdate)> m_recoveryGate;
    std::function<void(const QString &updateOpId, const QString &outcome)> m_recoveryComplete;
    std::function<PostUpdateHealthEvidence(const QString &updateOpId, quint32 snapshotId, const QString &sessionStatus)> m_healthProbe;
};
