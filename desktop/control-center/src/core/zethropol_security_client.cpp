#include "zethropol_security_client.h"
#include <QDBusMessage>
#include <QDBusReply>
#include <QDebug>

ZethropolSecurityClient::ZethropolSecurityClient(QObject *parent)
    : QObject(parent), m_interface(nullptr)
{
    m_interface = new QDBusInterface(
        "org.zethropol.Security",
        "/org/zethropol/Security",
        "org.zethropol.Security.v1",
        QDBusConnection::systemBus(),
        this
    );

    if (m_interface->isValid()) {
        // Rust tarafındaki state_changed sinyalini Qt slotuna bağlıyoruz (OBSERVE)
        QDBusConnection::systemBus().connect(
            "org.zethropol.Security",
            "/org/zethropol/Security",
            "org.zethropol.Security.v1",
            "state_changed",
            this,
            SLOT(handleDbusSignal(QVariantMap))
        );
        refreshState();
    } else {
        qWarning() << "Zethropol Security Service bağlantısı kurulamadı:" << m_interface->lastError().message();
    }
}

ZethropolSecurityClient::~ZethropolSecurityClient() {}

void ZethropolSecurityClient::refreshState() {
    if (!m_interface || !m_interface->isValid()) return;

    // Asenkron READ çağrısı
    QDBusMessage reply = m_interface->call("GetState");
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QVariantMap stateMap = reply.arguments().at(0).value<QVariantMap>();
        parseStateMap(stateMap);
    }
}

void ZethropolSecurityClient::toggleFirewall(bool enable) {
    if (!m_interface || !m_interface->isValid()) return;

    // ADMIN/ACTION tetikleme (Asenkron operationId geri döner)
    QDBusMessage reply = m_interface->call("ToggleFirewall", enable);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QVariantMap result = reply.arguments().at(0).value<QVariantMap>();
        QString opId = result.value("operation_id").toString();
        emit operationInitiated(opId);
    } else {
        emit errorOccurred(reply.errorMessage());
    }
}

void ZethropolSecurityClient::handleDbusSignal(const QVariantMap &newState) {
    parseStateMap(newState);
}

void ZethropolSecurityClient::parseStateMap(const QVariantMap &stateMap) {
    m_firewall = stateMap.value("firewall").toMap();
    m_secureBoot = stateMap.value("secure_boot").toMap();
    m_kernel = stateMap.value("kernel").toMap();
    m_apparmor = stateMap.value("apparmor").toMap();
    m_integrity = stateMap.value("system_integrity").toMap();

    emit securityStateChanged();
}
