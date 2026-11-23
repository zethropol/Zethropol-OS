#pragma once

#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class HardwareServiceClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(QString lastUpdated READ lastUpdated NOTIFY stateChanged)

public:
    explicit HardwareServiceClient(QObject *parent = nullptr);

    static QVariantMap normalizeSystemInfo(
        QVariantMap system,
        const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment(),
        const QString &osReleasePath = QStringLiteral("/etc/os-release"),
        const QStringList &sessionDesktopCandidates = {});

    QVariantMap state() const;
    bool available() const { return m_available; }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    QString lastUpdated() const { return m_lastUpdated; }
    Q_INVOKABLE void refresh();

signals:
    void stateChanged();

private slots:
    void readState();

private:
    QTimer hardwareTimer;
    bool m_available = false;
    bool m_loading = false;
    QString m_error;
    QString m_lastUpdated;
    QVariantMap m_state;
};
