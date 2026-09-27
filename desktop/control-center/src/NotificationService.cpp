#include "NotificationService.h"

#include <QProcess>

NotificationService::NotificationService(QObject *parent)
    : QObject(parent)
{
}

void NotificationService::send(const QString &title, const QString &message)
{
    QProcess::startDetached(QStringLiteral("notify-send"), {
        QStringLiteral("--app-name=Zethropol Control Center"),
        title,
        message
    });
}
