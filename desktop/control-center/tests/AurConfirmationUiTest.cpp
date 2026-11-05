#include <QTest>
#include <QObject>
#include <QSignalSpy>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QScopedPointer>
#include <QFile>
#include <QUrl>

class FakeAurBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool aurInstallReady MEMBER aurInstallReady NOTIFY readinessChanged)
public:
    bool aurInstallReady = true;
    int calls = 0;
    bool approved = false;
    QString lastId;
signals:
    void readinessChanged();
public:
    Q_INVOKABLE void confirmAurInstall(const QString &, bool accept) {
        ++calls;
        approved = accept;
    }
    Q_INVOKABLE void confirmAurCancellation(const QString &id, bool accept) {
        ++calls;
        lastId = id;
        approved = accept;
    }
    Q_INVOKABLE void confirmLocalAurCancellation(const QString &id, bool accept) {
        ++calls;
        lastId = id;
        approved = accept;
    }
};

class AurConfirmationUiTest : public QObject {
    Q_OBJECT
private slots:
    void consentRequiresExplicitApproval() {
        QFile file(QStringLiteral(AUR_INSTALL_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(file.readAll(), QUrl::fromLocalFile(file.fileName()));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        FakeAurBridge bridge;
        QQuickWindow window;
        QScopedPointer<QObject> dialog(component.createWithInitialProperties({
            {QStringLiteral("updateBridge"), QVariant::fromValue(&bridge)},
            {QStringLiteral("parent"), QVariant::fromValue(window.contentItem())},
            {QStringLiteral("receiptHash"), QString(64, QLatin1Char('a'))}
        }));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        window.show();
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        auto *consent = dialog->findChild<QObject *>("aurInstallConsentCheck");
        auto *button = dialog->findChild<QObject *>("confirmAurInstallButton");
        QVERIFY(consent);
        QVERIFY(button);
        QVERIFY(!consent->property("checked").toBool());
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.calls, 1);
        QVERIFY(!bridge.approved);
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        QVERIFY(!consent->property("checked").toBool());
        consent->setProperty("checked", true);
        QCoreApplication::processEvents();
        QVERIFY(button->property("enabled").toBool());
        bridge.aurInstallReady=false;emit bridge.readinessChanged();
        QCoreApplication::processEvents();
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QVERIFY(!bridge.approved);
        bridge.aurInstallReady=true;emit bridge.readinessChanged();
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "reject"));
        QCOMPARE(bridge.calls, 3);
        QVERIFY(!bridge.approved);
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        QVERIFY(!consent->property("checked").toBool());
        consent->setProperty("checked", true);
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.calls, 4);
        QVERIFY(bridge.approved);
    }

    void aurCancellationRequiresOperationIdAndExplicitApproval() {
        QFile file(QStringLiteral(AUR_CANCEL_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(file.readAll(), QUrl::fromLocalFile(file.fileName()));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        FakeAurBridge bridge;
        QQuickWindow window;
        QScopedPointer<QObject> dialog(component.createWithInitialProperties({
            {QStringLiteral("updateBridge"), QVariant::fromValue(&bridge)},
            {QStringLiteral("parent"), QVariant::fromValue(window.contentItem())}
        }));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        window.show();
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        auto *button = dialog->findChild<QObject *>("confirmAurCancellationButton");
        QVERIFY(button);
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.calls, 1);
        QVERIFY(!bridge.approved);
        QCOMPARE(bridge.lastId, QString());

        dialog->setProperty("operationId", QStringLiteral("broker-op-42"));
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        QVERIFY(button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "reject"));
        QCOMPARE(bridge.calls, 2);
        QCOMPARE(bridge.lastId, QStringLiteral("broker-op-42"));
        QVERIFY(!bridge.approved);

        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.calls, 3);
        QCOMPARE(bridge.lastId, QStringLiteral("broker-op-42"));
        QVERIFY(bridge.approved);
    }

    void localAurCancellationRequiresExecutionIdAndExplicitApproval() {
        QFile file(QStringLiteral(AUR_LOCAL_CANCEL_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(file.readAll(), QUrl::fromLocalFile(file.fileName()));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        FakeAurBridge bridge;
        QQuickWindow window;
        QScopedPointer<QObject> dialog(component.createWithInitialProperties({
            {QStringLiteral("updateBridge"), QVariant::fromValue(&bridge)},
            {QStringLiteral("parent"), QVariant::fromValue(window.contentItem())}
        }));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        window.show();
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        auto *button = dialog->findChild<QObject *>("confirmLocalAurCancellationButton");
        QVERIFY(button);
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.calls, 1);
        QVERIFY(!bridge.approved);
        QCOMPARE(bridge.lastId, QString());

        dialog->setProperty("executionId", QStringLiteral("local-build-17"));
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QCoreApplication::processEvents();
        QVERIFY(button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "reject"));
        QCOMPARE(bridge.calls, 2);
        QCOMPARE(bridge.lastId, QStringLiteral("local-build-17"));
        QVERIFY(!bridge.approved);

        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.calls, 3);
        QCOMPARE(bridge.lastId, QStringLiteral("local-build-17"));
        QVERIFY(bridge.approved);
    }
};

QTEST_MAIN(AurConfirmationUiTest)
#include "AurConfirmationUiTest.moc"
