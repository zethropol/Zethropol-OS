#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class CompatibilityManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool launching READ launching NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)

    Q_PROPERTY(QString wineVersion READ wineVersion NOTIFY stateChanged)
    Q_PROPERTY(QString wineMode READ wineMode NOTIFY stateChanged)
    Q_PROPERTY(bool vulkanAvailable READ vulkanAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool hardwareVulkan13 READ hardwareVulkan13 NOTIFY stateChanged)
    Q_PROPERTY(QString vulkanGpuName READ vulkanGpuName NOTIFY stateChanged)
    Q_PROPERTY(QString vulkanApiVersion READ vulkanApiVersion NOTIFY stateChanged)
    Q_PROPERTY(QString vulkanReason READ vulkanReason NOTIFY stateChanged)
    Q_PROPERTY(bool dxvkAvailable READ dxvkAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString dxvkSource READ dxvkSource NOTIFY stateChanged)
    Q_PROPERTY(bool vkd3dAvailable READ vkd3dAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool bwrapAvailable READ bwrapAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString bwrapPath READ bwrapPath NOTIFY stateChanged)

    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY stateChanged)
    Q_PROPERTY(int profileCount READ profileCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap lastDryRunPlan READ lastDryRunPlan NOTIFY stateChanged)

public:
    explicit CompatibilityManager(QObject *parent = nullptr);
    ~CompatibilityManager() override;

    bool available() const;
    bool busy() const;
    bool launching() const;
    QString statusMessage() const;
    QString lastError() const;

    QString wineVersion() const;
    QString wineMode() const;
    bool vulkanAvailable() const;
    bool hardwareVulkan13() const;
    QString vulkanGpuName() const;
    QString vulkanApiVersion() const;
    QString vulkanReason() const;
    bool dxvkAvailable() const;
    QString dxvkSource() const;
    bool vkd3dAvailable() const;
    bool bwrapAvailable() const;
    QString bwrapPath() const;

    QVariantList profiles() const;
    int profileCount() const;
    QVariantMap lastDryRunPlan() const;

    // Testability / explicit runtime configuration hooks (never touch real user prefix dirs in tests)
    void setPrefixCtlPath(const QString &path);
    void setWineRunPath(const QString &path);
    void setProcessEnvironment(const QProcessEnvironment &env);
    void setCommandTimeoutMs(int timeoutMs);
    void setInitTimeoutMs(int timeoutMs);

    static bool isValidProfileName(const QString &name);
    static bool isValidTemplate(const QString &templateName);
    static bool isValidRenderer(const QString &renderer);
    static bool isSupportedTargetPath(const QString &filePath);

    Q_INVOKABLE QString localPathFromUrl(const QUrl &url) const;

public slots:
    void refresh();
    void initProfile(const QString &profile,
                     const QString &templateName = QStringLiteral("default-win64"),
                     const QString &renderer = QStringLiteral("auto"),
                     bool sandboxEnabled = true,
                     bool force = false);
    void auditProfile(const QString &profile, bool fix = false);
    void syncShortcuts(const QString &profile);
    void removeProfile(const QString &profile, bool userConfirmed = false);
    void previewLaunch(const QString &targetPathOrUrl,
                       const QString &profile = QStringLiteral("default-win64"),
                       const QString &renderer = QStringLiteral("auto"),
                       bool sandboxEnabled = true);
    void launchTarget(const QString &targetPathOrUrl,
                      const QString &profile = QStringLiteral("default-win64"),
                      const QString &renderer = QStringLiteral("auto"),
                      bool sandboxEnabled = true);

signals:
    void stateChanged();
    void refreshFinished(bool ok);
    void operationFinished(const QString &operation, bool ok, const QString &message);
    void dryRunCompleted(bool ok, const QVariantMap &plan);
    void launchFinished(const QString &targetPath, int exitCode, const QString &message);

private:
    enum class ActiveOp {
        None,
        RefreshProbe,
        RefreshList,
        RefreshAudit,
        InitProfile,
        AuditProfile,
        SyncShortcuts,
        RemoveProfile,
        DryRun
    };

    QString resolvePrefixCtl() const;
    QString resolveWineRun() const;
    QString normalizeTargetPath(const QString &targetPathOrUrl) const;
    QProcessEnvironment effectiveEnvironment() const;

    void startCommand(ActiveOp op,
                      const QString &program,
                      const QStringList &args,
                      int timeoutMs,
                      const QString &contextProfile = QString());
    void handleActiveProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleActiveProcessError(QProcess::ProcessError error);
    void handleActiveProcessTimeout();
    void finishActiveOp(bool ok, const QString &opName, const QString &message, const QString &errorDetail = QString());

    void startNextAuditInRefresh();
    bool parseProbeJson(const QByteArray &stdoutBytes, QString *errorOut);
    bool parseListJson(const QByteArray &stdoutBytes, QString *errorOut);
    bool applyAuditJson(const QString &profileName, const QByteArray &stdoutBytes, QString *errorOut);
    bool parseDryRunJson(const QByteArray &stdoutBytes, QString *errorOut);

    bool m_available = false;
    bool m_busy = false;
    bool m_launching = false;
    QString m_statusMessage = QStringLiteral("Uyumluluk katmanı kontrol ediliyor...");
    QString m_lastError;

    QString m_wineVersion = QStringLiteral("Bilinmiyor");
    QString m_wineMode = QStringLiteral("unknown");
    bool m_vulkanAvailable = false;
    bool m_hardwareVulkan13 = false;
    QString m_vulkanGpuName = QStringLiteral("Bilinmiyor");
    QString m_vulkanApiVersion = QStringLiteral("N/A");
    QString m_vulkanReason;
    bool m_dxvkAvailable = false;
    QString m_dxvkSource = QStringLiteral("none");
    bool m_vkd3dAvailable = false;
    bool m_bwrapAvailable = false;
    QString m_bwrapPath = QStringLiteral("not-installed");

    QVariantList m_profiles;
    QVariantMap m_lastDryRunPlan;

    QString m_customPrefixCtl;
    QString m_customWineRun;
    bool m_hasCustomEnv = false;
    QProcessEnvironment m_customEnv;
    int m_commandTimeoutMs = 15000;
    int m_initTimeoutMs = 120000;

    QTimer m_initialRefreshTimer;
    QTimer m_opTimeoutTimer;
    QPointer<QProcess> m_activeProcess;
    ActiveOp m_activeOp = ActiveOp::None;
    QString m_activeContextProfile;
    bool m_activeTimedOut = false;
    QStringList m_pendingAuditProfiles;
    QList<QPointer<QProcess>> m_launchedProcesses;
};
