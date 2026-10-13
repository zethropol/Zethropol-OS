#include "zethropol_update_client.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QMetaType>

namespace {
bool isDBusArgument(const QVariant &value)
{
    return value.userType() == qMetaTypeId<QDBusArgument>();
}

QVariantMap readStringMap(const QVariant &value)
{
    if (!isDBusArgument(value))
        return value.toMap();

    const QDBusArgument argument = value.value<QDBusArgument>();
    QVariantMap result;
    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        QVariant item;
        argument >> key >> item;
        argument.endMapEntry();
        result.insert(key, item);
    }
    argument.endMap();
    return result;
}

QVariantMap readUpdateState(const QVariant &value)
{
    if (!isDBusArgument(value))
        return value.toMap();

    const QDBusArgument argument = value.value<QDBusArgument>();
    QVariantMap result;
    bool checking = false;
    bool updating = false;
    QString lastCheck;
    uint count = 0;

    argument.beginStructure();
    argument >> checking >> updating >> lastCheck >> count;
    argument.endStructure();

    result.insert(QStringLiteral("is_checking"), checking);
    result.insert(QStringLiteral("is_updating"), updating);
    result.insert(QStringLiteral("last_check_time"), lastCheck);
    result.insert(QStringLiteral("pending_updates_count"), count);
    return result;
}

QVariantList readPendingUpdates(const QVariant &value)
{
    if (!isDBusArgument(value))
        return value.toList();

    const QDBusArgument argument = value.value<QDBusArgument>();
    QVariantList result;
    argument.beginArray();
    while (!argument.atEnd()) {
        QString name;
        QString currentVersion;
        QString newVersion;
        QString repository;
        qulonglong downloadSize = 0;

        argument.beginStructure();
        argument >> name >> currentVersion >> newVersion >> repository >> downloadSize;
        argument.endStructure();

        QVariantMap item;
        item.insert(QStringLiteral("package_name"), name);
        item.insert(QStringLiteral("current_version"), currentVersion);
        item.insert(QStringLiteral("new_version"), newVersion);
        item.insert(QStringLiteral("repository"), repository);
        item.insert(QStringLiteral("download_size"), downloadSize);
        result.append(item);
    }
    argument.endArray();
    return result;
}

QVariantMap readOperationResult(const QVariant &value)
{
    if (!isDBusArgument(value))
        return value.toMap();

    const QDBusArgument argument = value.value<QDBusArgument>();
    bool success = false;
    QString code;
    QString message;
    QString operationId;
    QVariantMap data;

    argument.beginStructure();
    argument >> success >> code >> message >> operationId >> data;
    argument.endStructure();

    QVariantMap result;
    result.insert(QStringLiteral("success"), success);
    result.insert(QStringLiteral("code"), code);
    result.insert(QStringLiteral("message"), message);
    result.insert(QStringLiteral("operation_id"), operationId);
    result.insert(QStringLiteral("data"), readStringMap(data));
    return result;
}

QString dbusError(const QDBusMessage &reply)
{
    return reply.errorMessage().isEmpty()
        ? QStringLiteral("Update D-Bus çağrısı beklenen yanıtı döndürmedi.")
        : reply.errorMessage();
}
}

ZethropolUpdateClient::ZethropolUpdateClient(QObject *parent)
    : QObject(parent)
{
    m_interface = new QDBusInterface(
        QStringLiteral("org.zethropol.Update"),
        QStringLiteral("/org/zethropol/Update"),
        QStringLiteral("org.zethropol.Update.v1"),
        QDBusConnection::systemBus(), this);

    if (!m_interface->isValid()) {
        emit errorOccurred(m_interface->lastError().message());
        return;
    }

    QDBusConnection::systemBus().connect(
        QStringLiteral("org.zethropol.Update"),
        QStringLiteral("/org/zethropol/Update"),
        QStringLiteral("org.zethropol.Update.v1"),
        QStringLiteral("UpdateStatusChanged"),
        this, SLOT(handleUpdateSignal(QVariant)));
}

void ZethropolUpdateClient::refreshUpdateState()
{
    if (!m_interface || !m_interface->isValid()) {
        const QString message = m_interface
            ? m_interface->lastError().message()
            : QStringLiteral("Update D-Bus arayüzü oluşturulamadı.");
        emit errorOccurred(message);
        emit refreshFinished(false, message);
        emit pendingUpdatesChanged();
        return;
    }

    const QDBusMessage stateReply = m_interface->call(QStringLiteral("GetUpdateState"));
    if (stateReply.type() != QDBusMessage::ReplyMessage || stateReply.arguments().isEmpty()) {
        const QString message = dbusError(stateReply);
        emit errorOccurred(message);
        emit refreshFinished(false, message);
        emit pendingUpdatesChanged();
        return;
    }

    m_updateState = readUpdateState(stateReply.arguments().constFirst());
    emit updateStateChanged();

    const QDBusMessage listReply = m_interface->call(QStringLiteral("ListPendingUpdates"));
    if (listReply.type() != QDBusMessage::ReplyMessage || listReply.arguments().isEmpty()) {
        const QString message = dbusError(listReply);
        emit errorOccurred(message);
        emit refreshFinished(false, message);
        emit pendingUpdatesChanged();
        return;
    }

    m_pendingUpdates = readPendingUpdates(listReply.arguments().constFirst());
    emit pendingUpdatesChanged();
    emit refreshFinished(true, QString());
}

void ZethropolUpdateClient::triggerSystemUpdate()
{
    if (!m_interface || !m_interface->isValid()) {
        emit errorOccurred(QStringLiteral("Update D-Bus arayüzü kullanılamıyor."));
        return;
    }

    const QDBusMessage reply = m_interface->call(QStringLiteral("TriggerUpdate"));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        emit errorOccurred(dbusError(reply));
        return;
    }

    const QVariantMap result = readOperationResult(reply.arguments().constFirst());
    const QString message = result.value(QStringLiteral("message")).toString();
    if (!result.value(QStringLiteral("success")).toBool()) {
        emit errorOccurred(message.isEmpty()
            ? QStringLiteral("Güncelleme başlatılmadı.")
            : message);
        return;
    }

    emit operationInitiated(result.value(QStringLiteral("operation_id")).toString());
}

void ZethropolUpdateClient::checkOperationStatus(const QString &operationId)
{
    if (!m_interface || !m_interface->isValid()) {
        emit errorOccurred(QStringLiteral("Update D-Bus arayüzü kullanılamıyor."));
        return;
    }

    const QDBusMessage reply = m_interface->call(
        QStringLiteral("GetOperationState"), operationId);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        emit errorOccurred(dbusError(reply));
        return;
    }

    const QVariantMap result = readStringMap(reply.arguments().constFirst());
    emit operationStatusReceived(
        operationId,
        result.value(QStringLiteral("status")).toString(),
        result.value(QStringLiteral("message")).toString());
}

void ZethropolUpdateClient::handleUpdateSignal(const QVariant &newState)
{
    m_updateState = readUpdateState(newState);
    emit updateStateChanged();
}
