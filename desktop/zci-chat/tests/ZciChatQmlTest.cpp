#include "ChatClient.h"

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtTest>

namespace {

QObject *findVisual(QQuickItem *item, const QString &name)
{
    if (!item) {
        return nullptr;
    }
    if (item->objectName() == name) {
        return item;
    }
    for (QQuickItem *child : item->childItems()) {
        if (QObject *found = findVisual(child, name)) {
            return found;
        }
    }
    return nullptr;
}

QObject *findByObjectName(QObject *root, const QString &name)
{
    if (!root) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    if (QObject *child = root->findChild<QObject *>(name)) {
        return child;
    }
    if (auto *win = qobject_cast<QQuickWindow *>(root)) {
        return findVisual(win->contentItem(), name);
    }
    if (auto *item = qobject_cast<QQuickItem *>(root)) {
        return findVisual(item, name);
    }
    return nullptr;
}

} // namespace

class ZciChatQmlTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }

    void mainQmlLoadsWithZeroWarningsDisplaysSecurityBadgesAndMessages()
    {
        zethropol::chat::ChatClient client;
        QVariantList sampleMsgs;
        QVariantMap u;
        u.insert(QStringLiteral("role"), QStringLiteral("user"));
        u.insert(QStringLiteral("content"), QStringLiteral("Merhaba Zethropol"));
        sampleMsgs.append(u);
        QVariantMap a;
        a.insert(QStringLiteral("role"), QStringLiteral("assistant"));
        a.insert(QStringLiteral("content"), QStringLiteral("Yerel ZCI Chatbot hazır."));
        sampleMsgs.append(a);
        client.applyMockStateForTest(QStringLiteral("Standby"),
                                     QStringLiteral("dev-baseline"),
                                     QStringLiteral("gemma-4-E4B-it-Q4_K_M"),
                                     sampleMsgs);

        QQmlApplicationEngine engine;
        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &errs) { warnings.append(errs); });
        engine.rootContext()->setContextProperty(QStringLiteral("ChatBridge"), &client);
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Zethropol/ZciChat/qml/Main.qml")));

        QVERIFY(!engine.rootObjects().isEmpty());
        QObject *win = engine.rootObjects().first();
        QCOMPARE(win->objectName(), QStringLiteral("zciChatMainWindow"));

        QObject *rtLbl = findByObjectName(win, QStringLiteral("runtimeStateLabel"));
        QVERIFY(rtLbl != nullptr);
        QVERIFY(rtLbl->property("text").toString().contains(QStringLiteral("Standby")));
        QVERIFY(rtLbl->property("text").toString().contains(QStringLiteral("0 MiB VRAM")));

        QObject *hwLbl = findByObjectName(win, QStringLiteral("hardwareProfileLabel"));
        QVERIFY(hwLbl != nullptr);
        QCOMPARE(hwLbl->property("text").toString(), QStringLiteral("dev-baseline"));

        QVERIFY(findByObjectName(win, QStringLiteral("securityBadge_loopback")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("securityBadge_isolated")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("securityBadge_noshell")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("securityBadge_privacy")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("systemContextToggle")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("clearHistoryButton")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("promptInputField")) != nullptr);
        QVERIFY(findByObjectName(win, QStringLiteral("sendButton")) != nullptr);

        QObject *msgView = findByObjectName(win, QStringLiteral("messagesListView"));
        QVERIFY(msgView != nullptr);
        QCOMPARE(msgView->property("count").toInt(), 2);

        QString warnMsg;
        for (const QQmlError &e : warnings) {
            warnMsg += e.toString() + QLatin1Char('\n');
        }
        QVERIFY2(warnings.isEmpty(), qPrintable(warnMsg));
    }

    void qmlAndDesktopFilesMeetStaticSecurityAndPackagingContract()
    {
        QFile qmlFile(QStringLiteral(ZCI_CHAT_QML_DIR "/Main.qml"));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly));
        const QString src = QString::fromUtf8(qmlFile.readAll());
        QVERIFY(!src.contains(QStringLiteral("Qt.createQmlObject")));
        QVERIFY(!src.contains(QStringLiteral("QProcess")));
        QVERIFY(!src.contains(QStringLiteral("eval(")));
        QVERIFY(!src.contains(QStringLiteral("XMLHttpRequest")));

        QFile desktopFile(QStringLiteral(ZCI_CHAT_DESKTOP_FILE));
        QVERIFY(desktopFile.open(QIODevice::ReadOnly));
        const QString desktop = QString::fromUtf8(desktopFile.readAll());
        QVERIFY(desktop.contains(QStringLiteral("Exec=zci-chat")));
        QVERIFY(desktop.contains(QStringLiteral("Icon=zethropol-zci-chat")));
    }

    void failClosedWhenCustomBusAddressIsUnreachable()
    {
        zethropol::chat::ChatClient client;
        QVERIFY(!client.connectToBus(QStringLiteral("unix:path=/tmp/nonexistent-zci-chat-test.sock")));
        const QVariantMap st = client.callGetChatStatus();
        QCOMPARE(st.value(QStringLiteral("success")).toBool(), false);
        QCOMPARE(st.value(QStringLiteral("code")).toString(), QStringLiteral("zci-chat-unavailable"));
    }
};

QTEST_MAIN(ZciChatQmlTest)
#include "ZciChatQmlTest.moc"
