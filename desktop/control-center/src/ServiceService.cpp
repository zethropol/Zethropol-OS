#include "ServiceService.h"

#include <QProcess>
#include <QRegularExpression>
#include <QVariantMap>

ServiceService::ServiceService(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QVariantList ServiceService::services() const
{
    return m_services;
}

int ServiceService::total() const
{
    return m_total;
}

int ServiceService::active() const
{
    return m_active;
}

int ServiceService::failed() const
{
    return m_failed;
}

bool ServiceService::busy() const
{
    return m_busy;
}

QString ServiceService::actionStatus() const
{
    return m_actionStatus;
}

void ServiceService::refresh()
{
    QProcess unitsProcess;
    unitsProcess.start(QStringLiteral("/usr/bin/systemctl"),
                       {QStringLiteral("list-units"),
                        QStringLiteral("--type=service"),
                        QStringLiteral("--all"),
                        QStringLiteral("--no-legend"),
                        QStringLiteral("--plain"),
                        QStringLiteral("--no-pager")});
    unitsProcess.waitForFinished(5000);

    QProcess filesProcess;
    filesProcess.start(QStringLiteral("/usr/bin/systemctl"),
                       {QStringLiteral("list-unit-files"),
                        QStringLiteral("--type=service"),
                        QStringLiteral("--no-legend"),
                        QStringLiteral("--plain"),
                        QStringLiteral("--no-pager")});
    filesProcess.waitForFinished(5000);

    QVariantMap servicesByName;

    const QStringList unitLines =
        QString::fromLocal8Bit(unitsProcess.readAllStandardOutput())
            .split(QRegularExpression(QStringLiteral("[\r\n]+")),
                   Qt::SkipEmptyParts);

    for (const QString &line : unitLines) {
        const QStringList fields =
            line.trimmed().split(QRegularExpression(QStringLiteral("[[:space:]]+")),
                                 Qt::KeepEmptyParts);

        if (fields.size() < 4)
            continue;

        QVariantMap service;
        service.insert(QStringLiteral("name"), fields.at(0));
        service.insert(QStringLiteral("load"), fields.at(1));
        service.insert(QStringLiteral("active"), fields.at(2));
        service.insert(QStringLiteral("sub"), fields.at(3));

        const QString description =
            line.section(QRegularExpression(QStringLiteral("[[:space:]]+")),
                         4)
                .trimmed();

        service.insert(QStringLiteral("description"), description);
        service.insert(QStringLiteral("enabled"), QStringLiteral("unknown"));

        servicesByName.insert(fields.at(0), service);
    }

    const QStringList fileLines =
        QString::fromLocal8Bit(filesProcess.readAllStandardOutput())
            .split(QRegularExpression(QStringLiteral("[\r\n]+")),
                   Qt::SkipEmptyParts);

    for (const QString &line : fileLines) {
        const QStringList fields =
            line.trimmed().split(QRegularExpression(QStringLiteral("[[:space:]]+")),
                                 Qt::SkipEmptyParts);

        if (fields.size() < 2)
            continue;

        const QString name = fields.at(0);
        QVariantMap service = servicesByName.value(name).toMap();

        if (service.isEmpty()) {
            service.insert(QStringLiteral("name"), name);
            service.insert(QStringLiteral("load"), QStringLiteral("unknown"));
            service.insert(QStringLiteral("active"), QStringLiteral("inactive"));
            service.insert(QStringLiteral("sub"), QStringLiteral("dead"));
            service.insert(QStringLiteral("description"), QString());
        }

        service.insert(QStringLiteral("enabled"), fields.at(1));
        servicesByName.insert(name, service);
    }

    QVariantList services;
    int activeCount = 0;
    int failedCount = 0;

    for (auto it = servicesByName.cbegin(); it != servicesByName.cend(); ++it) {
        const QVariantMap service = it.value().toMap();
        const QString activeState =
            service.value(QStringLiteral("active")).toString();

        if (activeState == QStringLiteral("active"))
            ++activeCount;

        if (activeState == QStringLiteral("failed"))
            ++failedCount;

        services.append(service);
    }

    m_services = services;
    m_total = services.size();
    m_active = activeCount;
    m_failed = failedCount;

    emit stateChanged();
}


bool ServiceService::serviceExists(const QString &name) const
{
    if (name.isEmpty())
        return false;

    for (const QVariant &item : m_services) {
        const QVariantMap service = item.toMap();
        if (service.value(QStringLiteral("name")).toString() == name)
            return true;
    }

    return false;
}

void ServiceService::runAction(const QString &name, const QString &action)
{
    if (m_busy || !serviceExists(name))
        return;

    m_busy = true;
    m_actionStatus = QStringLiteral("%1 %2...").arg(action, name);
    emit stateChanged();

    auto *process = new QProcess(this);

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            m_actionStatus = QStringLiteral("Could not start service action.");
        else
            m_actionStatus = QStringLiteral("Service action failed.");

        m_busy = false;
        process->deleteLater();
        emit stateChanged();
    });

    connect(process, &QProcess::finished, this,
            [this, process, action](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output =
            QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();

        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            m_actionStatus =
                QStringLiteral("Service %1 completed successfully.").arg(action);
            m_busy = false;
            process->deleteLater();
            refresh();
            emit stateChanged();
            return;
        }

        m_actionStatus = !output.isEmpty()
            ? output
            : QStringLiteral("Service %1 failed.").arg(action);

        m_busy = false;
        process->deleteLater();
        emit stateChanged();
    });

    process->setProgram(QStringLiteral("/usr/bin/pkexec"));
    process->setArguments({
        QStringLiteral("/usr/bin/systemctl"),
        action,
        name
    });
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();
}

void ServiceService::startService(const QString &name)
{
    runAction(name, QStringLiteral("start"));
}

void ServiceService::stopService(const QString &name)
{
    runAction(name, QStringLiteral("stop"));
}

void ServiceService::restartService(const QString &name)
{
    runAction(name, QStringLiteral("restart"));
}

void ServiceService::enableService(const QString &name)
{
    runAction(name, QStringLiteral("enable"));
}

void ServiceService::disableService(const QString &name)
{
    runAction(name, QStringLiteral("disable"));
}
