#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class SecurityService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString firewall READ firewall NOTIFY stateChanged)
    Q_PROPERTY(QString secureBoot READ secureBoot NOTIFY stateChanged)
    Q_PROPERTY(QString kernelLockdown READ kernelLockdown NOTIFY stateChanged)
    Q_PROPERTY(QStringList lsm READ lsm NOTIFY stateChanged)
    Q_PROPERTY(QString appArmor READ appArmor NOTIFY stateChanged)
    Q_PROPERTY(int failedServices READ failedServices NOTIFY stateChanged)

public:
    explicit SecurityService(QObject *parent = nullptr);

    QString firewall() const;
    QString secureBoot() const;
    QString kernelLockdown() const;
    QStringList lsm() const;
    QString appArmor() const;
    int failedServices() const;

public slots:
    void refresh();

signals:
    void stateChanged();

private:
    QString m_firewall;
    QString m_secureBoot;
    QString m_kernelLockdown;
    QStringList m_lsm;
    QString m_appArmor;
    int m_failedServices = 0;
};
