#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDBusInterface>

class ZethropolRecoveryClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList recoveryPoints READ recoveryPoints NOTIFY recoveryPointsChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorOccurred)
public:
    explicit ZethropolRecoveryClient(QObject *parent = nullptr);
    QVariantList recoveryPoints() const { return m_recoveryPoints; }
    QVariantMap state() const { return m_state; }
    QString lastError() const { return m_lastError; }
public slots:
    void refreshState();
    void refreshRecoveryPoints();
    void createRecoveryPoint(const QString &name, const QString &description);
    void rollbackToPoint(uint pointId);
    void checkOperationStatus(const QString &operationId);
signals:
    void stateChanged();
    void recoveryPointsChanged();
    void errorOccurred(const QString &error);
    void operationInitiated(const QString &operationId);
    void operationStatusReceived(const QString &operationId, const QString &status, const QString &message);
private slots:
    void handleRecoverySignal(const QVariantList &points);
private:
    QDBusInterface *m_interface;
    QVariantList m_recoveryPoints;
    QVariantMap m_state;
    QString m_lastError;
};
