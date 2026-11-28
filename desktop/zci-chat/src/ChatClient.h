#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QDBusPendingCallWatcher;

namespace zethropol::chat {

/// Fail-closed Qt6::DBus client for `org.zethropol.ZciChat.v1` (`/org/zethropol/ZciChat`).
/// - Zero external network sockets (`Qt6::Network` is not linked).
/// - Zero shell subprocesses (`QProcess` is not used).
/// - Non-blocking asynchronous `SendMessage` (`QDBusPendingCallWatcher`) for desktop GUI smoothness.
class ChatClient : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool isSending READ isSending NOTIFY busyChanged)
    Q_PROPERTY(QString sendingStatusText READ sendingStatusText NOTIFY busyChanged)
    Q_PROPERTY(QString runtimeState READ runtimeState NOTIFY statusChanged)
    Q_PROPERTY(bool vramLocked READ vramLocked NOTIFY statusChanged)
    Q_PROPERTY(qulonglong activeVramMb READ activeVramMb NOTIFY statusChanged)
    Q_PROPERTY(QString hardwareProfile READ hardwareProfile NOTIFY statusChanged)
    Q_PROPERTY(QString activeModelId READ activeModelId NOTIFY statusChanged)
    Q_PROPERTY(bool loopbackOnly READ loopbackOnly NOTIFY statusChanged)
    Q_PROPERTY(bool externalTelemetryDisabled READ externalTelemetryDisabled NOTIFY statusChanged)
    Q_PROPERTY(bool commandExecutionDisabled READ commandExecutionDisabled NOTIFY statusChanged)
    Q_PROPERTY(bool storageEncrypted READ storageEncrypted NOTIFY statusChanged)
    Q_PROPERTY(bool storagePermissionsStrict READ storagePermissionsStrict NOTIFY statusChanged)
    Q_PROPERTY(int maxContextTokens READ maxContextTokens NOTIFY statusChanged)
    Q_PROPERTY(int maxOutputTokens READ maxOutputTokens NOTIFY statusChanged)
    Q_PROPERTY(qulonglong blockedCommandAttempts READ blockedCommandAttempts NOTIFY statusChanged)
    Q_PROPERTY(qulonglong blockedInjectionAttempts READ blockedInjectionAttempts NOTIFY statusChanged)
    Q_PROPERTY(QString statusBanner READ statusBanner NOTIFY statusChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)

    Q_PROPERTY(bool includeSystemContext READ includeSystemContext WRITE setIncludeSystemContext NOTIFY settingsChanged)
    Q_PROPERTY(QString activeSessionId READ activeSessionId NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)

public:
    static constexpr const char *kBusName = "org.zethropol.ZciChat";
    static constexpr const char *kObjectPath = "/org/zethropol/ZciChat";
    static constexpr const char *kInterface = "org.zethropol.ZciChat.v1";
    static constexpr int kSendMessageTimeoutMs = 125000;

    explicit ChatClient(QObject *parent = nullptr);
    ~ChatClient() override;

    bool connectToBus(const QString &customBusAddress = QString());

    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    bool isSending() const { return m_busy; }
    QString sendingStatusText() const { return m_sendingStatusText; }
    QString runtimeState() const { return m_runtimeState; }
    bool vramLocked() const { return m_vramLocked; }
    qulonglong activeVramMb() const { return m_activeVramMb; }
    QString hardwareProfile() const { return m_hardwareProfile; }
    QString activeModelId() const { return m_activeModelId; }
    bool loopbackOnly() const { return m_loopbackOnly; }
    bool externalTelemetryDisabled() const { return m_externalTelemetryDisabled; }
    bool commandExecutionDisabled() const { return m_commandExecutionDisabled; }
    bool storageEncrypted() const { return m_storageEncrypted; }
    bool storagePermissionsStrict() const { return m_storagePermissionsStrict; }
    int maxContextTokens() const { return m_maxContextTokens; }
    int maxOutputTokens() const { return m_maxOutputTokens; }
    qulonglong blockedCommandAttempts() const { return m_blockedCommandAttempts; }
    qulonglong blockedInjectionAttempts() const { return m_blockedInjectionAttempts; }
    QString statusBanner() const { return m_statusBanner; }
    QString errorMessage() const { return m_errorMessage; }

    bool includeSystemContext() const { return m_includeSystemContext; }
    void setIncludeSystemContext(bool enabled);

    QString activeSessionId() const { return m_activeSessionId; }
    QVariantList sessions() const { return m_sessions; }
    QVariantList messages() const { return m_messages; }

    Q_INVOKABLE bool refreshStatus(bool clearExistingError = false);
    Q_INVOKABLE bool refreshSessions(bool loadActiveHistory = true);
    Q_INVOKABLE QString createSession(const QString &title = QStringLiteral("Yeni ZCI Sohbeti"));
    Q_INVOKABLE bool selectSession(const QString &sessionId);
    Q_INVOKABLE bool sendMessage(const QString &userMessage, int maxTokens = 2048);
    Q_INVOKABLE bool clearActiveSession();
    Q_INVOKABLE bool clearAllHistory();
    Q_INVOKABLE void clearError();

    /// Structured synchronous D-Bus helpers for CLI mode (`--status`, `--send`, `--history`, `--clear`).
    QVariantMap callGetChatStatus();
    QVariantMap callCreateSession(const QString &title, bool includeSystemContext);
    QVariantMap callSendMessage(const QString &sessionId,
                                const QString &userMessage,
                                bool includeSystemContext,
                                quint32 maxTokens);
    QVariantMap callGetSessionHistory(const QString &sessionId);
    QVariantMap callListSessions();
    QVariantMap callClearSession(const QString &sessionId);

    /// Deterministic test-only state injectors (never exposed to QML).
    void applyMockStateForTest(const QString &runtimeState,
                               const QString &hardwareProfile,
                               const QString &activeModelId,
                               const QVariantList &messages,
                               const QVariantList &sessions = {},
                               const QString &activeSessionId = QString());
    void simulateSendingStateForTest(bool sending, const QString &errorBanner = QString());

signals:
    void statusChanged();
    void busyChanged();
    void errorChanged();
    void settingsChanged();
    void sessionsChanged();
    void messagesChanged();

private slots:
    void onDbusMessageReceived(const QString &sessionId,
                               const QString &messageId,
                               const QString &role,
                               const QString &contentPreview,
                               bool executable,
                               const QString &policyAuthority);
    void onDbusSessionCleared(const QString &sessionId, const QString &reason);

private:
    static QVariant unwrapDbusVariant(const QVariant &value);
    static QVariantMap decodeDbusReplyEnvelope(const QDBusMessage &reply);
    QDBusConnection activeBusConnection() const;
    QVariantMap invokeEnvelope(const QString &method, const QVariantList &args = {}, int timeoutMs = 15000);
    void handleAsyncSendReply(const QString &sessionId,
                              const QString &sentUserMessage,
                              const QDBusMessage &reply);
    void appendNoticeMessage(const QString &title, const QString &text, bool isError);
    void syncSessionMessageCountLocal(const QString &sessionId, int count);
    void setBusy(bool b, const QString &statusText = QString());
    void setError(const QString &err);

    QString m_connectionName;
    bool m_busConnected = false;

    bool m_connected = false;
    bool m_busy = false;
    QString m_sendingStatusText;
    QString m_runtimeState = QStringLiteral("Standby");
    bool m_vramLocked = false;
    qulonglong m_activeVramMb = 0;
    QString m_hardwareProfile = QStringLiteral("dev-baseline");
    QString m_activeModelId;
    bool m_loopbackOnly = true;
    bool m_externalTelemetryDisabled = true;
    bool m_commandExecutionDisabled = true;
    bool m_storageEncrypted = true;
    bool m_storagePermissionsStrict = true;
    int m_maxContextTokens = 8192;
    int m_maxOutputTokens = 2048;
    qulonglong m_blockedCommandAttempts = 0;
    qulonglong m_blockedInjectionAttempts = 0;
    QString m_statusBanner = QStringLiteral("ZCI Chatbot yerel servise bağlanıyor...");
    QString m_errorMessage;

    bool m_includeSystemContext = false;
    QString m_activeSessionId;
    QVariantList m_sessions;
    QVariantList m_messages;
};

} // namespace zethropol::chat
