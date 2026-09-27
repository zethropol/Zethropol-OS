#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QVariantMap>

class SignalReceiver : public QObject
{
    Q_OBJECT

public slots:
    void statusChanged(const QString &status)
    {
        qInfo() << "Zethropol status changed:" << status;
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QDBusInterface service(
        QStringLiteral("org.zethropol.Prototype"),
        QStringLiteral("/org/zethropol/Prototype"),
        QString(),
        QDBusConnection::sessionBus());

    if (!service.isValid()) {
        qCritical() << "D-Bus interface unavailable:" << service.lastError().message();
        return 1;
    }

    const QDBusReply<QString> reply =
        service.call(QStringLiteral("serviceStatus"));

    if (!reply.isValid()) {
        qCritical() << "D-Bus call failed:" << reply.error().message();
        return 2;
    }

    qInfo() << "Zethropol service status:" << reply.value();

    const QDBusReply<QVariantMap> infoReply =
        service.call(QStringLiteral("serviceInfo"));

    if (!infoReply.isValid()) {
        qCritical() << "D-Bus serviceInfo call failed:" << infoReply.error().message();
        return 3;
    }

    qInfo() << "Zethropol service info:" << infoReply.value();

    const QDBusReply<QString> actionReply =
        service.call(QStringLiteral("performAction"), QStringLiteral("refresh"));

    if (!actionReply.isValid()) {
        qCritical() << "D-Bus ACTION failed:" << actionReply.error().message();
        return 5;
    }

    qInfo() << "Zethropol ACTION result:" << actionReply.value();

    const QDBusReply<QString> adminReply =
        service.call(QStringLiteral("performAdminAction"), QStringLiteral("system-refresh"));

    if (!adminReply.isValid()) {
        qCritical() << "D-Bus ADMIN failed:" << adminReply.error().message();
        return 6;
    }

    qInfo() << "Zethropol ADMIN result:" << adminReply.value();

    SignalReceiver receiver;

    const bool connected = QDBusConnection::sessionBus().connect(
        QStringLiteral("org.zethropol.Prototype"),
        QStringLiteral("/org/zethropol/Prototype"),
        QString(),
        QStringLiteral("statusChanged"),
        &receiver,
        SLOT(statusChanged(QString)));

    if (!connected) {
        qCritical() << "D-Bus signal connection failed";
        return 4;
    }

    return app.exec();
}

#include "client.moc"
