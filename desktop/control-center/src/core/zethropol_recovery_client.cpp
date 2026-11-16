#include "zethropol_recovery_client.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusVariant>

namespace {
QVariant unwrapDbusValue(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return unwrapDbusValue(value.value<QDBusVariant>().variant());
    if (value.userType() == QMetaType::QVariantMap) {
        const QVariantMap source = value.toMap();
        QVariantMap result;
        for (auto it = source.cbegin(); it != source.cend(); ++it)
            result.insert(it.key(), unwrapDbusValue(it.value()));
        return result;
    }
    if (value.userType() == QMetaType::QVariantList) {
        const QVariantList source = value.toList();
        QVariantList result;
        for (const QVariant &item : source)
            result.append(unwrapDbusValue(item));
        return result;
    }
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;
    const auto argument = value.value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::StructureType) {
        bool success = false;
        QString code;
        QString message;
        QString operationId;
        QVariantMap dataMap;
        argument.beginStructure();
        argument >> success >> code >> message >> operationId;
        if (!argument.atEnd() && argument.currentType() == QDBusArgument::MapType) {
            argument.beginMap();
            while (!argument.atEnd()) {
                argument.beginMapEntry();
                QString key;
                QDBusVariant wrapped;
                argument >> key >> wrapped;
                dataMap.insert(key, unwrapDbusValue(wrapped.variant()));
                argument.endMapEntry();
            }
            argument.endMap();
        }
        argument.endStructure();
        return QVariantMap{
            {QStringLiteral("success"), success},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message},
            {QStringLiteral("operation_id"), operationId},
            {QStringLiteral("data"), dataMap}
        };
    }
    if (argument.currentType() == QDBusArgument::ArrayType) {
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd()) {
            if (argument.currentType() == QDBusArgument::StructureType) {
                uint id = 0;
                QString name;
                QString createdAt;
                QString description;
                bool isCurrent = false;
                argument.beginStructure();
                argument >> id >> name >> createdAt >> description >> isCurrent;
                argument.endStructure();
                list.append(QVariantMap{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("created_at"), createdAt},
                    {QStringLiteral("description"), description},
                    {QStringLiteral("is_current"), isCurrent}
                });
            } else {
                QDBusVariant item;
                argument >> item;
                list.append(unwrapDbusValue(item.variant()));
            }
        }
        argument.endArray();
        return list;
    }
    if (argument.currentType() != QDBusArgument::MapType)
        return {};
    const bool isStringMap = argument.currentSignature() == QStringLiteral("a{ss}");
    QVariantMap map;
    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        if (isStringMap) {
            QString strVal;
            argument >> key >> strVal;
            map.insert(key, strVal);
        } else {
            QDBusVariant wrappedValue;
            argument >> key >> wrappedValue;
            map.insert(key, unwrapDbusValue(wrappedValue.variant()));
        }
        argument.endMapEntry();
    }
    argument.endMap();
    return map;
}
} // namespace

ZethropolRecoveryClient::ZethropolRecoveryClient(QObject *parent) : QObject(parent) {
    m_interface = new QDBusInterface("org.zethropol.Recovery", "/org/zethropol/Recovery", "org.zethropol.Recovery.v1", QDBusConnection::systemBus(), this);
    if (m_interface->isValid()) {
        QDBusConnection::systemBus().connect("org.zethropol.Recovery", "/org/zethropol/Recovery", "org.zethropol.Recovery.v1", "recovery_state_changed", this, SLOT(handleRecoverySignal(QVariantList)));
        refreshState();
        refreshRecoveryPoints();
    } else {
        m_lastError = m_interface->lastError().message();
        if (m_lastError.isEmpty())
            m_lastError = QStringLiteral("Recovery D-Bus service (org.zethropol.Recovery) is unavailable.");
    }
}
void ZethropolRecoveryClient::refreshState() {
    if (!m_interface || !m_interface->isValid()) {
        m_lastError = QStringLiteral("Recovery D-Bus interface is unavailable.");
        emit errorOccurred(m_lastError);
        return;
    }
    QDBusMessage reply = m_interface->call("GetState");
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        const QVariantMap envelope = unwrapDbusValue(reply.arguments().at(0)).toMap();
        m_state = unwrapDbusValue(envelope.value("data")).toMap();
        m_lastError.clear();
        emit stateChanged();
    } else if (reply.type() == QDBusMessage::ErrorMessage) {
        m_lastError = reply.errorMessage();
        emit errorOccurred(m_lastError);
    }
}
void ZethropolRecoveryClient::refreshRecoveryPoints() {
    if (!m_interface || !m_interface->isValid()) {
        m_lastError = QStringLiteral("Recovery D-Bus interface is unavailable.");
        emit errorOccurred(m_lastError);
        return;
    }
    QDBusMessage reply = m_interface->call("ListRecoveryPoints");
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        m_recoveryPoints = unwrapDbusValue(reply.arguments().at(0)).toList();
        m_lastError.clear();
        emit recoveryPointsChanged();
    } else if (reply.type() == QDBusMessage::ErrorMessage) {
        m_recoveryPoints.clear();
        m_lastError = reply.errorMessage();
        emit recoveryPointsChanged();
        emit errorOccurred(m_lastError);
    }
}
void ZethropolRecoveryClient::createRecoveryPoint(const QString &name, const QString &description) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("CreateRecoveryPoint", name, description);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        if (reply.arguments().size() >= 4 && reply.arguments().at(0).userType() == QMetaType::Bool) {
            emit operationInitiated(reply.arguments().at(3).toString());
        } else {
            emit operationInitiated(unwrapDbusValue(reply.arguments().at(0)).toMap().value("operation_id").toString());
        }
    }
}
void ZethropolRecoveryClient::rollbackToPoint(uint pointId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("RollbackToPoint", pointId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        if (reply.arguments().size() >= 4 && reply.arguments().at(0).userType() == QMetaType::Bool) {
            emit operationInitiated(reply.arguments().at(3).toString());
        } else {
            emit operationInitiated(unwrapDbusValue(reply.arguments().at(0)).toMap().value("operation_id").toString());
        }
    }
}
void ZethropolRecoveryClient::checkOperationStatus(const QString &operationId) {
    if (!m_interface || !m_interface->isValid()) return;
    QDBusMessage reply = m_interface->call("GetOperationState", operationId);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        QVariantMap result = unwrapDbusValue(reply.arguments().at(0)).toMap();
        emit operationStatusReceived(operationId, result.value("status").toString(), result.value("message").toString());
    }
}
void ZethropolRecoveryClient::handleRecoverySignal(const QVariantList &points) {
    m_recoveryPoints = points;
    emit recoveryPointsChanged();
}
