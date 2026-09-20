#include "UpdateService.h"

#include <QRegularExpression>
#include <QStringList>

UpdateService::UpdateService(QObject *parent)
    : QObject(parent)
{
    connect(&process, &QProcess::finished, this, &UpdateService::readOutput);
    check();
}

bool UpdateService::checking() const
{
    return m_checking;
}

bool UpdateService::available() const
{
    return m_available;
}

int UpdateService::count() const
{
    return m_count;
}

QVariantList UpdateService::updates() const
{
    return m_updates;
}

void UpdateService::check()
{
    if (process.state() != QProcess::NotRunning)
        return;

    m_checking = true;
    emit stateChanged();

    process.start(QStringLiteral("/usr/bin/checkupdates"));
}

void UpdateService::readOutput()
{
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QStringList lines = output.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);

    QVariantList updates;

    for (const QString &line : lines) {
        const QStringList fields = line.split(QRegularExpression(QStringLiteral("[[:space:]]+")), Qt::SkipEmptyParts);
        if (fields.size() < 3)
            continue;

        QVariantMap update;
        update.insert(QStringLiteral("name"), fields.at(0));
        update.insert(QStringLiteral("currentVersion"), fields.at(1));
        update.insert(QStringLiteral("newVersion"), fields.at(2));
        updates.append(update);
    }

    m_updates = updates;
    m_count = updates.size();
    m_available = m_count > 0;
    m_checking = false;

    emit stateChanged();
}
