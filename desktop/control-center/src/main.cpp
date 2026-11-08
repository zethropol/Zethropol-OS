#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCoreApplication>

#include "HardwareServiceClient.h"
#include "UpdateScanner.h"
#include "UpdateInstaller.h"
#include "RecoveryService.h"
#include "SecurityService.h"
#include "ServiceService.h"
#include "ApplicationService.h"
#include "NotificationService.h"
#include "core/zethropol_power_client.h"
#include "core/zethropol_monitoring_client.h"

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME")
        && qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':')).contains(
            QStringLiteral("KDE"), Qt::CaseInsensitive)) {
        qputenv("QT_QPA_PLATFORMTHEME", QByteArrayLiteral("kde"));
    }
    QGuiApplication app(argc, argv);

    HardwareServiceClient hardwareServiceClient;
    UpdateScanner updateScanner;
    UpdateInstaller updateInstaller(&updateScanner);
    RecoveryService recoveryService;
    SecurityService securityService;
    ServiceService serviceService;
    ApplicationService applicationService;
    NotificationService notificationService;
    ZethropolPowerClient powerClient;
    ZethropolMonitoringClient monitoringClient;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("HardwareBridge"), &hardwareServiceClient);
    engine.rootContext()->setContextProperty(QStringLiteral("UpdateBridge"), &updateScanner);
    engine.rootContext()->setContextProperty(QStringLiteral("UpdateInstallBridge"), &updateInstaller);
    engine.rootContext()->setContextProperty(QStringLiteral("RecoveryBridge"), &recoveryService);
    engine.rootContext()->setContextProperty(QStringLiteral("SecurityBridge"), &securityService);
    engine.rootContext()->setContextProperty(QStringLiteral("ServiceBridge"), &serviceService);
    engine.rootContext()->setContextProperty(QStringLiteral("ApplicationBridge"), &applicationService);
    engine.rootContext()->setContextProperty(QStringLiteral("NotificationBridge"), &notificationService);
    engine.rootContext()->setContextProperty(QStringLiteral("PowerBridge"), &powerClient);
    engine.rootContext()->setContextProperty(QStringLiteral("MonitoringBridge"), &monitoringClient);

    const QUrl url(QStringLiteral("qrc:/Zethropol/ControlCenter/qml/Main.qml"));

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
