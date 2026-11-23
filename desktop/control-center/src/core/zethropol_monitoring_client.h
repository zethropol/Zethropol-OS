#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>

class ZethropolMonitoringClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit ZethropolMonitoringClient(QObject *parent = nullptr);
    QVariantMap state() const;
    bool available() const { return m_available; }
    QString error() const { return m_error; }

signals:
    void stateChanged();

private slots:
    void requestState();
    void readState();

private:
    QTimer refreshTimer;
    bool m_loading = false;
    bool m_available = false;
    QString m_error;
    QVariantMap m_state;
};
