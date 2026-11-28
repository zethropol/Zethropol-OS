#include "ChatClient.h"

#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace zethropol::chat {

ChatClient::ChatClient(QObject *parent)
    : QObject(parent)
{}

ChatClient::~ChatClient()
{
    if (!m_connectionName.isEmpty()) {
        QDBusConnection::disconnectFromBus(m_connectionName);
    }
}

bool ChatClient::connectToBus(const QString &customBusAddress)
{
    if (!m_connectionName.isEmpty()) {
        QDBusConnection::disconnectFromBus(m_connectionName);
        m_connectionName.clear();
    }

    QDBusConnection conn = QDBusConnection::sessionBus();
    if (!customBusAddress.trimmed().isEmpty()) {
        m_connectionName = QStringLiteral("zci-chat-%1")
                               .arg(QUuid::createUuid().toString(QUuid::Id128));
        conn = QDBusConnection::connectToBus(customBusAddress.trimmed(), m_connectionName);
    }

    m_busConnected = conn.isConnected();
    if (!m_busConnected) {
        m_connected = false;
        setError(QStringLiteral("D-Bus oturum veri yoluna bağlanılamadı."));
        emit statusChanged();
        return false;
    }

    conn.connect(QString::fromLatin1(kBusName),
                 QString::fromLatin1(kObjectPath),
                 QString::fromLatin1(kInterface),
                 QStringLiteral("MessageReceived"),
                 this,
                 SLOT(onDbusMessageReceived(QString, QString, QString, QString, bool, QString)));
    conn.connect(QString::fromLatin1(kBusName),
                 QString::fromLatin1(kObjectPath),
                 QString::fromLatin1(kInterface),
                 QStringLiteral("SessionCleared"),
                 this,
                 SLOT(onDbusSessionCleared(QString, QString)));
    return true;
}

QVariant ChatClient::unwrapDbusVariant(const QVariant &value)
{
    if (!value.isValid()) {
        return {};
    }
    if (value.canConvert<QDBusVariant>()) {
        return unwrapDbusVariant(value.value<QDBusVariant>().variant());
    }
    if (value.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = value.value<QDBusArgument>();
        const auto kind = arg.currentType();
        if (kind == QDBusArgument::MapType) {
            QVariantMap map;
            arg.beginMap();
            while (!arg.atEnd()) {
                QString key;
                QVariant val;
                arg.beginMapEntry();
                arg >> key >> val;
                arg.endMapEntry();
                map.insert(key, unwrapDbusVariant(val));
            }
            arg.endMap();
            return map;
        }
        if (kind == QDBusArgument::ArrayType) {
            QVariantList list;
            arg.beginArray();
            while (!arg.atEnd()) {
                QVariant elem;
                arg >> elem;
                list.append(unwrapDbusVariant(elem));
            }
            arg.endArray();
            return list;
        }
    }
    if (value.typeId() == QMetaType::QVariantMap) {
        const QVariantMap in = value.toMap();
        QVariantMap out;
        for (auto it = in.constBegin(); it != in.constEnd(); ++it) {
            out.insert(it.key(), unwrapDbusVariant(it.value()));
        }
        return out;
    }
    if (value.typeId() == QMetaType::QVariantList) {
        const QVariantList in = value.toList();
        QVariantList out;
        out.reserve(in.size());
        for (const QVariant &item : in) {
            out.append(unwrapDbusVariant(item));
        }
        return out;
    }
    return value;
}

QVariantMap ChatClient::invokeEnvelope(const QString &method, const QVariantList &args)
{
    QVariantMap fail;
    fail.insert(QStringLiteral("success"), false);
    fail.insert(QStringLiteral("code"), QStringLiteral("zci-chat-unavailable"));
    fail.insert(QStringLiteral("data"), QVariantMap{});

    QDBusConnection conn = m_connectionName.isEmpty()
        ? QDBusConnection::sessionBus()
        : QDBusConnection(m_connectionName);
    if (!conn.isConnected()) {
        fail.insert(QStringLiteral("message"), QStringLiteral("ZCI Chatbot D-Bus oturumu bağlı değil."));
        return fail;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kBusName),
        QString::fromLatin1(kObjectPath),
        QString::fromLatin1(kInterface),
        method);
    call.setArguments(args);

    const QDBusMessage reply = conn.call(call, QDBus::Block, 30000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fail.insert(QStringLiteral("message"),
                    QStringLiteral("ZCI Chatbot servisi erişilemiyor: %1").arg(reply.errorMessage()));
        return fail;
    }
    if (reply.arguments().isEmpty()) {
        fail.insert(QStringLiteral("message"), QStringLiteral("ZCI Chatbot boş yanıt döndürdü."));
        return fail;
    }
    const QVariant unwrapped = unwrapDbusVariant(reply.arguments().first());
    if (unwrapped.typeId() != QMetaType::QVariantMap) {
        fail.insert(QStringLiteral("message"), QStringLiteral("ZCI Chatbot yanıt zarfı geçersiz."));
        return fail;
    }
    return unwrapped.toMap();
}

void ChatClient::setIncludeSystemContext(bool enabled)
{
    if (m_includeSystemContext != enabled) {
        m_includeSystemContext = enabled;
        emit settingsChanged();
    }
}

void ChatClient::setBusy(bool b)
{
    if (m_busy != b) {
        m_busy = b;
        emit busyChanged();
    }
}

void ChatClient::setError(const QString &err)
{
    if (m_errorMessage != err) {
        m_errorMessage = err;
        emit errorChanged();
    }
}

QVariantMap ChatClient::callGetChatStatus()
{
    return invokeEnvelope(QStringLiteral("GetChatStatus"));
}

QVariantMap ChatClient::callCreateSession(const QString &title, bool includeSystemContext)
{
    return invokeEnvelope(QStringLiteral("CreateSession"), {title, includeSystemContext});
}

QVariantMap ChatClient::callSendMessage(const QString &sessionId,
                                        const QString &userMessage,
                                        bool includeSystemContext,
                                        quint32 maxTokens)
{
    return invokeEnvelope(QStringLiteral("SendMessage"),
                          {sessionId, userMessage, includeSystemContext, maxTokens});
}

QVariantMap ChatClient::callGetSessionHistory(const QString &sessionId)
{
    return invokeEnvelope(QStringLiteral("GetSessionHistory"), {sessionId});
}

QVariantMap ChatClient::callListSessions()
{
    return invokeEnvelope(QStringLiteral("ListSessions"));
}

QVariantMap ChatClient::callClearSession(const QString &sessionId)
{
    return invokeEnvelope(QStringLiteral("ClearSession"), {sessionId});
}

bool ChatClient::refreshStatus()
{
    const QVariantMap env = callGetChatStatus();
    if (!env.value(QStringLiteral("success")).toBool()) {
        m_connected = false;
        m_runtimeState = QStringLiteral("Unavailable");
        m_statusBanner = env.value(QStringLiteral("message")).toString();
        setError(m_statusBanner);
        emit statusChanged();
        return false;
    }

    const QVariantMap d = env.value(QStringLiteral("data")).toMap();
    m_connected = true;
    m_runtimeState = d.value(QStringLiteral("runtime_state"), QStringLiteral("Standby")).toString();
    m_vramLocked = d.value(QStringLiteral("vram_locked"), false).toBool();
    m_activeVramMb = d.value(QStringLiteral("active_vram_mb"), 0).toULongLong();
    m_hardwareProfile =
        d.value(QStringLiteral("hardware_profile"), QStringLiteral("dev-baseline")).toString();
    m_activeModelId = d.value(QStringLiteral("active_model_id")).toString();
    m_loopbackOnly = d.value(QStringLiteral("loopback_only"), true).toBool();
    m_externalTelemetryDisabled =
        !d.value(QStringLiteral("external_telemetry_enabled"), false).toBool();
    m_commandExecutionDisabled =
        !d.value(QStringLiteral("command_execution_enabled"), false).toBool();
    m_storageEncrypted = d.value(QStringLiteral("storage_encrypted"), true).toBool();
    m_storagePermissionsStrict =
        d.value(QStringLiteral("storage_permissions_strict"), true).toBool();
    m_maxContextTokens = d.value(QStringLiteral("max_context_tokens"), 8192).toInt();
    m_maxOutputTokens = d.value(QStringLiteral("max_output_tokens"), 768).toInt();
    m_blockedCommandAttempts = d.value(QStringLiteral("blocked_command_attempts"), 0).toULongLong();
    m_blockedInjectionAttempts =
        d.value(QStringLiteral("blocked_injection_attempts"), 0).toULongLong();
    m_statusBanner = env.value(QStringLiteral("message")).toString();
    setError(QString());
    emit statusChanged();
    return true;
}

bool ChatClient::refreshSessions()
{
    const QVariantMap env = callListSessions();
    if (!env.value(QStringLiteral("success")).toBool()) {
        return false;
    }
    const QVariantMap d = env.value(QStringLiteral("data")).toMap();
    const QStringList rawList = d.value(QStringLiteral("sessions_json")).toStringList();
    QVariantList parsed;
    for (const QString &line : rawList) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (doc.isObject()) {
            parsed.append(doc.object().toVariantMap());
        }
    }
    m_sessions = parsed;
    if (m_activeSessionId.isEmpty() && !m_sessions.isEmpty()) {
        m_activeSessionId = m_sessions.last().toMap().value(QStringLiteral("session_id")).toString();
    }
    emit sessionsChanged();

    if (!m_activeSessionId.isEmpty()) {
        selectSession(m_activeSessionId);
    }
    return true;
}

QString ChatClient::createSession(const QString &title)
{
    const QVariantMap env = callCreateSession(title, m_includeSystemContext);
    if (!env.value(QStringLiteral("success")).toBool()) {
        setError(env.value(QStringLiteral("message")).toString());
        return {};
    }
    const QString sid =
        env.value(QStringLiteral("data")).toMap().value(QStringLiteral("session_id")).toString();
    m_activeSessionId = sid;
    setError(QString());
    refreshSessions();
    return sid;
}

bool ChatClient::selectSession(const QString &sessionId)
{
    if (sessionId.trimmed().isEmpty()) {
        return false;
    }
    const QVariantMap env = callGetSessionHistory(sessionId.trimmed());
    if (!env.value(QStringLiteral("success")).toBool()) {
        setError(env.value(QStringLiteral("message")).toString());
        return false;
    }
    const QVariantMap d = env.value(QStringLiteral("data")).toMap();
    const QStringList rawMsgs = d.value(QStringLiteral("messages_json")).toStringList();
    QVariantList parsed;
    for (const QString &line : rawMsgs) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (doc.isObject()) {
            parsed.append(doc.object().toVariantMap());
        }
    }
    m_activeSessionId = sessionId.trimmed();
    m_messages = parsed;
    emit sessionsChanged();
    emit messagesChanged();
    return true;
}

bool ChatClient::sendMessage(const QString &userMessage, int maxTokens)
{
    const QString cleaned = userMessage.trimmed();
    if (cleaned.isEmpty()) {
        return false;
    }
    if (m_activeSessionId.isEmpty()) {
        if (createSession().isEmpty()) {
            return false;
        }
    }

    setBusy(true);
    setError(QString());
    const quint32 tok = static_cast<quint32>(qBound(1, maxTokens, 4096));
    const QVariantMap env =
        callSendMessage(m_activeSessionId, cleaned, m_includeSystemContext, tok);
    setBusy(false);

    if (!env.value(QStringLiteral("success")).toBool()) {
        setError(env.value(QStringLiteral("message")).toString());
        refreshStatus();
        return false;
    }

    if (env.value(QStringLiteral("code")).toString() == QLatin1String("passive-standby")) {
        m_statusBanner = env.value(QStringLiteral("message")).toString();
        emit statusChanged();
    }

    selectSession(m_activeSessionId);
    refreshStatus();
    return true;
}

bool ChatClient::clearActiveSession()
{
    if (m_activeSessionId.isEmpty()) {
        return false;
    }
    const QVariantMap env = callClearSession(m_activeSessionId);
    if (!env.value(QStringLiteral("success")).toBool()) {
        setError(env.value(QStringLiteral("message")).toString());
        return false;
    }
    m_messages.clear();
    setError(QString());
    emit messagesChanged();
    refreshSessions();
    refreshStatus();
    return true;
}

bool ChatClient::clearAllHistory()
{
    const QVariantMap env = callClearSession(QStringLiteral("*"));
    if (!env.value(QStringLiteral("success")).toBool()) {
        setError(env.value(QStringLiteral("message")).toString());
        return false;
    }
    m_activeSessionId.clear();
    m_sessions.clear();
    m_messages.clear();
    setError(QString());
    emit sessionsChanged();
    emit messagesChanged();
    refreshStatus();
    return true;
}

void ChatClient::onDbusMessageReceived(const QString &sessionId,
                                       const QString & /*messageId*/,
                                       const QString & /*role*/,
                                       const QString & /*contentPreview*/,
                                       bool /*executable*/,
                                       const QString & /*policyAuthority*/)
{
    if (sessionId == m_activeSessionId) {
        selectSession(m_activeSessionId);
    }
}

void ChatClient::onDbusSessionCleared(const QString &sessionId, const QString & /*reason*/)
{
    if (sessionId == QLatin1String("*") || sessionId == m_activeSessionId) {
        m_messages.clear();
        emit messagesChanged();
    }
    refreshSessions();
}

void ChatClient::applyMockStateForTest(const QString &runtimeState,
                                       const QString &hardwareProfile,
                                       const QString &activeModelId,
                                       const QVariantList &messages)
{
    m_connected = true;
    m_runtimeState = runtimeState;
    m_vramLocked = (runtimeState == QLatin1String("Active"));
    m_activeVramMb = m_vramLocked ? 1536 : 0;
    m_hardwareProfile = hardwareProfile;
    m_activeModelId = activeModelId;
    m_messages = messages;
    m_statusBanner = QStringLiteral("ZCI Chatbot hazır (test modu).");
    emit statusChanged();
    emit messagesChanged();
}

} // namespace zethropol::chat
