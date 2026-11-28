#include "AgentClient.h"

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace {
constexpr const char *kServiceName = "org.zethropol.ZciAgent";
constexpr const char *kObjectPath = "/org/zethropol/ZciAgent";
constexpr const char *kInterfaceName = "org.zethropol.ZciAgent.v1";

QVariantList parseJsonStringList(const QVariant &raw)
{
    QVariantList out;
    const QVariantList items = raw.toList();
    for (const QVariant &item : items) {
        const QJsonDocument doc = QJsonDocument::fromJson(item.toString().toUtf8());
        if (doc.isObject()) {
            out.append(doc.object().toVariantMap());
        }
    }
    return out;
}
} // namespace

AgentClient::AgentClient(const QString &busAddress, QObject *parent)
    : QObject(parent)
    , m_busAddress(busAddress.trimmed())
    , m_connectionName(QStringLiteral("zci-agent-") + QUuid::createUuid().toString(QUuid::Id128))
    , m_workspaceDir(QDir::homePath() + QStringLiteral("/Zethropol-OS"))
{
    QDBusConnection bus = openBus();
    if (bus.isConnected()) {
        bus.connect(QString::fromLatin1(kServiceName),
                    QString::fromLatin1(kObjectPath),
                    QString::fromLatin1(kInterfaceName),
                    QStringLiteral("PlanProposed"),
                    this,
                    SLOT(onPlanProposedSignal(QString,QString,quint32,quint32)));
        bus.connect(QString::fromLatin1(kServiceName),
                    QString::fromLatin1(kObjectPath),
                    QString::fromLatin1(kInterfaceName),
                    QStringLiteral("StepCompleted"),
                    this,
                    SLOT(onStepCompletedSignal(QString,QString,QString,QString,QString,QString)));
        bus.connect(QString::fromLatin1(kServiceName),
                    QString::fromLatin1(kObjectPath),
                    QString::fromLatin1(kInterfaceName),
                    QStringLiteral("PlanFinished"),
                    this,
                    SLOT(onPlanFinishedSignal(QString,QString,quint32,quint32,QString)));
    }
}

AgentClient::~AgentClient()
{
    if (!m_busAddress.isEmpty()) {
        QDBusConnection::disconnectFromBus(m_connectionName);
    }
}

QDBusConnection AgentClient::openBus()
{
    if (!m_busAddress.isEmpty()) {
        QDBusConnection existing = QDBusConnection::connectToBus(m_busAddress, m_connectionName);
        return existing;
    }
    return QDBusConnection::sessionBus();
}

QVariant AgentClient::unwrapDbusValue(const QVariant &value)
{
    if (value.canConvert<QDBusVariant>()) {
        return unwrapDbusValue(value.value<QDBusVariant>().variant());
    }
    if (value.canConvert<QDBusArgument>()) {
        const auto arg = value.value<QDBusArgument>();
        const auto elemType = arg.currentType();
        if (elemType == QDBusArgument::MapType) {
            QVariantMap map;
            arg.beginMap();
            while (!arg.atEnd()) {
                QString key;
                QVariant val;
                arg.beginMapEntry();
                arg >> key >> val;
                arg.endMapEntry();
                map.insert(key, unwrapDbusValue(val));
            }
            arg.endMap();
            return map;
        }
        if (elemType == QDBusArgument::ArrayType) {
            QVariantList list;
            arg.beginArray();
            while (!arg.atEnd()) {
                QVariant item;
                arg >> item;
                list.append(unwrapDbusValue(item));
            }
            arg.endArray();
            return list;
        }
    }
    if (value.typeId() == QMetaType::QVariantMap) {
        QVariantMap src = value.toMap();
        QVariantMap dst;
        for (auto it = src.cbegin(); it != src.cend(); ++it) {
            dst.insert(it.key(), unwrapDbusValue(it.value()));
        }
        return dst;
    }
    if (value.typeId() == QMetaType::QVariantList) {
        QVariantList dst;
        const QVariantList src = value.toList();
        for (const QVariant &item : src) {
            dst.append(unwrapDbusValue(item));
        }
        return dst;
    }
    return value;
}

QVariantMap AgentClient::unwrapDbusMap(const QVariant &value)
{
    return unwrapDbusValue(value).toMap();
}

QVariantMap AgentClient::invokeDbus(const QString &method, const QVariantList &args, int timeoutMs)
{
    QDBusConnection bus = openBus();
    if (!bus.isConnected()) {
        return {
            {QStringLiteral("success"), false},
            {QStringLiteral("code"), QStringLiteral("dbus-unavailable")},
            {QStringLiteral("message"), QStringLiteral("Yerel D-Bus oturum veri yoluna bağlanılamadı.")},
            {QStringLiteral("data"), QVariantMap{}},
        };
    }

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kServiceName),
        QString::fromLatin1(kObjectPath),
        QString::fromLatin1(kInterfaceName),
        method);
    call.setArguments(args);

    const QDBusMessage reply = bus.call(call, QDBus::Block, timeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return {
            {QStringLiteral("success"), false},
            {QStringLiteral("code"), QStringLiteral("dbus-call-failed")},
            {QStringLiteral("message"), reply.errorMessage()},
            {QStringLiteral("data"), QVariantMap{}},
        };
    }
    if (reply.arguments().isEmpty()) {
        return {
            {QStringLiteral("success"), false},
            {QStringLiteral("code"), QStringLiteral("empty-dbus-reply")},
            {QStringLiteral("message"), QStringLiteral("D-Bus servisi boş yanıt döndürdü.")},
            {QStringLiteral("data"), QVariantMap{}},
        };
    }
    return unwrapDbusMap(reply.arguments().first());
}

void AgentClient::setBusy(bool b)
{
    if (m_busy != b) {
        m_busy = b;
        emit busyChanged();
    }
}

void AgentClient::setErrorBanner(const QString &err)
{
    if (m_errorBanner != err) {
        m_errorBanner = err;
        emit errorBannerChanged();
    }
}

void AgentClient::setActiveProfile(const QString &profile)
{
    const QString cleaned = (profile == QStringLiteral("zci-command"))
        ? QStringLiteral("zci-command")
        : QStringLiteral("zekans-code");
    if (m_activeProfile != cleaned) {
        m_activeProfile = cleaned;
        emit activeProfileChanged();
    }
}

void AgentClient::setWorkspaceDir(const QString &dir)
{
    const QString cleaned = dir.trimmed();
    if (m_workspaceDir != cleaned) {
        m_workspaceDir = cleaned;
        emit workspaceDirChanged();
    }
}

void AgentClient::setTestState(const QVariantList &steps,
                               const QVariantList &auditEntries,
                               const QString &snapperSummary,
                               const QString &planId,
                               const QString &planStatus)
{
    m_connected = true;
    m_runtimeState = QStringLiteral("Standby");
    m_vramLocked = false;
    m_activeVramMb = 0;
    m_hardwareProfile = QStringLiteral("dev-baseline");
    m_loopbackOnly = true;
    m_rawShellEnabled = false;
    m_rollbackLocked = true;
    m_repairLocked = true;
    m_auditChainValid = true;
    m_auditPermissionsStrict = true;
    m_auditEntryCount = static_cast<qulonglong>(auditEntries.size());
    m_auditHeadShort = QStringLiteral("a1b2c3d4e5f6");
    m_statusMessage = QStringLiteral("ZCI Komut Ajanı hazır.");
    m_activePlanId = planId;
    m_activePlanStatus = planStatus;
    m_activePlanSummary = QStringLiteral("Çok adımlı doğrulama ve onaylı çalışma alanı planı");
    m_snapperSummary = snapperSummary;
    m_steps = steps;
    m_auditEntries = auditEntries;
    emit statusChanged();
    emit activePlanChanged();
    emit auditEntriesChanged();
}

void AgentClient::applyPlanData(const QVariantMap &data)
{
    m_activePlanId = data.value(QStringLiteral("plan_id")).toString();
    m_activePlanStatus = data.value(QStringLiteral("status")).toString();
    if (data.contains(QStringLiteral("summary"))) {
        m_activePlanSummary = data.value(QStringLiteral("summary")).toString();
    }
    if (data.contains(QStringLiteral("snapper_preflight_summary"))) {
        m_snapperSummary = data.value(QStringLiteral("snapper_preflight_summary")).toString();
    }
    if (data.contains(QStringLiteral("steps_json"))) {
        m_steps = parseJsonStringList(data.value(QStringLiteral("steps_json")));
    }
    emit activePlanChanged();
}

QVariantMap AgentClient::callStatusSync()
{
    return invokeDbus(QStringLiteral("GetAgentStatus"));
}

QVariantMap AgentClient::callProposePlanSync(const QString &goal,
                                             const QString &profile,
                                             const QString &workspace)
{
    return invokeDbus(QStringLiteral("ProposePlan"), {goal, profile, workspace}, 40000);
}

QVariantMap AgentClient::callApproveStepSync(const QString &planId,
                                             const QString &stepId,
                                             bool approved,
                                             const QString &reason)
{
    return invokeDbus(QStringLiteral("ApproveStep"), {planId, stepId, approved, reason}, 10000);
}

QVariantMap AgentClient::callExecutePlanSync(const QString &planId)
{
    return invokeDbus(QStringLiteral("ExecuteApprovedPlan"), {planId}, 40000);
}

QVariantMap AgentClient::callCancelPlanSync(const QString &planId, const QString &reason)
{
    return invokeDbus(QStringLiteral("CancelPlan"), {planId, reason}, 10000);
}

QVariantMap AgentClient::callGetPlanStatusSync(const QString &planId)
{
    return invokeDbus(QStringLiteral("GetPlanStatus"), {planId}, 10000);
}

QVariantMap AgentClient::callAuditTrailSync(int limit)
{
    return invokeDbus(QStringLiteral("GetAuditTrail"), {static_cast<quint32>(qBound(1, limit, 200))}, 10000);
}

void AgentClient::refreshStatus()
{
    const QVariantMap reply = callStatusSync();
    const bool ok = reply.value(QStringLiteral("success")).toBool();
    const QVariantMap d = reply.value(QStringLiteral("data")).toMap();

    m_connected = ok;
    m_statusMessage = reply.value(QStringLiteral("message")).toString();
    if (ok) {
        m_runtimeState = d.value(QStringLiteral("runtime_state"), QStringLiteral("Standby")).toString();
        m_vramLocked = d.value(QStringLiteral("vram_locked"), false).toBool();
        m_activeVramMb = d.value(QStringLiteral("active_vram_mb"), 0).toULongLong();
        m_hardwareProfile = d.value(QStringLiteral("hardware_profile"), QStringLiteral("dev-baseline")).toString();
        m_activeModelId = d.value(QStringLiteral("active_model_id")).toString();
        m_loopbackOnly = d.value(QStringLiteral("loopback_only"), true).toBool();
        m_rawShellEnabled = d.value(QStringLiteral("raw_shell_execution_enabled"), false).toBool();
        m_rollbackLocked = !d.value(QStringLiteral("rollback_execution_enabled"), false).toBool();
        m_repairLocked = !d.value(QStringLiteral("repair_execution_enabled"), false).toBool();
        m_auditChainValid = d.value(QStringLiteral("audit_chain_valid"), false).toBool();
        m_auditPermissionsStrict = d.value(QStringLiteral("audit_permissions_strict"), false).toBool();
        m_auditEntryCount = d.value(QStringLiteral("audit_entry_count"), 0).toULongLong();
        const QString fullHead = d.value(QStringLiteral("audit_head_sha256")).toString();
        m_auditHeadShort = fullHead.left(12);
    }
    emit statusChanged();
    if (ok) {
        refreshPlans();
        refreshAuditTrail(25);
    }
}

void AgentClient::refreshPlans()
{
    const QVariantMap reply = invokeDbus(QStringLiteral("ListPlans"));
    if (reply.value(QStringLiteral("success")).toBool()) {
        const QVariantMap d = reply.value(QStringLiteral("data")).toMap();
        m_plans = parseJsonStringList(d.value(QStringLiteral("plans_json")));
        emit plansChanged();
    }
}

void AgentClient::refreshAuditTrail(int limit)
{
    const QVariantMap reply = callAuditTrailSync(limit);
    const QVariantMap d = reply.value(QStringLiteral("data")).toMap();
    if (!d.isEmpty()) {
        m_auditChainValid = d.value(QStringLiteral("chain_valid"), false).toBool();
        m_auditPermissionsStrict = d.value(QStringLiteral("permissions_strict"), false).toBool();
        m_auditEntryCount = d.value(QStringLiteral("entry_count"), 0).toULongLong();
        m_auditHeadShort = d.value(QStringLiteral("head_sha256")).toString().left(12);
        QVariantList parsed = parseJsonStringList(d.value(QStringLiteral("entries_json")));
        std::reverse(parsed.begin(), parsed.end());
        m_auditEntries = parsed;
        emit statusChanged();
        emit auditEntriesChanged();
    }
}

void AgentClient::proposePlan(const QString &goal)
{
    const QString cleaned = goal.trimmed();
    if (cleaned.isEmpty()) {
        setErrorBanner(QStringLiteral("Plan önermek için görev metni girin."));
        return;
    }
    setBusy(true);
    clearError();
    const QVariantMap reply = callProposePlanSync(cleaned, m_activeProfile, m_workspaceDir);
    setBusy(false);
    if (!reply.value(QStringLiteral("success")).toBool()) {
        setErrorBanner(reply.value(QStringLiteral("message")).toString());
        refreshAuditTrail(25);
        return;
    }
    applyPlanData(reply.value(QStringLiteral("data")).toMap());
    refreshPlans();
    refreshAuditTrail(25);
}

void AgentClient::approveStep(const QString &stepId, bool approved, const QString &reason)
{
    if (m_activePlanId.isEmpty()) {
        return;
    }
    setBusy(true);
    clearError();
    const QVariantMap reply = callApproveStepSync(
        m_activePlanId,
        stepId,
        approved,
        reason.isEmpty() ? (approved ? QStringLiteral("GUI üzerinden onaylandı") : QStringLiteral("GUI üzerinden reddedildi")) : reason);
    setBusy(false);
    if (!reply.value(QStringLiteral("success")).toBool()) {
        setErrorBanner(reply.value(QStringLiteral("message")).toString());
        return;
    }
    selectPlan(m_activePlanId);
    refreshAuditTrail(25);
}

void AgentClient::executeApprovedPlan()
{
    if (m_activePlanId.isEmpty()) {
        return;
    }
    setBusy(true);
    clearError();
    const QVariantMap reply = callExecutePlanSync(m_activePlanId);
    setBusy(false);
    if (!reply.value(QStringLiteral("success")).toBool()) {
        setErrorBanner(reply.value(QStringLiteral("message")).toString());
    }
    selectPlan(m_activePlanId);
    refreshPlans();
    refreshAuditTrail(25);
}

void AgentClient::cancelActivePlan(const QString &reason)
{
    if (m_activePlanId.isEmpty()) {
        return;
    }
    setBusy(true);
    const QVariantMap reply = callCancelPlanSync(
        m_activePlanId,
        reason.isEmpty() ? QStringLiteral("Kullanıcı tarafından iptal edildi") : reason);
    setBusy(false);
    if (!reply.value(QStringLiteral("success")).toBool()) {
        setErrorBanner(reply.value(QStringLiteral("message")).toString());
    }
    selectPlan(m_activePlanId);
    refreshPlans();
    refreshAuditTrail(25);
}

void AgentClient::selectPlan(const QString &planId)
{
    const QVariantMap reply = callGetPlanStatusSync(planId);
    if (reply.value(QStringLiteral("success")).toBool()) {
        applyPlanData(reply.value(QStringLiteral("data")).toMap());
    }
}

void AgentClient::clearError()
{
    setErrorBanner(QString());
}

void AgentClient::onPlanProposedSignal(const QString &planId,
                                       const QString &,
                                       quint32,
                                       quint32)
{
    if (m_activePlanId.isEmpty()) {
        selectPlan(planId);
    }
    refreshPlans();
}

void AgentClient::onStepCompletedSignal(const QString &planId,
                                        const QString &,
                                        const QString &,
                                        const QString &,
                                        const QString &,
                                        const QString &)
{
    if (planId == m_activePlanId) {
        selectPlan(planId);
    }
}

void AgentClient::onPlanFinishedSignal(const QString &planId,
                                       const QString &,
                                       quint32,
                                       quint32,
                                       const QString &)
{
    if (planId == m_activePlanId) {
        selectPlan(planId);
    }
    refreshPlans();
    refreshAuditTrail(25);
}
