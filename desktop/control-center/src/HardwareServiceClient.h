#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QDBusInterface>

class HardwareServiceClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString lastUpdated READ lastUpdated NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap performance READ performance NOTIFY performanceChanged)

public:
    explicit HardwareServiceClient(QObject *parent = nullptr);

    QVariantMap state() const;
    QVariantMap performance() const;
    bool available() const { return m_available; }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    QString lastUpdated() const { return m_lastUpdated; }
    Q_INVOKABLE void refresh();

signals:
    void stateChanged();
    void performanceChanged();

private slots:
    void readState();
    void readPerformance();
    void requestPerformance();

private:
    QDBusInterface performanceInterface;
    QTimer hardwareTimer;
    QTimer performanceTimer;
    bool m_available = false;
    bool m_loading = false;
    QString m_error;
    QString m_lastUpdated;
    QVariantMap m_state;
    QVariantMap m_performance;
};
