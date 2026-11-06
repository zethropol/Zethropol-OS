#include <QTimer>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#define main aurSourceApplicationMain
#include "/opt/aur-acceptance/source/control-center/src/main.cpp"
#undef main
static void scheduleGuestProbe()
{
    QTimer::singleShot(12000, [] {
        QFile marker("/etc/aur-vm-evidence/uuid");
        if (!marker.open(QIODevice::ReadOnly) || marker.readAll().trimmed() != "c98f6c9a-a516-45f3-bfe9-ad78c712b54b") {
            QCoreApplication::exit(91); return;
        }
        for (QWindow *window : QGuiApplication::topLevelWindows()) {
            auto *context = QQmlEngine::contextForObject(window);
            if (!context) continue;
            auto *bridge = qobject_cast<UpdateService *>(context->contextProperty("UpdateBridge").value<QObject *>());
            if (!bridge) continue;
            QSignalSpy confirmations(bridge, &UpdateService::aurInstallConfirmationRequested);
            bool before = bridge->aurInstallReady();
            bridge->requestAurInstallConfirmation();
            bool after = bridge->aurInstallReady();
            QJsonObject result{{"scope", "real guest Qt/QML and system bus; closed-admission case"},
                {"qml_window_created", true}, {"install_ready_before", before},
                {"install_ready_after", after}, {"install_confirmations", confirmations.count()},
                {"aur_status", bridge->aurStatus()}, {"aur_count", bridge->aurCount()},
                {"aur_scan_warning", bridge->aurScanWarning()}};
            qInfo().noquote() << "AUR_GUEST_UI_RESULT" << QJsonDocument(result).toJson(QJsonDocument::Compact);
            if (auto *quick = qobject_cast<QQuickWindow *>(window))
                quick->grabWindow().save("/home/aur-acceptance/control-center-closed.png");
            QCoreApplication::exit(before || after || confirmations.count() ? 92 : 0);
            return;
        }
        qCritical("AUR_GUEST_UI_NO_WINDOW_OR_BRIDGE");
        QCoreApplication::exit(93);
    });
}
Q_COREAPP_STARTUP_FUNCTION(scheduleGuestProbe)
int main(int argc, char **argv) { return aurSourceApplicationMain(argc, argv); }
