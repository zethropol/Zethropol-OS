#include "ChatClient.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>

#include <cstdio>

namespace {

bool shouldUseCliMode(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--cli") || arg == QLatin1String("--status")
            || arg == QLatin1String("--send") || arg == QLatin1String("--history")
            || arg == QLatin1String("--clear") || arg == QLatin1String("--help")
            || arg == QLatin1String("-h") || arg == QLatin1String("--version")
            || arg == QLatin1String("-v")) {
            return true;
        }
    }
    return false;
}

int runCliMode(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Zethropol"));
    QCoreApplication::setApplicationName(QStringLiteral("zci-chat"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Zethropol OS ZCI Chatbot — Yerel, Loopback-Only, İzole AI Sohbet İstemcisi"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption cliOpt(QStringLiteral("cli"),
                              QStringLiteral("Etkileşimli terminal sohbet kipini başlat"));
    QCommandLineOption statusOpt(QStringLiteral("status"),
                                 QStringLiteral("ZCI Chatbot, Runtime ve güvenlik durumunu JSON olarak yazdır"));
    QCommandLineOption sendOpt(QStringLiteral("send"),
                               QStringLiteral("Tek bir yerel sohbet iletisi gönder"),
                               QStringLiteral("mesaj"));
    QCommandLineOption historyOpt(QStringLiteral("history"),
                                  QStringLiteral("Seçili oturumun sohbet geçmişini göster"));
    QCommandLineOption clearOpt(QStringLiteral("clear"),
                                QStringLiteral("Seçili oturum (veya '*' ile tüm) geçmişini temizle"));
    QCommandLineOption sessionOpt(QStringLiteral("session"),
                                  QStringLiteral("Hedef sohbet oturum kimliği"),
                                  QStringLiteral("session-id"));
    QCommandLineOption sysCtxOpt(QStringLiteral("system-context"),
                                 QStringLiteral("Açık onaylı salt-okunur sistem bağlamını ekle"));
    QCommandLineOption maxTokOpt(QStringLiteral("max-tokens"),
                                 QStringLiteral("Çıktı token üst sınırı"),
                                 QStringLiteral("tokens"),
                                 QStringLiteral("2048"));
    QCommandLineOption busOpt(QStringLiteral("bus-address"),
                              QStringLiteral("Özel D-Bus adresi (test/yalıtılmış oturum)"),
                              QStringLiteral("address"));

    parser.addOptions({cliOpt, statusOpt, sendOpt, historyOpt, clearOpt, sessionOpt, sysCtxOpt, maxTokOpt, busOpt});
    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    zethropol::chat::ChatClient client;
    if (!client.connectToBus(parser.value(busOpt))) {
        err << "Hata: D-Bus oturumuna bağlanılamadı.\n";
        return 2;
    }

    if (parser.isSet(statusOpt)) {
        const QVariantMap env = client.callGetChatStatus();
        out << QString::fromUtf8(QJsonDocument::fromVariant(env).toJson(QJsonDocument::Indented));
        return env.value(QStringLiteral("success")).toBool() ? 0 : 1;
    }

    QString sid = parser.value(sessionOpt).trimmed();

    if (parser.isSet(clearOpt)) {
        const QString target = sid.isEmpty() ? QStringLiteral("*") : sid;
        const QVariantMap env = client.callClearSession(target);
        out << QString::fromUtf8(QJsonDocument::fromVariant(env).toJson(QJsonDocument::Indented));
        return env.value(QStringLiteral("success")).toBool() ? 0 : 1;
    }

    if (parser.isSet(historyOpt)) {
        if (sid.isEmpty()) {
            const QVariantMap listed = client.callListSessions();
            const QStringList raw = listed.value(QStringLiteral("data"))
                                        .toMap()
                                        .value(QStringLiteral("sessions_json"))
                                        .toStringList();
            if (!raw.isEmpty()) {
                sid = QJsonDocument::fromJson(raw.last().toUtf8())
                          .object()
                          .value(QStringLiteral("session_id"))
                          .toString();
            }
        }
        const QVariantMap env = client.callGetSessionHistory(sid);
        out << QString::fromUtf8(QJsonDocument::fromVariant(env).toJson(QJsonDocument::Indented));
        return env.value(QStringLiteral("success")).toBool() ? 0 : 1;
    }

    const bool useSysCtx = parser.isSet(sysCtxOpt);
    const quint32 maxTok = static_cast<quint32>(qBound(1, parser.value(maxTokOpt).toInt(), 4096));

    if (parser.isSet(sendOpt)) {
        if (sid.isEmpty()) {
            const QVariantMap created =
                client.callCreateSession(QStringLiteral("CLI Sohbeti"), useSysCtx);
            if (!created.value(QStringLiteral("success")).toBool()) {
                out << QString::fromUtf8(QJsonDocument::fromVariant(created).toJson(QJsonDocument::Indented));
                return 1;
            }
            sid = created.value(QStringLiteral("data"))
                      .toMap()
                      .value(QStringLiteral("session_id"))
                      .toString();
        }
        const QVariantMap res = client.callSendMessage(sid, parser.value(sendOpt), useSysCtx, maxTok);
        out << QString::fromUtf8(QJsonDocument::fromVariant(res).toJson(QJsonDocument::Indented));
        return res.value(QStringLiteral("success")).toBool() ? 0 : 1;
    }

    // Interactive `--cli` REPL mode
    if (sid.isEmpty()) {
        const QVariantMap created =
            client.callCreateSession(QStringLiteral("Terminal Sohbeti"), useSysCtx);
        if (!created.value(QStringLiteral("success")).toBool()) {
            err << "Oturum açılamadı: " << created.value(QStringLiteral("message")).toString() << "\n";
            return 1;
        }
        sid = created.value(QStringLiteral("data"))
                  .toMap()
                  .value(QStringLiteral("session_id"))
                  .toString();
    }

    out << "Zethropol ZCI Chatbot CLI (oturum: " << sid << ")\n";
    out << "Çıkmak için '/quit', geçmişi temizlemek için '/clear', durum için '/status' yazın.\n";
    out.flush();

    QTextStream in(stdin);
    while (!in.atEnd()) {
        out << "zci> ";
        out.flush();
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line == QLatin1String("/quit") || line == QLatin1String("/exit")) {
            break;
        }
        if (line == QLatin1String("/status")) {
            const QVariantMap st = client.callGetChatStatus();
            const QVariantMap d = st.value(QStringLiteral("data")).toMap();
            out << "[Durum] profil=" << d.value(QStringLiteral("hardware_profile")).toString()
                << " motor=" << d.value(QStringLiteral("runtime_state")).toString()
                << " vram=" << d.value(QStringLiteral("active_vram_mb")).toULongLong() << " MiB"
                << " komut_yurutme=kapali telemetri=kapali\n";
            out.flush();
            continue;
        }
        if (line == QLatin1String("/clear")) {
            client.callClearSession(sid);
            out << "[Bilgi] Sohbet geçmişi temizlendi.\n";
            out.flush();
            continue;
        }

        const QVariantMap res = client.callSendMessage(sid, line, useSysCtx, maxTok);
        if (!res.value(QStringLiteral("success")).toBool()) {
            out << "[Engellendi / Hata] (" << res.value(QStringLiteral("code")).toString() << ") "
                << res.value(QStringLiteral("message")).toString() << "\n";
        } else if (res.value(QStringLiteral("code")).toString() == QLatin1String("passive-standby")) {
            out << "[Pasif Bekleme] " << res.value(QStringLiteral("message")).toString() << "\n";
        } else {
            const QVariantMap d = res.value(QStringLiteral("data")).toMap();
            out << d.value(QStringLiteral("reply_text")).toString() << "\n";
        }
        out.flush();
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (shouldUseCliMode(argc, argv)) {
        return runCliMode(argc, argv);
    }

    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Zethropol"));
    QCoreApplication::setApplicationName(QStringLiteral("ZCI Chatbot"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setDesktopFileName(QStringLiteral("zci-chat"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/zethropol-zci-chat.svg")));

    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    QString customBus;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QLatin1String("--bus-address")) {
            customBus = args[i + 1];
        }
    }

    zethropol::chat::ChatClient chatClient;
    chatClient.connectToBus(customBus);
    chatClient.refreshStatus();
    chatClient.refreshSessions();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("ChatBridge"), &chatClient);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Zethropol.ZciChat"), QStringLiteral("Main"));
    return app.exec();
}
