#include "HardwareServiceClient.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

HardwareServiceClient::HardwareServiceClient(QObject *parent)
    : QObject(parent)
{
    connect(&process, &QProcess::finished, this, &HardwareServiceClient::readOutput);
    connect(&monitorProcess, &QProcess::finished, this, &HardwareServiceClient::readMonitorOutput);

    monitorTimer.setInterval(1000);
    connect(&monitorTimer, &QTimer::timeout, this, [this]() {
        if (monitorProcess.state() == QProcess::NotRunning) {
            monitorProcess.start(QDir::homePath() + QStringLiteral("/Zethropol-OS/services/hardware/target/debug/hardware"), {QStringLiteral("--monitor-json")});
        }
    });

    process.setProgram(QDir::homePath() + QStringLiteral("/Zethropol-OS/services/hardware/target/debug/hardware"));
    process.setArguments({QStringLiteral("--json")});
    process.start();
    monitorProcess.start(QDir::homePath() + QStringLiteral("/Zethropol-OS/services/hardware/target/debug/hardware"), {QStringLiteral("--monitor-json")});
    monitorTimer.start();
}

QVariantMap HardwareServiceClient::state() const
{
    return m_state;
}

QVariantMap HardwareServiceClient::performance() const
{
    return m_performance;
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

void HardwareServiceClient::readOutput()
{
    const QByteArray output = process.readAllStandardOutput();
    const QJsonDocument document = QJsonDocument::fromJson(output);

    if (!document.isObject())
        return;

    m_state = document.object().toVariantMap();
    emit stateChanged();
}
