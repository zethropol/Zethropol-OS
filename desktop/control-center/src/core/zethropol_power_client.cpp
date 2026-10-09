#include "zethropol_power_client.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QVariant>
#include <QDBusArgument>

static QVariant dbusValueToVariant(const QVariant &value)
{
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const QDBusArgument argument = value.value<QDBusArgument>();
    QVariantMap map;
    argument.beginMap();

    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        QVariant nested;
        argument >> key >> nested;
        map.insert(key, dbusValueToVariant(nested));
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

    QDBusConnection::systemBus().connect(
        QStringLiteral("org.zethropol.Power"),
        QStringLiteral("/org/zethropol/Power"),
        QStringLiteral("org.zethropol.Power.v1"),
        QStringLiteral("state_changed"),
        this,
        SLOT(handleDbusSignal(QVariant)));

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

void ZethropolPowerClient::handleDbusSignal(const QVariant &state)
{
    m_state = dbusValueToVariant(state).toMap();
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
