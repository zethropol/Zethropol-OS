#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class AgentClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString runtimeState READ runtimeState NOTIFY statusChanged)
    Q_PROPERTY(bool vramLocked READ vramLocked NOTIFY statusChanged)
    Q_PROPERTY(qulonglong activeVramMb READ activeVramMb NOTIFY statusChanged)
    Q_PROPERTY(QString hardwareProfile READ hardwareProfile NOTIFY statusChanged)
    Q_PROPERTY(QString activeModelId READ activeModelId NOTIFY statusChanged)
    Q_PROPERTY(bool loopbackOnly READ loopbackOnly NOTIFY statusChanged)
    Q_PROPERTY(bool rawShellEnabled READ rawShellEnabled NOTIFY statusChanged)
    Q_PROPERTY(bool rollbackLocked READ rollbackLocked NOTIFY statusChanged)
    Q_PROPERTY(bool repairLocked READ repairLocked NOTIFY statusChanged)
    Q_PROPERTY(bool auditChainValid READ auditChainValid NOTIFY statusChanged)
    Q_PROPERTY(bool auditPermissionsStrict READ auditPermissionsStrict NOTIFY statusChanged)
    Q_PROPERTY(qulonglong auditEntryCount READ auditEntryCount NOTIFY statusChanged)
    Q_PROPERTY(QString auditHeadShort READ auditHeadShort NOTIFY statusChanged)
    Q_PROPERTY(QString activePlanId READ activePlanId NOTIFY activePlanChanged)
    Q_PROPERTY(QString activePlanStatus READ activePlanStatus NOTIFY activePlanChanged)
    Q_PROPERTY(QString activePlanSummary READ activePlanSummary NOTIFY activePlanChanged)
    Q_PROPERTY(QString snapperSummary READ snapperSummary NOTIFY activePlanChanged)
    Q_PROPERTY(QString activeProfile READ activeProfile WRITE setActiveProfile NOTIFY activeProfileChanged)
    Q_PROPERTY(QString workspaceDir READ workspaceDir WRITE setWorkspaceDir NOTIFY workspaceDirChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(QString errorBanner READ errorBanner NOTIFY errorBannerChanged)
    Q_PROPERTY(QVariantList plans READ plans NOTIFY plansChanged)
    Q_PROPERTY(QVariantList steps READ steps NOTIFY activePlanChanged)
    Q_PROPERTY(QVariantList auditEntries READ auditEntries NOTIFY auditEntriesChanged)

public:
    explicit AgentClient(const QString &busAddress = QString(), QObject *parent = nullptr);
    ~AgentClient() override;

    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    QString runtimeState() const { return m_runtimeState; }
    bool vramLocked() const { return m_vramLocked; }
    qulonglong activeVramMb() const { return m_activeVramMb; }
    QString hardwareProfile() const { return m_hardwareProfile; }
    QString activeModelId() const { return m_activeModelId; }
    bool loopbackOnly() const { return m_loopbackOnly; }
    bool rawShellEnabled() const { return m_rawShellEnabled; }
    bool rollbackLocked() const { return m_rollbackLocked; }
    bool repairLocked() const { return m_repairLocked; }
    bool auditChainValid() const { return m_auditChainValid; }
    bool auditPermissionsStrict() const { return m_auditPermissionsStrict; }
    qulonglong auditEntryCount() const { return m_auditEntryCount; }
    QString auditHeadShort() const { return m_auditHeadShort; }
    QString activePlanId() const { return m_activePlanId; }
    QString activePlanStatus() const { return m_activePlanStatus; }
    QString activePlanSummary() const { return m_activePlanSummary; }
    QString snapperSummary() const { return m_snapperSummary; }
    QString activeProfile() const { return m_activeProfile; }
    QString workspaceDir() const { return m_workspaceDir; }
    QString statusMessage() const { return m_statusMessage; }
    QString errorBanner() const { return m_errorBanner; }
    QVariantList plans() const { return m_plans; }
    QVariantList steps() const { return m_steps; }
    QVariantList auditEntries() const { return m_auditEntries; }

    void setActiveProfile(const QString &profile);
    void setWorkspaceDir(const QString &dir);

    // Test-only state injection helpers for deterministic QML rendering tests
    void setTestState(const QVariantList &steps,
                      const QVariantList &auditEntries,
                      const QString &snapperSummary,
                      const QString &planId,
                      const QString &planStatus);

    Q_INVOKABLE void refreshStatus();
    Q_INVOKABLE void refreshPlans();
    Q_INVOKABLE void refreshAuditTrail(int limit = 25);
    Q_INVOKABLE void proposePlan(const QString &goal);
    Q_INVOKABLE void approveStep(const QString &stepId, bool approved, const QString &reason = QString());
    Q_INVOKABLE void executeApprovedPlan();
    Q_INVOKABLE void cancelActivePlan(const QString &reason = QString());
    Q_INVOKABLE void selectPlan(const QString &planId);
    Q_INVOKABLE void clearError();

    // Synchronous headless CLI helpers
    QVariantMap callStatusSync();
    QVariantMap callProposePlanSync(const QString &goal,
                                    const QString &profile,
                                    const QString &workspace);
    QVariantMap callApproveStepSync(const QString &planId,
                                    const QString &stepId,
                                    bool approved,
                                    const QString &reason);
    QVariantMap callExecutePlanSync(const QString &planId);
    QVariantMap callCancelPlanSync(const QString &planId, const QString &reason);
    QVariantMap callGetPlanStatusSync(const QString &planId);
    QVariantMap callAuditTrailSync(int limit = 30);

    static QVariant unwrapDbusValue(const QVariant &value);
    static QVariantMap unwrapDbusMap(const QVariant &value);

signals:
    void statusChanged();
    void busyChanged();
    void activePlanChanged();
    void activeProfileChanged();
    void workspaceDirChanged();
    void errorBannerChanged();
    void plansChanged();
    void auditEntriesChanged();

private slots:
    void onPlanProposedSignal(const QString &planId,
                              const QString &agentProfile,
                              quint32 stepCount,
                              quint32 approvalRequiredCount);
    void onStepCompletedSignal(const QString &planId,
                               const QString &stepId,
                               const QString &status,
                               const QString &riskLevel,
                               const QString &policyRuleId,
                               const QString &auditHash);
    void onPlanFinishedSignal(const QString &planId,
                              const QString &status,
                              quint32 completedSteps,
                              quint32 deniedSteps,
                              const QString &auditHeadSha256);

private:
    QDBusConnection openBus();
    QVariantMap invokeDbus(const QString &method, const QVariantList &args = {}, int timeoutMs = 10000);
    void setBusy(bool b);
    void setErrorBanner(const QString &err);
    void applyPlanData(const QVariantMap &data);

    QString m_busAddress;
    QString m_connectionName;
    bool m_connected = false;
    bool m_busy = false;
    QString m_runtimeState = QStringLiteral("Standby");
    bool m_vramLocked = false;
    qulonglong m_activeVramMb = 0;
    QString m_hardwareProfile = QStringLiteral("dev-baseline");
    QString m_activeModelId;
    bool m_loopbackOnly = true;
    bool m_rawShellEnabled = false;
    bool m_rollbackLocked = true;
    bool m_repairLocked = true;
    bool m_auditChainValid = true;
    bool m_auditPermissionsStrict = true;
    qulonglong m_auditEntryCount = 0;
    QString m_auditHeadShort = QStringLiteral("000000000000");
    QString m_activePlanId;
    QString m_activePlanStatus = QStringLiteral("idle");
    QString m_activePlanSummary;
    QString m_snapperSummary;
    QString m_activeProfile = QStringLiteral("zekans-code");
    QString m_workspaceDir;
    QString m_statusMessage = QStringLiteral("ZCI Komut Ajanı & ZEKANS bağlantısı bekleniyor...");
    QString m_errorBanner;
    QVariantList m_plans;
    QVariantList m_steps;
    QVariantList m_auditEntries;
};
