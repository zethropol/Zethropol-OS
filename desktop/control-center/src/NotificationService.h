#pragma once

#include <QObject>

class NotificationService : public QObject
{
    Q_OBJECT

public:
    explicit NotificationService(QObject *parent = nullptr);

    Q_INVOKABLE void send(const QString &title, const QString &message);
};
