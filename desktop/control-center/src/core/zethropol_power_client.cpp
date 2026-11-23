#include "zethropol_power_client.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QVariant>
#include <QDBusArgument>

static QVariant dbusValueToVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return dbusValueToVariant(value.value<QDBusVariant>().variant());

    if (value.userType() == QMetaType::QVariantMap) {
        const QVariantMap source = value.toMap();
        QVariantMap result;
        for (auto it = source.cbegin(); it != source.cend(); ++it)
            result.insert(it.key(), dbusValueToVariant(it.value()));
        return result;
    }

    if (value.userType() == QMetaType::QVariantList) {
        const QVariantList source = value.toList();
        QVariantList result;
        result.reserve(source.size());
        for (const QVariant &item : source)
            result.append(dbusValueToVariant(item));
        return result;
    }

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const QDBusArgument argument = value.value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::ArrayType) {
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd()) {
            QDBusVariant item;
            argument >> item;
            list.append(dbusValueToVariant(item.variant()));
        }
        argument.endArray();
        return list;
    }
    if (argument.currentType() != QDBusArgument::MapType)
        return {};

    QVariantMap map;
    argument.beginMap();

    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        QDBusVariant nested;
        argument >> key >> nested;
        map.insert(key, dbusValueToVariant(nested.variant()));
        argument.endMapEntry();
    }

    argument.endMap();
    return map;
}

ZethropolPowerClient::ZethropolPowerClient(QObject *parent)
    : QObject(parent)
    , m_interface(new QDBusInterface(
          QStringLiteral("org.zethropol.Power"),
          QStringLiteral("/org/zethropol/Power"),
          QStringLiteral("org.zethropol.Power.v1"),
          QDBusConnection::systemBus(),
          this))
{
    if (!m_interface->isValid()) {
        emit errorOccurred(m_interface->lastError().message());
        return;
    }

    m_signalConnected = QDBusConnection::systemBus().connect(
        QStringLiteral("org.zethropol.Power"),
        QStringLiteral("/org/zethropol/Power"),
        QStringLiteral("org.zethropol.Power.v1"),
        QStringLiteral("StateChanged"),
        this,
        SLOT(handleDbusSignal(QVariantMap)));

    refreshState();
}

ZethropolPowerClient::~ZethropolPowerClient() = default;

void ZethropolPowerClient::refreshState()
{
    if (!m_interface->isValid()) {
        emit errorOccurred(QStringLiteral("Power D-Bus interface is unavailable"));
        return;
    }

    const QDBusMessage reply =
        m_interface->call(QStringLiteral("GetState"));

    if (reply.type() == QDBusMessage::ErrorMessage) {
        emit errorOccurred(reply.errorMessage());
        return;
    }

    if (reply.arguments().isEmpty()) {
        emit errorOccurred(QStringLiteral("Power service returned an empty response"));
        return;
    }

    const QVariantMap result = dbusValueToVariant(reply.arguments().constFirst()).toMap();

    if (result.isEmpty()) {
        emit errorOccurred(QStringLiteral("Invalid Power service response"));
        return;
    }

    parseResult(result);
}

void ZethropolPowerClient::setProfile(const QString &profile)
{
    invokeOperation(QStringLiteral("SetProfile"), {profile});
}

void ZethropolPowerClient::setGovernor(const QString &governor)
{
    invokeOperation(QStringLiteral("SetGovernor"), {governor});
}

void ZethropolPowerClient::suspend()
{
    invokeOperation(QStringLiteral("Suspend"));
}

void ZethropolPowerClient::hibernate()
{
    invokeOperation(QStringLiteral("Hibernate"));
}

void ZethropolPowerClient::reboot()
{
    invokeOperation(QStringLiteral("Reboot"));
}

void ZethropolPowerClient::shutdown()
{
    invokeOperation(QStringLiteral("Shutdown"));
}

void ZethropolPowerClient::handleDbusSignal(const QVariantMap &state)
{
    const QVariantMap normalized = dbusValueToVariant(state).toMap();
    if (normalized.isEmpty())
        return;
    m_state = normalized;
    emit powerStateChanged();
}

void ZethropolPowerClient::parseResult(const QVariantMap &result)
{
    const bool success = result.value(QStringLiteral("success")).toBool();

    if (!success) {
        emit errorOccurred(
            result.value(QStringLiteral("message"),
                        QStringLiteral("Power operation failed")).toString());
        return;
    }

    const QString operationId =
        result.value(QStringLiteral("operationId")).toString();

    if (!operationId.isEmpty())
        emit operationInitiated(operationId);

    const QVariant data = dbusValueToVariant(result.value(QStringLiteral("data")));

    if (data.isValid() && data.canConvert<QVariantMap>()) {
        m_state = data.toMap();
        emit powerStateChanged();
    }
}

void ZethropolPowerClient::invokeOperation(
    const QString &method,
    const QVariantList &args)
{
    if (!m_interface->isValid()) {
        emit errorOccurred(QStringLiteral("Power D-Bus interface is unavailable"));
        return;
    }

    const QDBusMessage reply =
        m_interface->callWithArgumentList(
            QDBus::AutoDetect,
            method,
            args);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        emit errorOccurred(reply.errorMessage());
        return;
    }

    if (reply.arguments().isEmpty()) {
        emit errorOccurred(QStringLiteral("Power service returned an empty response"));
        return;
    }

    const QVariantMap result = dbusValueToVariant(reply.arguments().constFirst()).toMap();

    if (result.isEmpty()) {
        emit errorOccurred(QStringLiteral("Invalid Power service response"));
        return;
    }

    parseResult(result);
}
