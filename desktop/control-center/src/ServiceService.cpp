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
