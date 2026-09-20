#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QCoreApplication>

#include "HardwareServiceClient.h"
#include "UpdateService.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    HardwareServiceClient hardwareServiceClient;
    UpdateService updateService;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("HardwareBridge"), &hardwareServiceClient);
    engine.rootContext()->setContextProperty(QStringLiteral("UpdateBridge"), &updateService);

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
