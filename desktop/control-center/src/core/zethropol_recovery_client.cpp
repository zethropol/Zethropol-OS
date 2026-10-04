#include "zethropol_recovery_client.h"
#include <QDBusMessage>

ZethropolRecoveryClient::ZethropolRecoveryClient(QObject *parent) : QObject(parent) {
    m_interface = new QDBusInterface("org.zethropol.Recovery", "/org/zethropol/Recovery", "org.zethropol.Recovery.v1", QDBusConnection::systemBus(), this);
    if (m_interface->isValid()) {
        QDBusConnection::systemBus().connect("org.zethropol.Recovery", "/org/zethropol/Recovery", "org.zethropol.Recovery.v1", "recovery_state_changed", this, SLOT(handleRecoverySignal(QVariantList)));
        refreshRecoveryPoints();
    }
}
void ZethropolRecoveryClient::refreshRecoveryPoints() {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("ListRecoveryPoints");
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        m_recoveryPoints = reply.arguments().at(0).value<QVariantList>();
        emit recoveryPointsChanged();
    }
}
void ZethropolRecoveryClient::createRecoveryPoint(const QString &name, const QString &description) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("CreateRecoveryPoint", name, description);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        emit operationInitiated(reply.arguments().at(0).value<QVariantMap>().value("operation_id").toString());
    }
}
void ZethropolRecoveryClient::rollbackToPoint(uint pointId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("RollbackToPoint", pointId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        emit operationInitiated(reply.arguments().at(0).value<QVariantMap>().value("operation_id").toString());
    }
}
void ZethropolRecoveryClient::checkOperationStatus(const QString &operationId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("GetOperationState", operationId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QVariantMap result = reply.arguments().at(0).value<QVariantMap>();
        emit operationStatusReceived(operationId, result.value("status").toString(), result.value("message").toString());
    }
}
void ZethropolRecoveryClient::handleRecoverySignal(const QVariantList &points) {
    m_recoveryPoints = points;
    emit recoveryPointsChanged();
}