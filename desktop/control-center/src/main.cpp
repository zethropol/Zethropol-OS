#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCoreApplication>

#include "HardwareServiceClient.h"
#include "UpdateService.h"
#include "RecoveryService.h"
#include "SecurityService.h"
#include "ServiceService.h"
#include "ApplicationService.h"
#include "NotificationService.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    HardwareServiceClient hardwareServiceClient;
    UpdateService updateService;
    RecoveryService recoveryService;
    SecurityService securityService;
    ServiceService serviceService;
    ApplicationService applicationService;
    NotificationService notificationService;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("HardwareBridge"), &hardwareServiceClient);
    engine.rootContext()->setContextProperty(QStringLiteral("UpdateBridge"), &updateService);
    engine.rootContext()->setContextProperty(QStringLiteral("RecoveryBridge"), &recoveryService);
    engine.rootContext()->setContextProperty(QStringLiteral("SecurityBridge"), &securityService);
    engine.rootContext()->setContextProperty(QStringLiteral("ServiceBridge"), &serviceService);
    engine.rootContext()->setContextProperty(QStringLiteral("ApplicationBridge"), &applicationService);
    engine.rootContext()->setContextProperty(QStringLiteral("NotificationBridge"), &notificationService);

    const QUrl url(QStringLiteral("qrc:/qt/qml/Zethropol/ControlCenter/qml/Main.qml"));

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
