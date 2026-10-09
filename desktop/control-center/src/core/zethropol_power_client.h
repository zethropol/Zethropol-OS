#pragma once

#include <QObject>
#include <QVariantMap>
#include <QDBusInterface>

class ZethropolPowerClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY powerStateChanged)

public:
    explicit ZethropolPowerClient(QObject *parent = nullptr);
    ~ZethropolPowerClient() override;

    QVariantMap state() const { return m_state; }

public slots:
    void refreshState();
    void setProfile(const QString &profile);
    void setGovernor(const QString &governor);
    void suspend();
    void hibernate();
    void reboot();
    void shutdown();

signals:
    void powerStateChanged();
    void operationInitiated(const QString &operationId);
    void errorOccurred(const QString &message);

private slots:
    void handleDbusSignal(const QVariant &state);

private:
    QDBusInterface *m_interface;
    QVariantMap m_state;

    void parseResult(const QVariantMap &result);
    void invokeOperation(const QString &method, const QVariantList &args = {});
};
