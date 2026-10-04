#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDBusInterface>

class ZethropolUpdateClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap updateState READ updateState NOTIFY updateStateChanged)
    Q_PROPERTY(QVariantList pendingUpdates READ pendingUpdates NOTIFY pendingUpdatesChanged)
public:
    explicit ZethropolUpdateClient(QObject *parent = nullptr);
    QVariantMap updateState() const { return m_updateState; }
    QVariantList pendingUpdates() const { return m_pendingUpdates; }
public slots:
    void refreshUpdateState();
    void triggerSystemUpdate();
    void checkOperationStatus(const QString &operationId);
signals:
    void updateStateChanged();
    void pendingUpdatesChanged();
    void operationInitiated(const QString &operationId);
    void operationStatusReceived(const QString &operationId, const QString &status, const QString &message);
    void errorOccurred(const QString &message);
private slots:
    void handleUpdateSignal(const QVariantMap &newState);
private:
    QDBusInterface *m_interface;
    QVariantMap m_updateState;
    QVariantList m_pendingUpdates;
};