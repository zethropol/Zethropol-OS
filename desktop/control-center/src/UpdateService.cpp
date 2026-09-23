#include "UpdateService.h"

#include <QRegularExpression>
#include <QStringList>

UpdateService::UpdateService(QObject *parent)
    : QObject(parent)
{
    connect(&process, &QProcess::finished, this, &UpdateService::readOutput);
    connect(&cleanupProcess, &QProcess::finished, this, &UpdateService::finishCleanup);
    connect(&orphanProcess, &QProcess::finished, this, &UpdateService::finishOrphanScan);
    connect(&orphanProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        Q_UNUSED(error);

        if (m_installing) {
            m_installing = false;
            m_status = QStringLiteral("System update process completed, but orphan package check failed.");
        }

        emit stateChanged();
    });
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

QStringList UpdateService::orphanPackages() const
{
    return m_orphanPackages;
}

int UpdateService::orphanCount() const
{
    return m_orphanPackages.size();
}

bool UpdateService::removingOrphans() const
{
    return m_removingOrphans;
}

void UpdateService::check()
{
    if (process.state() != QProcess::NotRunning || orphanProcess.state() != QProcess::NotRunning)
        return;

    m_checking = true;
    m_installing = false;
    m_failed = false;
    m_status = QStringLiteral("Checking for updates...");
    m_output.clear();

    emit stateChanged();

    process.start(QStringLiteral("/usr/bin/checkupdates"));

    orphanProcess.start(QStringLiteral("/usr/bin/pacman"), {
        QStringLiteral("-Qtdq")
    });
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
            cleanupProcess.setArguments({
                QStringLiteral("/bin/sh"),
                QStringLiteral("-c"),
                QStringLiteral("/usr/bin/paccache -rk3 && /usr/bin/paccache -ruk0")
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


void UpdateService::finishOrphanScan()
{
    const QString output =
        QString::fromLocal8Bit(orphanProcess.readAllStandardOutput()).trimmed();

    m_orphanPackages = output.isEmpty()
        ? QStringList()
        : output.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);

    if (m_installing) {
        m_installing = false;

        if (m_orphanPackages.isEmpty()) {
            m_status = QStringLiteral("System updates installed successfully.");
        } else {
            m_status = QStringLiteral("System updates installed. Orphan packages found.");
        }
    }

    emit stateChanged();
}

void UpdateService::removeOrphans()
{
    if (m_removingOrphans || m_orphanPackages.isEmpty())
        return;

    m_removingOrphans = true;
    m_status = QStringLiteral("Removing orphan packages...");
    emit stateChanged();

    auto *process = new QProcess(this);

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        Q_UNUSED(error);
        m_removingOrphans = false;
        m_status = QStringLiteral("Orphan package removal failed.");
        process->deleteLater();
        emit stateChanged();
    });

    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            m_orphanPackages.clear();
            m_status = QStringLiteral("Orphan packages removed successfully.");
        } else {
            const QString output =
                QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();
            m_status = !output.isEmpty()
                ? output
                : QStringLiteral("Orphan package removal failed.");
        }

        m_removingOrphans = false;
        process->deleteLater();
        emit stateChanged();
    });

    QStringList arguments = {
        QStringLiteral("/usr/bin/pacman"),
        QStringLiteral("-Rns"),
        QStringLiteral("--noconfirm")
    };
    arguments.append(m_orphanPackages);

    process->setProgram(QStringLiteral("/usr/bin/pkexec"));
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();
}

void UpdateService::finishCleanup()
{
    const bool success =
        cleanupProcess.exitStatus() == QProcess::NormalExit &&
        cleanupProcess.exitCode() == 0;

    const QString output =
        QString::fromLocal8Bit(cleanupProcess.readAllStandardOutput()).trimmed();

    if (success) {
        m_status = QStringLiteral("Checking for orphan packages...");

        if (orphanProcess.state() == QProcess::NotRunning)
            orphanProcess.start(QStringLiteral("/usr/bin/pacman"), {
                QStringLiteral("-Qtdq")
            });
    } else {
        m_installing = false;

        if (!output.isEmpty()) {
            m_status = output;
        } else {
            m_status =
                QStringLiteral("System updates installed, but package cache cleanup failed.");
        }
    }

    emit stateChanged();
}
