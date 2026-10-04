#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QDBusInterface>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QMetaType>

struct ServiceSignalInfo
{
    QString name;
    QString description;
    QString loadState;
    QString activeState;
    QString subState;
    bool enabled = false;
};

Q_DECLARE_METATYPE(ServiceSignalInfo)

QDBusArgument &operator<<(QDBusArgument &argument, const ServiceSignalInfo &info);
const QDBusArgument &operator>>(const QDBusArgument &argument, ServiceSignalInfo &info);

class ZethropolServicesClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList servicesList READ servicesList NOTIFY servicesChanged)

public:
    explicit ZethropolServicesClient(QObject *parent = nullptr);
    virtual ~ZethropolServicesClient();

    QVariantList servicesList() const { return m_servicesList; }

public slots:
    void refreshServices();
    void manageService(const QString &serviceName, const QString &action);
    void checkOperationStatus(const QString &operationId);

signals:
    void servicesChanged();
    void operationInitiated(const QString &operationId);
    void operationStatusReceived(const QString &operationId, const QString &status, const QString &message);
    void errorOccurred(const QString &message);

private slots:
    void handleServiceSignal(const ServiceSignalInfo &serviceInfo);

private:
    QDBusInterface *m_interface;
    QVariantList m_servicesList;
};
