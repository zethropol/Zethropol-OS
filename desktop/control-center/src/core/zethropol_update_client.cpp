#include "zethropol_update_client.h"
#include <QDBusMessage>

ZethropolUpdateClient::ZethropolUpdateClient(QObject *parent) : QObject(parent) {
    m_interface = new QDBusInterface("org.zethropol.Update", "/org/zethropol/Update", "org.zethropol.Update.v1", QDBusConnection::systemBus(), this);
    if (m_interface->isValid()) {
        QDBusConnection::systemBus().connect("org.zethropol.Update", "/org/zethropol/Update", "org.zethropol.Update.v1", "update_status_changed", this, SLOT(handleUpdateSignal(QVariantMap)));
        refreshUpdateState();
    }
}
void ZethropolUpdateClient::refreshUpdateState() {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage stateReply = m_interface->call("GetUpdateState");
    if (stateReply.type() == QDBusMessage::ReplyMessage && !stateReply.arguments().isEmpty()) {
        m_updateState = stateReply.arguments().at(0).value<QVariantMap>();
        emit updateStateChanged();
    }
    QDBusMessage listReply = m_interface->call("ListPendingUpdates");
    if (listReply.type() == QDBusMessage::ReplyMessage && !listReply.arguments().isEmpty()) {
        m_pendingUpdates = listReply.arguments().at(0).value<QVariantList>();
        emit pendingUpdatesChanged();
    }
}
void ZethropolUpdateClient::triggerSystemUpdate() {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("TriggerUpdate");
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        emit operationInitiated(reply.arguments().at(0).value<QVariantMap>().value("operation_id").toString());
    }
}
void ZethropolUpdateClient::checkOperationStatus(const QString &operationId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("GetOperationState", operationId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QVariantMap result = reply.arguments().at(0).value<QVariantMap>();
        emit operationStatusReceived(operationId, result.value("status").toString(), result.value("message").toString());
    }
}
void ZethropolUpdateClient::handleUpdateSignal(const QVariantMap &newState) {
    m_updateState = newState;
    emit updateStateChanged();
}