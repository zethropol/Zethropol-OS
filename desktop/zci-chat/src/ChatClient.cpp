#include "ChatClient.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
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

QDBusConnection ChatClient::activeBusConnection() const
{
    return m_connectionName.isEmpty()
        ? QDBusConnection::sessionBus()
        : QDBusConnection(m_connectionName);
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

QVariantMap ChatClient::decodeDbusReplyEnvelope(const QDBusMessage &reply)
{
    QVariantMap fail;
    fail.insert(QStringLiteral("success"), false);
    fail.insert(QStringLiteral("code"), QStringLiteral("zci-chat-unavailable"));
    fail.insert(QStringLiteral("data"), QVariantMap{});

    if (reply.type() == QDBusMessage::ErrorMessage) {
        const QString errName = reply.errorName();
        if (errName == QLatin1String("org.freedesktop.DBus.Error.NoReply")
            || errName == QLatin1String("org.freedesktop.DBus.Error.Timeout")) {
            fail.insert(QStringLiteral("code"), QStringLiteral("dbus-timeout"));
            fail.insert(QStringLiteral("message"),
                        QStringLiteral("Yerel model yanıtı zaman aşımına uğradı (125 sn). İsteği daha kısa bir soruyla tekrar deneyebilirsiniz."));
            return fail;
        }
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

QVariantMap ChatClient::invokeEnvelope(const QString &method, const QVariantList &args, int timeoutMs)
{
    QDBusConnection conn = activeBusConnection();
    if (!conn.isConnected()) {
        return {
            {QStringLiteral("success"), false},
            {QStringLiteral("code"), QStringLiteral("zci-chat-unavailable")},
            {QStringLiteral("message"), QStringLiteral("ZCI Chatbot D-Bus oturumu bağlı değil.")},
            {QStringLiteral("data"), QVariantMap{}},
        };
    }

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kBusName),
        QString::fromLatin1(kObjectPath),
        QString::fromLatin1(kInterface),
        method);
    call.setArguments(args);

    const QDBusMessage reply = conn.call(call, QDBus::Block, timeoutMs);
    return decodeDbusReplyEnvelope(reply);
}

void ChatClient::setIncludeSystemContext(bool enabled)
{
    if (m_includeSystemContext != enabled) {
        m_includeSystemContext = enabled;
        emit settingsChanged();
    }
}

void ChatClient::setBusy(bool b, const QString &statusText)
{
    const QString nextText = b
        ? (statusText.isEmpty() ? QStringLiteral("Yerel model yanıt hazırlıyor...") : statusText)
        : QString();
    if (m_busy != b || m_sendingStatusText != nextText) {
        m_busy = b;
        m_sendingStatusText = nextText;
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

void ChatClient::clearError()
{
    setError(QString());
}

void ChatClient::syncSessionMessageCountLocal(const QString &sessionId, int count)
{
    if (sessionId.isEmpty() || count < 0) {
        return;
    }
    bool changed = false;
    for (int i = 0; i < m_sessions.size(); ++i) {
        QVariantMap item = m_sessions[i].toMap();
        if (item.value(QStringLiteral("session_id")).toString() == sessionId) {
            item.insert(QStringLiteral("message_count"), count);
            item.insert(QStringLiteral("messageCount"), count);
            m_sessions[i] = item;
            changed = true;
            break;
        }
    }
    if (changed) {
        emit sessionsChanged();
    }
}

void ChatClient::appendNoticeMessage(const QString &title, const QString &text, bool isError)
{
    QVariantMap notice;
    notice.insert(QStringLiteral("role"), QStringLiteral("notice"));
    notice.insert(QStringLiteral("title"), title);
    notice.insert(QStringLiteral("content"), text);
    notice.insert(QStringLiteral("is_error"), isError);
    notice.insert(QStringLiteral("executable"), false);
    notice.insert(QStringLiteral("policy_authority"), QStringLiteral("none"));
    m_messages.append(notice);
    emit messagesChanged();
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
                          {sessionId, userMessage, includeSystemContext, maxTokens},
                          kSendMessageTimeoutMs);
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

bool ChatClient::refreshStatus(bool clearExistingError)
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
    m_maxOutputTokens = d.value(QStringLiteral("max_output_tokens"), 2048).toInt();
    m_blockedCommandAttempts = d.value(QStringLiteral("blocked_command_attempts"), 0).toULongLong();
    m_blockedInjectionAttempts =
        d.value(QStringLiteral("blocked_injection_attempts"), 0).toULongLong();
    m_statusBanner = env.value(QStringLiteral("message")).toString();
    if (clearExistingError) {
        setError(QString());
    }
    emit statusChanged();
    return true;
}

bool ChatClient::refreshSessions(bool loadActiveHistory)
{
    const QVariantMap env = callListSessions();
    if (!env.value(QStringLiteral("success")).toBool()) {
        return false;
    }
    const QVariantMap d = env.value(QStringLiteral("data")).toMap();
    const QStringList rawList = d.value(QStringLiteral("sessions_json")).toStringList();
    QVariantList parsed;
    parsed.reserve(rawList.size());
    for (const QString &line : rawList) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (doc.isObject()) {
            QVariantMap obj = doc.object().toVariantMap();
            const int msgCount = obj.contains(QStringLiteral("message_count"))
                ? obj.value(QStringLiteral("message_count")).toInt()
                : obj.value(QStringLiteral("messageCount"), 0).toInt();
            obj.insert(QStringLiteral("message_count"), msgCount);
            obj.insert(QStringLiteral("messageCount"), msgCount);
            parsed.append(obj);
        }
    }
    m_sessions = parsed;
    if (m_activeSessionId.isEmpty() && !m_sessions.isEmpty()) {
        m_activeSessionId = m_sessions.last().toMap().value(QStringLiteral("session_id")).toString();
    }
    emit sessionsChanged();

    if (loadActiveHistory && !m_activeSessionId.isEmpty()) {
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
    refreshSessions(true);
    return sid;
}

bool ChatClient::selectSession(const QString &sessionId)
{
    const QString cleanId = sessionId.trimmed();
    if (cleanId.isEmpty()) {
        return false;
    }
    const QVariantMap env = callGetSessionHistory(cleanId);
    if (!env.value(QStringLiteral("success")).toBool()) {
        setError(env.value(QStringLiteral("message")).toString());
        return false;
    }
    const QVariantMap d = env.value(QStringLiteral("data")).toMap();
    const QStringList rawMsgs = d.value(QStringLiteral("messages_json")).toStringList();
    QVariantList parsed;
    parsed.reserve(rawMsgs.size());
    for (const QString &line : rawMsgs) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (doc.isObject()) {
            parsed.append(doc.object().toVariantMap());
        }
    }
    const int reportedCount = d.contains(QStringLiteral("message_count"))
        ? d.value(QStringLiteral("message_count")).toInt()
        : (d.contains(QStringLiteral("messageCount"))
               ? d.value(QStringLiteral("messageCount")).toInt()
               : static_cast<int>(parsed.size()));

    m_activeSessionId = cleanId;
    m_messages = parsed;
    syncSessionMessageCountLocal(cleanId, reportedCount);
    emit sessionsChanged();
    emit messagesChanged();
    return true;
}

bool ChatClient::sendMessage(const QString &userMessage, int maxTokens)
{
    if (m_busy) {
        return false;
    }
    const QString cleaned = userMessage.trimmed();
    if (cleaned.isEmpty()) {
        return false;
    }
    if (m_activeSessionId.isEmpty()) {
        if (createSession().isEmpty()) {
            const QString msg = m_errorMessage.isEmpty()
                ? QStringLiteral("Yerel sohbet oturumu açılamadı; ileti gönderilemedi.")
                : m_errorMessage;
            setError(msg);
            appendNoticeMessage(QStringLiteral("Oturum Hatası"), msg, true);
            return false;
        }
    }

    QDBusConnection conn = activeBusConnection();
    if (!conn.isConnected()) {
        const QString msg = QStringLiteral("ZCI Chatbot D-Bus oturumu bağlı değil; mesajınız korunarak gönderim iptal edildi.");
        setError(msg);
        QVariantMap preserved;
        preserved.insert(QStringLiteral("role"), QStringLiteral("user"));
        preserved.insert(QStringLiteral("content"), cleaned);
        preserved.insert(QStringLiteral("executable"), false);
        preserved.insert(QStringLiteral("policy_authority"), QStringLiteral("none"));
        m_messages.append(preserved);
        emit messagesChanged();
        appendNoticeMessage(QStringLiteral("Bağlantı Hatası"), msg, true);
        return false;
    }

    // Show user message immediately & activate non-blocking sending indicator
    QVariantMap optimisticUser;
    optimisticUser.insert(QStringLiteral("role"), QStringLiteral("user"));
    optimisticUser.insert(QStringLiteral("content"), cleaned);
    optimisticUser.insert(QStringLiteral("pending"), true);
    optimisticUser.insert(QStringLiteral("executable"), false);
    optimisticUser.insert(QStringLiteral("policy_authority"), QStringLiteral("none"));
    m_messages.append(optimisticUser);
    emit messagesChanged();

    setError(QString());
    setBusy(true, QStringLiteral("Yerel model yanıt hazırlıyor..."));

    const QString targetSessionId = m_activeSessionId;
    const quint32 tok = static_cast<quint32>(qBound(1, maxTokens, 4096));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QString::fromLatin1(kBusName),
        QString::fromLatin1(kObjectPath),
        QString::fromLatin1(kInterface),
        QStringLiteral("SendMessage"));
    call.setArguments({targetSessionId, cleaned, m_includeSystemContext, tok});

    // Fully asynchronous non-blocking D-Bus call (125s timeout) — NEVER blocks the Qt GUI thread!
    QDBusPendingCall pending = conn.asyncCall(call, kSendMessageTimeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, targetSessionId, cleaned](QDBusPendingCallWatcher *w) {
                const QDBusMessage reply = w->reply();
                w->deleteLater();
                handleAsyncSendReply(targetSessionId, cleaned, reply);
            });
    return true;
}

void ChatClient::handleAsyncSendReply(const QString &sessionId,
                                      const QString &sentUserMessage,
                                      const QDBusMessage &reply)
{
    setBusy(false);

    const QVariantMap env = decodeDbusReplyEnvelope(reply);
    const bool ok = env.value(QStringLiteral("success")).toBool();
    const QString code = env.value(QStringLiteral("code")).toString();
    const QString message = env.value(QStringLiteral("message")).toString();

    if (!ok) {
        const QString visibleErr = message.isEmpty()
            ? QStringLiteral("İleti gönderilemedi (%1).").arg(code)
            : message;
        setError(visibleErr);

        // Preserve user's message visibly and attach clear Turkish notice in stream (NEVER silent!)
        QString title = QStringLiteral("İleti Gönderilemedi");
        if (code == QLatin1String("command-execution-denied-stage13")
            || code == QLatin1String("prompt-injection-blocked")
            || code == QLatin1String("untrusted-injection-blocked")) {
            title = QStringLiteral("Güvenlik Politikası Engeli");
        } else if (code == QLatin1String("dbus-timeout") || code == QLatin1String("inference-timeout")) {
            title = QStringLiteral("Zaman Aşımı Uyarısı");
        }

        // Ensure user's sent text is still in m_messages even if session changed
        bool hasSent = false;
        if (!m_messages.isEmpty()) {
            const QVariantMap last = m_messages.last().toMap();
            if (last.value(QStringLiteral("role")).toString() == QLatin1String("user")
                && last.value(QStringLiteral("content")).toString() == sentUserMessage) {
                hasSent = true;
            }
        }
        if (!hasSent) {
            QVariantMap preserved;
            preserved.insert(QStringLiteral("role"), QStringLiteral("user"));
            preserved.insert(QStringLiteral("content"), sentUserMessage);
            preserved.insert(QStringLiteral("executable"), false);
            preserved.insert(QStringLiteral("policy_authority"), QStringLiteral("none"));
            m_messages.append(preserved);
            emit messagesChanged();
        }
        appendNoticeMessage(title, visibleErr, true);
        // Refresh counters & telemetry WITHOUT clearing our error banner!
        refreshStatus(false);
        refreshSessions(false);
        return;
    }

    if (code == QLatin1String("passive-standby")) {
        const QString standbyNotice = message.isEmpty()
            ? QStringLiteral("Yerel AI motoru pasif beklemede; model yanıtı şu an üretilemedi.")
            : message;
        m_statusBanner = standbyNotice;
        setError(standbyNotice);
        appendNoticeMessage(QStringLiteral("Pasif Bekleme (Standby)"), standbyNotice, true);
        emit statusChanged();
        refreshStatus(false);
        refreshSessions(false);
        return;
    }

    // Successful reply: sync canonical history, sidebar counts, and runtime status!
    setError(QString());
    if (sessionId == m_activeSessionId) {
        selectSession(m_activeSessionId);
    }
    refreshSessions(false);
    refreshStatus(false);
}

bool ChatClient::clearActiveSession()
{
    if (m_activeSessionId.isEmpty() || m_busy) {
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
    refreshSessions(false);
    refreshStatus(false);
    return true;
}

bool ChatClient::clearAllHistory()
{
    if (m_busy) {
        return false;
    }
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
    refreshStatus(false);
    return true;
}

void ChatClient::onDbusMessageReceived(const QString &sessionId,
                                       const QString & /*messageId*/,
                                       const QString & /*role*/,
                                       const QString & /*contentPreview*/,
                                       bool /*executable*/,
                                       const QString & /*policyAuthority*/)
{
    if (!m_busy) {
        if (sessionId == m_activeSessionId) {
            selectSession(m_activeSessionId);
        }
        refreshSessions(false);
    }
}

void ChatClient::onDbusSessionCleared(const QString &sessionId, const QString & /*reason*/)
{
    if (sessionId == QLatin1String("*") || sessionId == m_activeSessionId) {
        m_messages.clear();
        emit messagesChanged();
    }
    refreshSessions(false);
}

void ChatClient::applyMockStateForTest(const QString &runtimeState,
                                       const QString &hardwareProfile,
                                       const QString &activeModelId,
                                       const QVariantList &messages,
                                       const QVariantList &sessions,
                                       const QString &activeSessionId)
{
    m_connected = true;
    m_runtimeState = runtimeState;
    m_vramLocked = (runtimeState == QLatin1String("Active"));
    m_activeVramMb = m_vramLocked ? 1536 : 0;
    m_hardwareProfile = hardwareProfile;
    m_activeModelId = activeModelId;
    m_messages = messages;
    m_sessions = sessions;
    m_activeSessionId = activeSessionId;
    m_statusBanner = QStringLiteral("ZCI Chatbot hazır (test modu).");
    emit statusChanged();
    emit sessionsChanged();
    emit messagesChanged();
}

void ChatClient::simulateSendingStateForTest(bool sending, const QString &errorBanner)
{
    setBusy(sending, sending ? QStringLiteral("Yerel model yanıt hazırlıyor...") : QString());
    if (!errorBanner.isEmpty()) {
        setError(errorBanner);
        appendNoticeMessage(QStringLiteral("Güvenlik Politikası Engeli"), errorBanner, true);
    }
}

} // namespace zethropol::chat
