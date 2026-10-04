#include "zethropol_services_client.h"

#include <QDBusMessage>
#include <QDBusReply>
#include <QDebug>

QDBusArgument &operator<<(QDBusArgument &argument, const ServiceSignalInfo &info)
{
    argument.beginStructure();
    argument << info.name
             << info.description
             << info.loadState
             << info.activeState
             << info.subState
             << info.enabled;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ServiceSignalInfo &info)
{
    argument.beginStructure();
    argument >> info.name
             >> info.description
             >> info.loadState
             >> info.activeState
             >> info.subState
             >> info.enabled;
    argument.endStructure();
    return argument;
}

namespace {

QVariantMap decodeService(const QDBusArgument &argument)
{
    QVariantMap service;

    argument.beginStructure();

    QString name;
    QString description;
    QString loadState;
    QString activeState;
    QString subState;
    bool enabled = false;

    argument
        >> name
        >> description
        >> loadState
        >> activeState
        >> subState
        >> enabled;

    argument.endStructure();

    service.insert("name", name);
    service.insert("description", description);
    service.insert("load_state", loadState);
    service.insert("active_state", activeState);
    service.insert("sub_state", subState);
    service.insert("enabled", enabled);

    return service;
}

QVariantList decodeServices(const QDBusArgument &argument)
{
    QVariantList services;

    argument.beginArray();

    while (!argument.atEnd()) {
        QVariantMap service = decodeService(argument);
        services.append(service);
    }

    argument.endArray();

    return services;
}

bool decodeOperationResult(
    const QDBusArgument &argument,
    QString &operationId,
    QString &code,
    QString &message)
{
    argument.beginStructure();

    bool success = false;
    QString dataCode;
    QString dataMessage;
    QString dataOperationId;
    QVariantMap data;

    argument
        >> success
        >> dataCode
        >> dataMessage
        >> dataOperationId
        >> data;

    argument.endStructure();

    code = dataCode;
    message = dataMessage;
    operationId = dataOperationId;

    return success;
}

QVariantMap decodeStringMap(const QDBusArgument &argument)
{
    QVariantMap result;

    argument.beginMap();

    while (!argument.atEnd()) {
        argument.beginMapEntry();

        QString key;
        QString value;

        argument >> key >> value;

        result.insert(key, value);

        argument.endMapEntry();
    }

    argument.endMap();

    return result;
}

}

ZethropolServicesClient::ZethropolServicesClient(QObject *parent)
    : QObject(parent),
      m_interface(nullptr)
{
    qDBusRegisterMetaType<ServiceSignalInfo>();

    m_interface = new QDBusInterface(
        "org.zethropol.Services",
        "/org/zethropol/Services",
        "org.zethropol.Services.v1",
        QDBusConnection::systemBus(),
        this
    );

    if (m_interface->isValid()) {
        const bool connected = QDBusConnection::systemBus().connect(
            "org.zethropol.Services",
            "/org/zethropol/Services",
            "org.zethropol.Services.v1",
            "ServiceStateChanged",
            "(sssssb)",
            this,
            SLOT(handleServiceSignal(ServiceSignalInfo))
        );

        if (!connected) {
            qWarning() << "Services service_state_changed sinyali bağlanamadı";
        }

        refreshServices();
    } else {
        qWarning()
            << "Zethropol Services Service bağlantısı kurulamadı:"
            << m_interface->lastError().message();
    }
}

ZethropolServicesClient::~ZethropolServicesClient() = default;

void ZethropolServicesClient::refreshServices()
{
    if (!m_interface || !m_interface->isValid())
        return;

    QDBusMessage reply = m_interface->call("ListServices");

    if (reply.type() != QDBusMessage::ReplyMessage) {
        emit errorOccurred(reply.errorMessage());
        return;
    }

    if (reply.arguments().isEmpty())
        return;

    const QVariant argument = reply.arguments().at(0);

    if (!argument.canConvert<QDBusArgument>()) {
        emit errorOccurred(
            "ListServices dönüşü beklenen D-Bus compound type değil."
        );
        return;
    }

    const QDBusArgument dbusArgument =
        argument.value<QDBusArgument>();

    m_servicesList = decodeServices(dbusArgument);

    emit servicesChanged();
}

void ZethropolServicesClient::manageService(
    const QString &serviceName,
    const QString &action)
{
    if (!m_interface || !m_interface->isValid())
        return;

    QDBusMessage reply =
        m_interface->call("ManageService", serviceName, action);

    if (reply.type() != QDBusMessage::ReplyMessage) {
        emit errorOccurred(reply.errorMessage());
        return;
    }

    if (reply.arguments().isEmpty()) {
        emit errorOccurred("ManageService boş cevap döndürdü.");
        return;
    }

    const QVariant argument = reply.arguments().at(0);

    if (!argument.canConvert<QDBusArgument>()) {
        emit errorOccurred(
            "ManageService dönüşü beklenen D-Bus structure değil."
        );
        return;
    }

    const QDBusArgument dbusArgument =
        argument.value<QDBusArgument>();

    QString operationId;
    QString code;
    QString message;

    const bool success =
        decodeOperationResult(
            dbusArgument,
            operationId,
            code,
            message
        );

    if (!success) {
        emit errorOccurred(
            message.isEmpty()
                ? QStringLiteral("Servis işlemi reddedildi.")
                : message
        );
        return;
    }

    emit operationInitiated(operationId);
}

void ZethropolServicesClient::checkOperationStatus(
    const QString &operationId)
{
    if (!m_interface || !m_interface->isValid())
        return;

    QDBusMessage reply =
        m_interface->call("GetOperationState", operationId);

    if (reply.type() != QDBusMessage::ReplyMessage) {
        emit errorOccurred(reply.errorMessage());
        return;
    }

    if (reply.arguments().isEmpty())
        return;

    const QVariant argument = reply.arguments().at(0);

    if (!argument.canConvert<QDBusArgument>()) {
        emit errorOccurred(
            "GetOperationState dönüşü beklenen D-Bus map değil."
        );
        return;
    }

    const QDBusArgument dbusArgument =
        argument.value<QDBusArgument>();

    const QVariantMap result =
        decodeStringMap(dbusArgument);

    const QString status =
        result.value("status").toString();

    const QString message =
        result.value("message").toString();

    emit operationStatusReceived(
        operationId,
        status,
        message
    );
}

void ZethropolServicesClient::handleServiceSignal(const ServiceSignalInfo &serviceInfo)
{
    Q_UNUSED(serviceInfo);
    // Sinyal geldiğinde direkt listeyi asenkron olarak tazelemek en güvenli yoldur
    refreshServices();
}
