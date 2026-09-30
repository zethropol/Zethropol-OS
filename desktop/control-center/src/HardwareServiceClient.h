#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QDBusInterface>

class HardwareServiceClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap performance READ performance NOTIFY performanceChanged)

public:
    explicit HardwareServiceClient(QObject *parent = nullptr);

    QVariantMap state() const;
    QVariantMap performance() const;

signals:
    void stateChanged();
    void performanceChanged();

private slots:
    void readState();
    void readPerformance();
    void requestPerformance();

private:
    QDBusInterface hardwareInterface;
    QDBusInterface performanceInterface;
    QTimer performanceTimer;
    QVariantMap m_state;
    QVariantMap m_performance;
};
