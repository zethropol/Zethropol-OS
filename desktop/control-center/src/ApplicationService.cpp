#include "ApplicationService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSet>
#include <QProcess>
#include <QTextStream>
#include <QVariantMap>
#include <QRegularExpression>
#include <algorithm>

ApplicationService::ApplicationService(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QVariantList ApplicationService::applications() const
{
    return m_applications;
}

int ApplicationService::total() const
{
    return m_total;
}

bool ApplicationService::removing() const
{
    return m_removing;
}

QString ApplicationService::removeStatus() const
{
    return m_removeStatus;
}

void ApplicationService::refresh()
{
    const QStringList applicationPaths = {
        QStringLiteral("/usr/share/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)
    };

    QSet<QString> seenFiles;
    QVariantList applications;

    for (const QString &path : applicationPaths) {
        QDir directory(path);
        if (!directory.exists())
            continue;

        const QFileInfoList files = directory.entryInfoList(
            {QStringLiteral("*.desktop")},
            QDir::Files | QDir::Readable,
            QDir::Name);

        for (const QFileInfo &fileInfo : files) {
            const QString filePath = fileInfo.absoluteFilePath();

            if (seenFiles.contains(filePath))
                continue;

            seenFiles.insert(filePath);

            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            QTextStream stream(&file);
            QVariantMap application;
            bool desktopEntry = false;
            bool hidden = false;
            bool noDisplay = false;

            while (!stream.atEnd()) {
                const QString line = stream.readLine().trimmed();

                if (line == QStringLiteral("[Desktop Entry]")) {
                    desktopEntry = true;
                    continue;
                }

                if (!desktopEntry)
                    continue;

                if (line.startsWith(QStringLiteral("[")) &&
                    line.endsWith(QStringLiteral("]")))
                    break;

                const int separator = line.indexOf(QLatin1Char('='));
                if (separator <= 0)
                    continue;

                const QString key = line.left(separator);
                const QString value = line.mid(separator + 1);

                if (key == QStringLiteral("Name"))
                    application.insert(QStringLiteral("name"), value);
                else if (key == QStringLiteral("GenericName"))
                    application.insert(QStringLiteral("genericName"), value);
                else if (key == QStringLiteral("Comment"))
                    application.insert(QStringLiteral("description"), value);
                else if (key == QStringLiteral("Icon"))
                    application.insert(QStringLiteral("icon"), value);
                else if (key == QStringLiteral("Categories"))
                    application.insert(QStringLiteral("categories"), value);
                else if (key == QStringLiteral("Exec"))
                    application.insert(QStringLiteral("exec"), value);
                else if (key == QStringLiteral("Hidden"))
                    hidden = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
                else if (key == QStringLiteral("NoDisplay"))
                    noDisplay = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
            }

            if (!desktopEntry || hidden || noDisplay)
                continue;

            if (!application.contains(QStringLiteral("name")))
                continue;

            application.insert(QStringLiteral("desktopFile"), fileInfo.fileName());
            application.insert(QStringLiteral("path"), filePath);
            application.insert(
                QStringLiteral("launchable"),
                application.contains(QStringLiteral("exec")));
            application.insert(QStringLiteral("package"), QString());
            application.insert(QStringLiteral("packageManager"), QString());
            application.insert(QStringLiteral("removable"), false);

            if (filePath.startsWith(QStringLiteral("/usr/share/applications/")) ||
                filePath.startsWith(QStringLiteral("/usr/local/share/applications/"))) {

                QProcess ownerProcess;
                ownerProcess.start(
                    QStringLiteral("/usr/bin/pacman"),
                    {QStringLiteral("-Qo"), filePath});

                ownerProcess.waitForFinished(3000);

                if (ownerProcess.exitCode() == 0) {
                    const QString output =
                        QString::fromLocal8Bit(
                            ownerProcess.readAllStandardOutput()).trimmed();

                    const int separatorPosition = output.indexOf(QLatin1Char(44));

                    if (separatorPosition >= 0) {
                        const QString owner = output.mid(separatorPosition + 1).trimmed();

                        const QStringList ownerFields =
                            owner.split(QRegularExpression(QStringLiteral("[[:space:]]+")),
                                        Qt::SkipEmptyParts);

                        if (!ownerFields.isEmpty()) {
                            const QString package = ownerFields.at(0);

                            application.insert(QStringLiteral("package"), package);
                            application.insert(
                                QStringLiteral("packageManager"),
                                QStringLiteral("pacman"));
                            application.insert(QStringLiteral("removable"), true);
                        }
                                        }
                }
            }

            applications.append(application);
        }
    }

    std::sort(
        applications.begin(),
        applications.end(),
        [](const QVariant &left, const QVariant &right) {
            return left.toMap().value(QStringLiteral("name")).toString().localeAwareCompare(
                       right.toMap().value(QStringLiteral("name")).toString()) < 0;
        });

    m_applications = applications;
    m_total = applications.size();

    emit stateChanged();
}

void ApplicationService::removeApplication(const QString &package)
{
    if (m_removing)
        return;

    if (package.isEmpty())
        return;

    bool knownPackage = false;

    for (const QVariant &item : m_applications) {
        const QVariantMap application = item.toMap();

        if (application.value(QStringLiteral("package")).toString() == package &&
            application.value(QStringLiteral("packageManager")).toString() ==
                QStringLiteral("pacman") &&
            application.value(QStringLiteral("removable")).toBool()) {
            knownPackage = true;
            break;
        }
    }

    if (!knownPackage) {
        m_removeStatus = QStringLiteral("Application package is not removable.");
        emit stateChanged();
        return;
    }

    m_removing = true;
    m_removeStatus = QStringLiteral("Removing %1...").arg(package);
    emit stateChanged();

    auto *process = new QProcess(this);

    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this, process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                m_removeStatus =
                    QStringLiteral("Could not start application removal.");
            } else {
                m_removeStatus =
                    QStringLiteral("Application removal process failed.");
            }

            m_removing = false;
            process->deleteLater();
            emit stateChanged();
        });

    connect(
        process,
        &QProcess::finished,
        this,
        [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
            const QString output =
                QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();

            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                m_removeStatus =
                    QStringLiteral("Application removed successfully.");
            } else if (output.contains(QStringLiteral("Authentication cancelled"),
                                       Qt::CaseInsensitive)) {
                m_removeStatus =
                    QStringLiteral("Application removal cancelled.");
            } else if (!output.isEmpty()) {
                m_removeStatus = output;
            } else {
                m_removeStatus =
                    QStringLiteral("Application removal failed.");
            }

            m_removing = false;

            process->deleteLater();

            refresh();
            emit stateChanged();
        });

    process->setProgram(QStringLiteral("/usr/bin/pkexec"));
    process->setArguments({
        QStringLiteral("/usr/bin/pacman"),
        QStringLiteral("-R"),
        QStringLiteral("--noconfirm"),
        package
    });
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();
}
