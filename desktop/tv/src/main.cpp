#include "ChannelManager.h"
#include "EpgManager.h"
#include "MpvItem.h"
#include "StreamResolver.h"

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQml>

#include <clocale>

int main(int argc, char *argv[])
{
    // libmpv OpenGL render context requires OpenGL Qt Quick backend on Wayland/X11,
    // while keeping offscreen/software test overrides intact.
    if (qEnvironmentVariable("QT_QUICK_BACKEND") != QStringLiteral("software")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }

    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Zethropol"));
    QCoreApplication::setApplicationName(QStringLiteral("Zethropol TV"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.zethropol.tv"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/zethropol-tv-256.png")));

    // Mandatory for libmpv initialization.
    std::setlocale(LC_NUMERIC, "C");

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    qmlRegisterType<zethropol::tv::MpvItem>("Zethropol.TV", 1, 0, "MpvItem");

    zethropol::tv::ChannelManager channelManager;
    zethropol::tv::EpgManager epgManager;
    zethropol::tv::StreamResolver streamResolver;

    QObject::connect(&channelManager, &zethropol::tv::ChannelManager::channelSelected,
                     &epgManager,
                     [&epgManager](const QString & /*streamUrl*/, const QString &name, const QString &tvgId) {
                         epgManager.setActiveChannel(tvgId, name);
                     });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("ChannelBridge"), &channelManager);
    engine.rootContext()->setContextProperty(QStringLiteral("EpgBridge"), &epgManager);
    engine.rootContext()->setContextProperty(QStringLiteral("StreamResolverBridge"), &streamResolver);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Zethropol.TV"), QStringLiteral("Main"));

    // Auto-select initial channel (last selected or first available) once QML is ready.
    if (!engine.rootObjects().isEmpty()) {
        if (!channelManager.currentChannelId().isEmpty()) {
            channelManager.selectChannelById(channelManager.currentChannelId());
        } else if (channelManager.filteredChannelCount() > 0) {
            channelManager.selectChannel(0);
        }
    }

    return app.exec();
}
