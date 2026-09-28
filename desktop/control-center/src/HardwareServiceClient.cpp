#include "HardwareServiceClient.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusArgument>

HardwareServiceClient::HardwareServiceClient(QObject *parent)
    : QObject(parent)
    , hardwareInterface(
        QStringLiteral("org.zethropol.Hardware"),
        QStringLiteral("/org/zethropol/Hardware"),
        QStringLiteral("org.zethropol.Hardware.v1"),
        QDBusConnection::systemBus(),
        this)
{
    connect(&monitorProcess, &QProcess::finished, this, &HardwareServiceClient::readMonitorOutput);

    monitorTimer.setInterval(1000);
    connect(&monitorTimer, &QTimer::timeout, this, [this]() {
        if (monitorProcess.state() == QProcess::NotRunning) {
            monitorProcess.start(
                QDir::homePath() + QStringLiteral("/Zethropol-OS/services/hardware/target/debug/hardware"),
                {QStringLiteral("--monitor-json")}
            );
        }
    });

    monitorProcess.start(
        QDir::homePath() + QStringLiteral("/Zethropol-OS/services/hardware/target/debug/hardware"),
        {QStringLiteral("--monitor-json")}
    );
    monitorTimer.start();

    auto *watcher = new QDBusPendingCallWatcher(
        hardwareInterface.asyncCall(QStringLiteral("GetState")),
        this
    );

    connect(watcher, &QDBusPendingCallWatcher::finished, this, &HardwareServiceClient::readState);
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
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto argument = value.value<QDBusArgument>();
    QVariantMap map;

    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();

        QString key;
        QVariant nestedValue;
        argument >> key >> nestedValue;

        map.insert(key, dbusValueToVariant(nestedValue));

        argument.endMapEntry();
    }
    argument.endMap();

    return map;
}

void HardwareServiceClient::readState()
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
            m_state = dbusValueToVariant(rawData).toMap();
            emit stateChanged();
        }
    }

    watcher->deleteLater();
}

void HardwareServiceClient::readMonitorOutput()
{
    const QByteArray output = monitorProcess.readAllStandardOutput();

    const QJsonDocument document = QJsonDocument::fromJson(output);

    if (!document.isObject())
        return;

    m_performance = document.object().toVariantMap();
    emit performanceChanged();
}
