#pragma once

#include <QObject>
#include <QVariantMap>
#include <QDBusInterface>

class ZethropolSecurityClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap firewall READ firewall NOTIFY securityStateChanged)
    Q_PROPERTY(QVariantMap secureBoot READ secureBoot NOTIFY securityStateChanged)
    Q_PROPERTY(QVariantMap kernel READ kernel NOTIFY securityStateChanged)
    Q_PROPERTY(QVariantMap apparmor READ apparmor NOTIFY securityStateChanged)
    Q_PROPERTY(QVariantMap integrity READ integrity NOTIFY securityStateChanged)

public:
    explicit ZethropolSecurityClient(QObject *parent = nullptr);
    virtual ~ZethropolSecurityClient();

    QVariantMap firewall() const { return m_firewall; }
    QVariantMap secureBoot() const { return m_secureBoot; }
    QVariantMap kernel() const { return m_kernel; }
    QVariantMap apparmor() const { return m_apparmor; }
    QVariantMap integrity() const { return m_integrity; }

public slots:
    void refreshState();
    void toggleFirewall(bool enable);

signals:
    void securityStateChanged();
    void operationInitiated(const QString &operationId);
    void errorOccurred(const QString &message);

private slots:
    void handleDbusSignal(const QVariantMap &state);

private:
    QDBusInterface *m_interface;
    QVariantMap m_firewall;
    QVariantMap m_secureBoot;
    QVariantMap m_kernel;
    QVariantMap m_apparmor;
    QVariantMap m_integrity;

    void parseStateMap(const QVariantMap &stateMap);
};
