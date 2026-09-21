#include "UpdateService.h"

#include <QRegularExpression>
#include <QStringList>

UpdateService::UpdateService(QObject *parent)
    : QObject(parent)
{
    connect(&process, &QProcess::finished, this, &UpdateService::readOutput);
    connect(&cleanupProcess, &QProcess::finished, this, &UpdateService::finishCleanup);
    check();
}

bool UpdateService::checking() const
{
    return m_checking;
}

bool UpdateService::installing() const
{
    return m_installing;
}

bool UpdateService::available() const
{
    return m_available;
}

bool UpdateService::failed() const
{
    return m_failed;
}

int UpdateService::count() const
{
    return m_count;
}

QString UpdateService::status() const
{
    return m_status;
}

QString UpdateService::output() const
{
    return m_output;
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
    m_installing = false;
    m_failed = false;
    m_status = QStringLiteral("Checking for updates...");
    m_output.clear();

    emit stateChanged();

    process.start(QStringLiteral("/usr/bin/checkupdates"));
}

void UpdateService::install()
{
    if (process.state() != QProcess::NotRunning || m_installing)
        return;

    m_checking = false;
    m_installing = true;
    m_failed = false;
    m_status = QStringLiteral("Installing system updates...");
    m_output.clear();

    emit stateChanged();

    process.setProgram(QStringLiteral("/usr/bin/pkexec"));
    process.setArguments({
        QStringLiteral("/usr/bin/pacman"),
        QStringLiteral("-Syu"),
        QStringLiteral("--noconfirm")
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
}

void UpdateService::readOutput()
{
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());

    if (m_installing) {
        m_output = output;
        m_checking = false;

        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
            m_failed = false;
            m_available = false;
            m_count = 0;
            m_updates.clear();
            m_status = QStringLiteral("Cleaning package cache...");

            cleanupProcess.setProgram(QStringLiteral("/usr/bin/pkexec"));
            m_cleaningUninstalled = false;
            cleanupProcess.setArguments({
                QStringLiteral("/usr/bin/paccache"),
                QStringLiteral("-rk3")
            });
            cleanupProcess.setProcessChannelMode(QProcess::MergedChannels);
            cleanupProcess.start();
        } else {
            m_installing = false;
            m_failed = true;
            m_status = QStringLiteral("System update failed.");
        }

        emit stateChanged();
        return;
    }

    const QStringList lines = output.split(
        QRegularExpression(QStringLiteral("[\r\n]+")),
        Qt::SkipEmptyParts);

    QVariantList updates;

    for (const QString &line : lines) {
        const QStringList fields = line.split(
            QRegularExpression(QStringLiteral("[[:space:]]+")),
            Qt::SkipEmptyParts);

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
    m_failed = false;
    m_status = m_available
        ? QStringLiteral("Updates are available.")
        : QStringLiteral("System is up to date.");

    emit stateChanged();
}


void UpdateService::finishCleanup()
{
    const bool success =
        cleanupProcess.exitStatus() == QProcess::NormalExit &&
        cleanupProcess.exitCode() == 0;

    if (!m_cleaningUninstalled) {
        if (success) {
            m_cleaningUninstalled = true;
            m_status = QStringLiteral("Removing uninstalled packages from cache...");

            cleanupProcess.setProgram(QStringLiteral("/usr/bin/pkexec"));
            cleanupProcess.setArguments({
                QStringLiteral("/usr/bin/paccache"),
                QStringLiteral("-ruk0")
            });
            cleanupProcess.setProcessChannelMode(QProcess::MergedChannels);
            cleanupProcess.start();
        } else {
            m_installing = false;
            m_status =
                QStringLiteral("System updates installed, but package cache cleanup failed.");
            emit stateChanged();
        }

        return;
    }

    if (success) {
        m_status =
            QStringLiteral("System updates installed and package cache cleaned.");
    } else {
        m_status =
            QStringLiteral("System updates installed, but package cache cleanup failed.");
    }

    m_cleaningUninstalled = false;
    m_installing = false;
    emit stateChanged();
}
