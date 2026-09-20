#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

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
    void readOutput();
    void readMonitorOutput();

private:
    QProcess process;
    QProcess monitorProcess;
    QTimer monitorTimer;
    QVariantMap m_state;
    QVariantMap m_performance;
};
