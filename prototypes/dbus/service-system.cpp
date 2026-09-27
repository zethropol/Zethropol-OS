#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

class ZethropolService : public QObject
{
    Q_OBJECT

public:
    explicit ZethropolService(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    QString serviceStatus() const
    {
        return QStringLiteral("ready");
    }

    QVariantMap serviceInfo() const
    {
        return {
            {QStringLiteral("status"), QStringLiteral("ready")},
            {QStringLiteral("name"), QStringLiteral("Zethropol Prototype Service")},
            {QStringLiteral("version"), QStringLiteral("0.1")}
        };
    }

    QString performAction(const QString &action)
    {
        if (action.isEmpty())
            return QStringLiteral("invalid-action");

        if (action == QStringLiteral("refresh"))
            return QStringLiteral("action-accepted");

        return QStringLiteral("unsupported-action");
    }

    QString performAdminAction(const QString &action)
    {
        if (action == QStringLiteral("system-refresh"))
            return QStringLiteral("authorization-required");

        return QStringLiteral("unsupported-admin-action");
    }

Q_SIGNALS:
    void statusChanged(const QString &status);
};

class ZethropolServiceAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.zethropol.Service")

public:
    explicit ZethropolServiceAdaptor(ZethropolService *service)
        : QDBusAbstractAdaptor(service),
          m_service(service)
    {
        connect(m_service, &ZethropolService::statusChanged,
                this, &ZethropolServiceAdaptor::statusChanged);
    }

public Q_SLOTS:
    QString serviceStatus() const
    {
        return m_service->serviceStatus();
    }

    QVariantMap serviceInfo() const
    {
        return m_service->serviceInfo();
    }

    QString performAction(const QString &action)
    {
        return m_service->performAction(action);
    }

    QString performAdminAction(const QString &action)
    {
        return m_service->performAdminAction(action);
    }

Q_SIGNALS:
    void statusChanged(const QString &status);

private:
    ZethropolService *m_service;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    ZethropolService service;
    ZethropolServiceAdaptor adaptor(&service);

    auto bus = QDBusConnection::systemBus();

    if (!bus.registerService(QStringLiteral("org.zethropol.SystemPrototype"))) {
        qWarning() << "D-Bus registerService failed:" << bus.lastError().name() << bus.lastError().message();
        return 1;
    }

    if (!bus.registerObject(
            QStringLiteral("/org/zethropol/Prototype"),
            &service,
            QDBusConnection::ExportAdaptors))
        return 2;

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&service]() {
        static bool ready = true;
        ready = !ready;
        emit service.statusChanged(
            ready ? QStringLiteral("ready") : QStringLiteral("busy"));
    });
    timer.start(2000);

    return app.exec();
}


#include "service-system.moc"
