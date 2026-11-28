#include "AgentClient.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>

namespace {
void printJson(const QVariantMap &map)
{
    QTextStream out(stdout);
    out << QString::fromUtf8(
        QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Indented));
    out.flush();
}

void printHelp()
{
    QTextStream out(stdout);
    out << "Zethropol ZCI Komut Ajanı & ZEKANS (v0.1.0)\n"
        << "Kullanım:\n"
        << "  zci-agent                                          Masaüstü Qt6/QML onay arayüzünü aç\n"
        << "  zci-agent --status [--bus-address <addr>]          Servis, kilit ve denetim zinciri durumu\n"
        << "  zci-agent --plan \"<görev>\" [--profile <p>] [--workspace <d>]\n"
        << "                                                     Güvenli allowlist adım planı öner\n"
        << "  zci-agent --approve <plan_id> <step_id> [--reject] [--reason \"...\"]\n"
        << "                                                     Adım onay veya red kararı kaydet\n"
        << "  zci-agent --execute <plan_id>                      Yalnızca onaylı planı zci-daemon üzerinden yürüt\n"
        << "  zci-agent --cancel <plan_id> [--reason \"...\"]      Planı iptal et\n"
        << "  zci-agent --plan-status <plan_id>                  Plan adımlarını ve denetim durumunu göster\n"
        << "  zci-agent --audit [--limit N]                      Hash-zincirli denetim günlüğünü doğrula ve listele\n";
    out.flush();
}

bool wantsHeadlessCli(const QStringList &args)
{
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QStringLiteral("--cli")
            || a == QStringLiteral("--status")
            || a == QStringLiteral("--plan")
            || a == QStringLiteral("--approve")
            || a == QStringLiteral("--execute")
            || a == QStringLiteral("--cancel")
            || a == QStringLiteral("--plan-status")
            || a == QStringLiteral("--audit")
            || a == QStringLiteral("--help")
            || a == QStringLiteral("-h")) {
            return true;
        }
    }
    return false;
}

int runCli(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    QString busAddress;
    QString profile = QStringLiteral("zekans-code");
    QString workspace;
    QString reason;
    QString goal;
    QString planId;
    QString stepId;
    bool modeStatus = false;
    bool modePlan = false;
    bool modeApprove = false;
    bool rejectFlag = false;
    bool modeExecute = false;
    bool modeCancel = false;
    bool modePlanStatus = false;
    bool modeAudit = false;
    int auditLimit = 30;

    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QStringLiteral("--help") || a == QStringLiteral("-h")) {
            printHelp();
            return 0;
        } else if (a == QStringLiteral("--cli")) {
            continue;
        } else if (a == QStringLiteral("--status")) {
            modeStatus = true;
        } else if (a == QStringLiteral("--reject")) {
            rejectFlag = true;
        } else if (a == QStringLiteral("--audit")) {
            modeAudit = true;
        } else if (a == QStringLiteral("--bus-address") && i + 1 < args.size()) {
            busAddress = args.at(++i);
        } else if (a == QStringLiteral("--profile") && i + 1 < args.size()) {
            profile = args.at(++i);
        } else if (a == QStringLiteral("--workspace") && i + 1 < args.size()) {
            workspace = args.at(++i);
        } else if (a == QStringLiteral("--reason") && i + 1 < args.size()) {
            reason = args.at(++i);
        } else if (a == QStringLiteral("--limit") && i + 1 < args.size()) {
            auditLimit = args.at(++i).toInt();
        } else if (a == QStringLiteral("--plan") && i + 1 < args.size()) {
            modePlan = true;
            goal = args.at(++i);
        } else if (a == QStringLiteral("--approve") && i + 2 < args.size()) {
            modeApprove = true;
            planId = args.at(++i);
            stepId = args.at(++i);
        } else if (a == QStringLiteral("--execute") && i + 1 < args.size()) {
            modeExecute = true;
            planId = args.at(++i);
        } else if (a == QStringLiteral("--cancel") && i + 1 < args.size()) {
            modeCancel = true;
            planId = args.at(++i);
        } else if (a == QStringLiteral("--plan-status") && i + 1 < args.size()) {
            modePlanStatus = true;
            planId = args.at(++i);
        } else {
            QTextStream err(stderr);
            err << "Bilinmeyen veya eksik argüman: " << a << "\n";
            return 2;
        }
    }

    AgentClient client(busAddress);
    QVariantMap reply;
    if (modePlan) {
        reply = client.callProposePlanSync(goal, profile, workspace);
    } else if (modeApprove) {
        reply = client.callApproveStepSync(planId, stepId, !rejectFlag, reason);
    } else if (modeExecute) {
        reply = client.callExecutePlanSync(planId);
    } else if (modeCancel) {
        reply = client.callCancelPlanSync(planId, reason);
    } else if (modePlanStatus) {
        reply = client.callGetPlanStatusSync(planId);
    } else if (modeAudit) {
        reply = client.callAuditTrailSync(auditLimit);
    } else if (modeStatus || true) {
        reply = client.callStatusSync();
    }

    printJson(reply);
    return reply.value(QStringLiteral("success")).toBool() ? 0 : 1;
}
} // namespace

int main(int argc, char *argv[])
{
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        rawArgs.append(QString::fromLocal8Bit(argv[i]));
    }
    if (wantsHeadlessCli(rawArgs)) {
        return runCli(argc, argv);
    }

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("zci-agent"));
    app.setApplicationDisplayName(QStringLiteral("Zethropol ZCI Komut Ajanı & ZEKANS"));
    app.setOrganizationName(QStringLiteral("Zethropol"));
    app.setWindowIcon(QIcon(QStringLiteral(":/zci-agent/data/zethropol-zci-agent.svg")));

    QString busAddress;
    for (int i = 1; i + 1 < rawArgs.size(); ++i) {
        if (rawArgs.at(i) == QStringLiteral("--bus-address")) {
            busAddress = rawArgs.at(i + 1);
            break;
        }
    }

    AgentClient client(busAddress);
    client.refreshStatus();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("agentClient"), &client);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Zethropol/ZciAgent/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec();
}
