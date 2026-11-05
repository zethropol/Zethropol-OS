#pragma once
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <QVariantMap>
#include <memory>

// Presentation acknowledgements are local; transaction/reboot facts always
// come from the authenticated service and are rechecked before a reboot call.
class PostUpdateFlow : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString details READ details NOTIFY changed)
    Q_PROPERTY(QString operationId READ operationId NOTIFY changed)
    Q_PROPERTY(QString rebootReason READ rebootReason NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canReviewReboot READ canReviewReboot NOTIFY changed)
public:
    explicit PostUpdateFlow(QObject *parent = nullptr, const QString &settingsFile = {});
    QString phase() const { return m_phase; }
    QString title() const { return m_title; }
    QString details() const { return m_details; }
    QString operationId() const { return m_id; }
    QString rebootReason() const { return m_state.value("reboot_reason").toString(); }
    QString statusText() const { return m_status; }
    bool busy() const { return m_loading || m_rebooting; }
    bool canReviewReboot() const;
    Q_INVOKABLE void setUpdateBusy(bool value);
    Q_INVOKABLE void start();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void acknowledgeCleanup();
    Q_INVOKABLE void deferReboot();
    Q_INVOKABLE void reviewReboot();
    Q_INVOKABLE void rebootNow();
signals:
    void changed();
    void operationError(const QString &title, const QString &details);
protected:
    virtual void fetchState(const QString &id, bool forReboot);
    virtual void callPowerReboot();
    quint64 requestGeneration() const { return m_generation; }
    void deliverState(const QVariantMap &state, bool forReboot = false, quint64 generation = 0);
    void deliverReadError(const QString &name, const QString &message, bool forReboot, quint64 generation = 0);
    void deliverRebootReply(bool success, const QString &code, const QString &message);
private:
    void clearPresentation();
    QString key(const QString &name) const;
    bool persist(const QString &name, bool value);
    bool terminalCommit(const QVariantMap &state) const;
    void present();
    void fail(const QString &message);
    std::unique_ptr<QSettings> m_settings;
    QTimer m_poll;
    QVariantMap m_state;
    quint64 m_generation = 0;
    QString m_phase = "idle", m_title, m_details, m_id, m_status;
    bool m_loading = false, m_rebooting = false, m_updateBusy = false, m_settingsError = false;
};
