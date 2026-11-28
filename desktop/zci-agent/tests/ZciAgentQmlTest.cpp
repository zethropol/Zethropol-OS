#include "AgentClient.h"

#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

namespace {
QQuickItem *findVisual(QQuickItem *root, const QString &name)
{
    if (!root) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    const auto children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findVisual(child, name)) {
            return found;
        }
    }
    return nullptr;
}

QStringList collectWarnings;
QtMessageHandler prevHandler = nullptr;

void captureQtWarnings(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        collectWarnings.append(msg);
    }
    if (prevHandler) {
        prevHandler(type, ctx, msg);
    }
}
} // namespace

class ZciAgentQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void initialSafeDefaults()
    {
        AgentClient client(QStringLiteral("unix:path=/nonexistent-zci-agent-test-bus"));
        QCOMPARE(client.rawShellEnabled(), false);
        QCOMPARE(client.loopbackOnly(), true);
        QCOMPARE(client.rollbackLocked(), true);
        QCOMPARE(client.repairLocked(), true);
        QCOMPARE(client.activeProfile(), QStringLiteral("zekans-code"));

        client.setActiveProfile(QStringLiteral("zci-command"));
        QCOMPARE(client.activeProfile(), QStringLiteral("zci-command"));
        client.setActiveProfile(QStringLiteral("invalid-profile"));
        QCOMPARE(client.activeProfile(), QStringLiteral("zekans-code"));
    }

    void staticSecurityScanNoNetworkNoShellProcess()
    {
        const QString srcDir = QStringLiteral(ZCI_AGENT_SOURCE_DIR);
        const QStringList files = {
            srcDir + QStringLiteral("/CMakeLists.txt"),
            srcDir + QStringLiteral("/src/AgentClient.h"),
            srcDir + QStringLiteral("/src/AgentClient.cpp"),
            srcDir + QStringLiteral("/src/main.cpp"),
            srcDir + QStringLiteral("/qml/Main.qml"),
        };
        const QStringList forbidden = {
            QStringLiteral("Qt6::Network"),
            QStringLiteral("QNetworkAccessManager"),
            QStringLiteral("QProcess"),
            QStringLiteral("system("),
            QStringLiteral("popen("),
        };
        for (const QString &path : files) {
            QFile f(path);
            QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(path));
            const QString body = QString::fromUtf8(f.readAll());
            for (const QString &needle : forbidden) {
                QVERIFY2(!body.contains(needle),
                         qPrintable(QStringLiteral("Forbidden token '%1' found in %2").arg(needle, path)));
            }
        }
    }

    void mainQmlLoadsWithZeroWarningsDisplaysRiskAndSecurityBadges()
    {
        collectWarnings.clear();
        prevHandler = qInstallMessageHandler(captureQtWarnings);

        AgentClient client(QStringLiteral("unix:path=/nonexistent-zci-agent-test-bus"));
        QVariantList steps;
        steps.append(QVariantMap{
            {QStringLiteral("step_id"), QStringLiteral("step-1")},
            {QStringLiteral("action_kind"), QStringLiteral("project_analysis")},
            {QStringLiteral("agent_id"), QStringLiteral("zekans-code")},
            {QStringLiteral("target"), QStringLiteral("/home/user/Projects/demo")},
            {QStringLiteral("description"), QStringLiteral("Salt-okunur proje analizi")},
            {QStringLiteral("risk_level"), QStringLiteral("ReadOnly")},
            {QStringLiteral("requires_approval"), false},
            {QStringLiteral("approval_status"), QStringLiteral("auto-readonly")},
            {QStringLiteral("execution_status"), QStringLiteral("completed")},
            {QStringLiteral("output_preview"), QStringLiteral("12 dosya tarandı")},
        });
        steps.append(QVariantMap{
            {QStringLiteral("step_id"), QStringLiteral("step-2")},
            {QStringLiteral("action_kind"), QStringLiteral("create_workspace_file")},
            {QStringLiteral("agent_id"), QStringLiteral("zekans-code")},
            {QStringLiteral("target"), QStringLiteral("/home/user/Projects/demo/rapor.txt")},
            {QStringLiteral("description"), QStringLiteral("Onaylı rapor dosyası oluşturma")},
            {QStringLiteral("risk_level"), QStringLiteral("ApprovalRequired")},
            {QStringLiteral("requires_approval"), true},
            {QStringLiteral("approval_status"), QStringLiteral("pending")},
            {QStringLiteral("execution_status"), QStringLiteral("planned")},
            {QStringLiteral("output_preview"), QString()},
        });

        QVariantList audit;
        audit.append(QVariantMap{
            {QStringLiteral("seq"), 1},
            {QStringLiteral("event_type"), QStringLiteral("plan_proposed")},
            {QStringLiteral("outcome"), QStringLiteral("awaiting-approval")},
            {QStringLiteral("detail"), QStringLiteral("steps=2 approval_required=1")},
            {QStringLiteral("entry_sha256"), QStringLiteral("abcdef1234567890")},
        });

        client.setTestState(
            steps,
            audit,
            QStringLiteral("Recovery.v1.GetState doğrulandı (rollback=false, repair=false)."),
            QStringLiteral("plan-test-1"),
            QStringLiteral("awaiting-approval"));

        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("agentClient"), &client);
        engine.load(QUrl(QStringLiteral("qrc:/test-qml/qml/Main.qml")));
        QCoreApplication::processEvents();

        qInstallMessageHandler(prevHandler);
        prevHandler = nullptr;

        QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to load");
        QVERIFY2(collectWarnings.isEmpty(),
                 qPrintable(QStringLiteral("QML warnings: %1").arg(collectWarnings.join(QStringLiteral(" | ")))));

        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(win != nullptr);
        QQuickItem *content = win->contentItem();
        QVERIFY(findVisual(content, QStringLiteral("securityBadge_noShell")) != nullptr);
        QVERIFY(findVisual(content, QStringLiteral("securityBadge_loopback")) != nullptr);
        QVERIFY(findVisual(content, QStringLiteral("securityBadge_recoveryLock")) != nullptr);
        QVERIFY(findVisual(content, QStringLiteral("securityBadge_auditChain")) != nullptr);
        QVERIFY(findVisual(content, QStringLiteral("snapperBanner")) != nullptr);
        QVERIFY(findVisual(content, QStringLiteral("auditTrailList")) != nullptr);
        QVERIFY(findVisual(content, QStringLiteral("stepsListView")) != nullptr);
    }

    void failClosedWhenBusUnavailable()
    {
        AgentClient client(QStringLiteral("unix:path=/nonexistent-zci-agent-test-bus"));
        const QVariantMap st = client.callStatusSync();
        QCOMPARE(st.value(QStringLiteral("success")).toBool(), false);
        QCOMPARE(st.value(QStringLiteral("code")).toString(), QStringLiteral("dbus-unavailable"));
    }

    void desktopEntryHasValidFields()
    {
        const QString desktopPath = QStringLiteral(ZCI_AGENT_SOURCE_DIR) + QStringLiteral("/data/zci-agent.desktop");
        QFile f(desktopPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString body = QString::fromUtf8(f.readAll());
        QVERIFY(body.contains(QStringLiteral("Exec=/usr/bin/zci-agent")));
        QVERIFY(body.contains(QStringLiteral("Icon=zethropol-zci-agent")));
        QVERIFY(body.contains(QStringLiteral("zci-code")));
    }
};

QTEST_MAIN(ZciAgentQmlTest)
#include "ZciAgentQmlTest.moc"
