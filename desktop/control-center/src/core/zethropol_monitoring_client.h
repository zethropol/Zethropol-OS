#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QDBusInterface>

class ZethropolMonitoringClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit ZethropolMonitoringClient(QObject *parent = nullptr);
    QVariantMap state() const;

signals:
    void stateChanged();

private slots:
    void requestState();
    void readState();

private:
    QDBusInterface monitoringInterface;
    QTimer refreshTimer;
    QVariantMap m_state;
};
