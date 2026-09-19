#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantMap>

class HardwareServiceClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit HardwareServiceClient(QObject *parent = nullptr);

    QVariantMap state() const;

signals:
    void stateChanged();

private slots:
    void readOutput();

private:
    QProcess process;
    QVariantMap m_state;
};
