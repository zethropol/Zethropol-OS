#include "zethropol_applications_client.h"
#include <QDBusMessage>

ZethropolApplicationsClient::ZethropolApplicationsClient(QObject *parent) : QObject(parent) {
    m_interface = new QDBusInterface("org.zethropol.Applications", "/org/zethropol/Applications", "org.zethropol.Applications.v1", QDBusConnection::systemBus(), this);
    if (m_interface->isValid()) {
        QDBusConnection::systemBus().connect("org.zethropol.Applications", "/org/zethropol/Applications", "org.zethropol.Applications.v1", "applications_list_changed", this, SLOT(handleAppsSignal(QVariantList)));
        refreshApplications();
    }
}
void ZethropolApplicationsClient::refreshApplications() {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("ListApplications");
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        m_applicationsList = reply.arguments().at(0).value<QVariantList>();
        emit applicationsChanged();
    }
}
void ZethropolApplicationsClient::uninstallApplication(const QString &appId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("UninstallApplication", appId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        emit operationInitiated(reply.arguments().at(0).value<QVariantMap>().value("operation_id").toString());
    }
}
void ZethropolApplicationsClient::checkOperationStatus(const QString &operationId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("GetOperationState", operationId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QVariantMap result = reply.arguments().at(0).value<QVariantMap>();
        emit operationStatusReceived(operationId, result.value("status").toString(), result.value("message").toString());
    }
}
void ZethropolApplicationsClient::handleAppsSignal(const QVariantList &apps) {
    m_applicationsList = apps;
    emit applicationsChanged();
}