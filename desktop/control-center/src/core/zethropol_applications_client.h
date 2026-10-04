#pragma once
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDBusInterface>

class ZethropolApplicationsClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList applicationsList READ applicationsList NOTIFY applicationsChanged)
public:
    explicit ZethropolApplicationsClient(QObject *parent = nullptr);
    QVariantList applicationsList() const { return m_applicationsList; }
public slots:
    void refreshApplications();
    void uninstallApplication(const QString &appId);
    void checkOperationStatus(const QString &operationId);
signals:
    void applicationsChanged();
    void operationInitiated(const QString &operationId);
    void operationStatusReceived(const QString &operationId, const QString &status, const QString &message);
private slots:
    void handleAppsSignal(const QVariantList &apps);
private:
    QDBusInterface *m_interface;
    QVariantList m_applicationsList;
};