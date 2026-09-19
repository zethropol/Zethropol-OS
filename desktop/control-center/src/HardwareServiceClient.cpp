#include "HardwareServiceClient.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

HardwareServiceClient::HardwareServiceClient(QObject *parent)
    : QObject(parent)
{
    connect(&process, &QProcess::finished, this, &HardwareServiceClient::readOutput);

    process.setProgram(QDir::homePath() + QStringLiteral("/Zethropol-OS/services/hardware/target/debug/hardware"));
    process.setArguments({QStringLiteral("--json")});
    process.start();
}

QVariantMap HardwareServiceClient::state() const
{
    return m_state;
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
