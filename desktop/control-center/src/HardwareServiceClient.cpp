#include "HardwareServiceClient.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QTimer>
#include <QDateTime>
#include <QDBusMessage>

HardwareServiceClient::HardwareServiceClient(QObject *parent)
    : QObject(parent)
    , performanceInterface(
        QStringLiteral("org.zethropol.Performance"),
        QStringLiteral("/org/zethropol/Performance"),
        QStringLiteral("org.zethropol.Performance.v1"),
        QDBusConnection::systemBus(),
        this)
{
    hardwareTimer.setInterval(10000);
    connect(&hardwareTimer, &QTimer::timeout, this, &HardwareServiceClient::refresh);
    hardwareTimer.start();
    QTimer::singleShot(0, this, &HardwareServiceClient::refresh);

    performanceTimer.setInterval(1000);
    connect(&performanceTimer, &QTimer::timeout, this, &HardwareServiceClient::requestPerformance);
    performanceTimer.start();

    requestPerformance();
}

QVariantMap HardwareServiceClient::state() const
{
    return m_state;
}

QVariantMap HardwareServiceClient::performance() const
{
    return m_performance;
}

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

    const auto argument = value.value<QDBusArgument>();
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
        QDBusVariant wrappedValue;
        argument >> key >> wrappedValue;
        map.insert(key, dbusValueToVariant(wrappedValue.variant()));
        argument.endMapEntry();
    }
    argument.endMap();
    return map;
}

void HardwareServiceClient::refresh()
{
    if (m_loading)
        return;
    m_loading = true;
    emit stateChanged();
    const auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Hardware"), QStringLiteral("/org/zethropol/Hardware"),
        QStringLiteral("org.zethropol.Hardware.v1"), QStringLiteral("GetState"));
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &HardwareServiceClient::readState);
}

void HardwareServiceClient::readState()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;
    QDBusPendingReply<QVariantMap> reply = *watcher;
    m_loading = false;
    m_available = false;
    m_error.clear();
    if (reply.isError()) {
        m_error = reply.error().message();
    } else {
        const QVariantMap result = reply.value();
        const QVariantMap data = dbusValueToVariant(result.value(QStringLiteral("data"))).toMap();
        if (result.value(QStringLiteral("success")).toBool()
            && result.value(QStringLiteral("code")).toString() == QStringLiteral("ok")
            && !data.isEmpty()) {
            m_state = data;
            m_available = true;
            m_lastUpdated = QDateTime::currentDateTime().toString(Qt::ISODate);
        } else {
            m_error = result.value(QStringLiteral("message")).toString();
            if (m_error.isEmpty())
                m_error = QStringLiteral("Hardware service returned no valid data.");
        }
    }
    if (!m_available)
        m_state.clear();
    emit stateChanged();
    watcher->deleteLater();
}

void HardwareServiceClient::requestPerformance()
{
    auto *watcher = new QDBusPendingCallWatcher(
        performanceInterface.asyncCall(QStringLiteral("GetState")),
        this
    );

    connect(
        watcher,
        &QDBusPendingCallWatcher::finished,
        this,
        &HardwareServiceClient::readPerformance
    );
}

void HardwareServiceClient::readPerformance()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;

    QDBusPendingReply<QVariantMap> reply = *watcher;

    if (!reply.isError()) {
        const QVariantMap result = reply.value();

        if (result.value(QStringLiteral("success")).toBool()
            && result.value(QStringLiteral("code")).toString() == QStringLiteral("ok")) {
            const QVariant rawData = result.value(QStringLiteral("data"));
            m_performance = dbusValueToVariant(rawData).toMap();
            emit performanceChanged();
        }
    }

    watcher->deleteLater();
}
