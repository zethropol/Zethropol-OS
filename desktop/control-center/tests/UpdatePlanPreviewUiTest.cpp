#include <QFile>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

class FakeUpdateBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString reviewHash READ reviewHash NOTIFY planPreviewChanged)
    Q_PROPERTY(bool blockedSources READ blockedSources NOTIFY planPreviewChanged)
    Q_PROPERTY(bool previewing MEMBER previewing)
    Q_PROPERTY(bool previewReady MEMBER previewReady)
    Q_PROPERTY(bool applicableSources MEMBER applicableSources NOTIFY planPreviewChanged)
    Q_PROPERTY(bool checking MEMBER checking)
    Q_PROPERTY(bool installing MEMBER installing)
    Q_PROPERTY(bool removalApprovalRequired MEMBER removalApprovalRequired NOTIFY planPreviewChanged)
    Q_PROPERTY(QString planHash MEMBER planHash NOTIFY planPreviewChanged)
    Q_PROPERTY(QString planPreview MEMBER planPreview NOTIFY planPreviewChanged)

public:
    QString reviewHash() const { return planHash; }
    bool blocked = false;
    bool blockedSources() const { return blocked; }
    Q_INVOKABLE void confirmSkippedSources(const QString &, bool) {}
    bool previewing = false;
    bool previewReady = true;
    bool applicableSources = true;
    bool checking = false;
    bool installing = false;
    bool removalApprovalRequired = true;
    QString planHash = QString(64, QLatin1Char('a'));
    bool approved = false;
    int installRequests = 0;
    Q_INVOKABLE void confirmRemovals(const QString &hash, bool accept) { approved = accept && hash == planHash; }
    Q_INVOKABLE void install() { ++installRequests; }
    QString planPreview;
    int previewRequests = 0;

    Q_INVOKABLE void previewPlan() { ++previewRequests; }

signals:
    void planPreviewChanged();
};

class UpdatePlanPreviewUiTest : public QObject
{
    Q_OBJECT

private slots:
    void riskyConfirmationStartsUncheckedAndRejectsStalePlan()
    {
        QFile file(QStringLiteral(UPDATE_CONFIRMATION_QML_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(file.readAll(), QUrl::fromLocalFile(file.fileName()));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        FakeUpdateBridge bridge;
        bridge.planPreview = QStringLiteral("REMOVE old-demo; REPLACED BY new-demo");
        QQuickWindow window;
        QScopedPointer<QObject> dialog(component.createWithInitialProperties({
            {QStringLiteral("updateBridge"), QVariant::fromValue(&bridge)},
            {QStringLiteral("parent"), QVariant::fromValue(window.contentItem())},
            {QStringLiteral("width"), 640}, {QStringLiteral("height"), 420}
        }));
        QVERIFY2(dialog, qPrintable(component.errorString()));
        window.show();
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QTest::qWait(200);
        QObject *consent = dialog->findChild<QObject *>(QStringLiteral("replacementRemovalConsent"));
        QVERIFY(consent);
        QVERIFY(!consent->property("checked").toBool());
        QVERIFY(!bridge.approved);
        QObject *ok = dialog->findChild<QObject *>(QStringLiteral("confirmOfficialTransaction"));
        QVERIFY(ok);
        QVERIFY(!ok->property("enabled").toBool());
        consent->setProperty("checked", true);
        QCoreApplication::processEvents();
        QVERIFY(ok->property("enabled").toBool());
        bridge.planHash = QString(64, QLatin1Char('b'));
        emit bridge.planPreviewChanged();
        QCoreApplication::processEvents();
        QVERIFY(!ok->property("enabled").toBool());
        QCOMPARE(bridge.installRequests, 0);
        // Even programmatic acceptance cannot authorize a changed plan.
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QCOMPARE(bridge.installRequests, 0);
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "reject"));
        QVERIFY(!bridge.approved);
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "open"));
        QTest::qWait(200);
        QVERIFY(!consent->property("checked").toBool());
        consent->setProperty("checked", true);
        QCoreApplication::processEvents();
        QVERIFY(QMetaObject::invokeMethod(dialog.data(), "accept"));
        QVERIFY(bridge.approved);
        QCOMPARE(bridge.installRequests, 1);
    }

    void blockedSourceConsentIsNeverImplicit() {
        QFile file(QStringLiteral(UPDATE_CONFIRMATION_QML_PATH));QVERIFY(file.open(QIODevice::ReadOnly));
        QQmlEngine engine;QQmlComponent component(&engine);component.setData(file.readAll(),QUrl::fromLocalFile(file.fileName()));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        FakeUpdateBridge bridge;bridge.blocked=true;bridge.removalApprovalRequired=false;
        QQuickWindow window;QScopedPointer<QObject> dialog(component.createWithInitialProperties({
            {"updateBridge",QVariant::fromValue(&bridge)},{"parent",QVariant::fromValue(window.contentItem())},{"width",760},{"height",620}}));
        QVERIFY(dialog);window.show();QVERIFY(QMetaObject::invokeMethod(dialog.data(),"open"));QTest::qWait(100);
        auto *consent=dialog->findChild<QObject *>("blockedSourceConsent");auto *ok=dialog->findChild<QObject *>("confirmOfficialTransaction");
        QVERIFY(consent);QVERIFY(ok);QVERIFY(!consent->property("checked").toBool());QVERIFY(!ok->property("enabled").toBool());
        consent->setProperty("checked",true);QCoreApplication::processEvents();QVERIFY(ok->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog.data(),"reject"));QVERIFY(QMetaObject::invokeMethod(dialog.data(),"open"));QTest::qWait(100);
        QVERIFY(!consent->property("checked").toBool());QVERIFY(!ok->property("enabled").toBool());QCOMPARE(bridge.installRequests,0);
    }
    void clickingPreviewOpensResizableWindowAndRefreshesPlan()
    {
        QFile qmlFile(QStringLiteral(UPDATE_PLAN_PREVIEW_QML_PATH));
        QVERIFY(qmlFile.open(QIODevice::ReadOnly));

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(qmlFile.readAll(), QUrl::fromLocalFile(qmlFile.fileName()));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        FakeUpdateBridge bridge;
        bridge.planPreview = QStringLiteral("No packages in preview.");
        QObject *root = component.createWithInitialProperties({
            {QStringLiteral("updateBridge"), QVariant::fromValue(&bridge)},
        });
        QVERIFY2(root, qPrintable(component.errorString()));

        QQuickWindow window;
        auto *rootItem = qobject_cast<QQuickItem *>(root);
        QVERIFY(rootItem);
        rootItem->setParentItem(window.contentItem());
        rootItem->setSize(QSizeF(640, 480));
        window.show();
        QCoreApplication::processEvents();

        QObject *button = root->findChild<QObject *>(QStringLiteral("previewPlanButton"));
        auto *dialog = root->findChild<QQuickWindow *>(QStringLiteral("updatePlanWindow"));
        QObject *previewText = root->findChild<QObject *>(QStringLiteral("previewPlanText"));
        QVERIFY(button);
        QVERIFY(dialog);
        QVERIFY(previewText);
        QVERIFY(!dialog->property("visible").toBool());

        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        QCOMPARE(bridge.previewRequests, 1);
        QVERIFY(dialog->property("visible").toBool());
        QCOMPARE(previewText->property("text").toString(), bridge.planPreview);
        QVERIFY(dialog != &window);
        QCOMPARE(dialog->modality(), Qt::NonModal);
        QVERIFY(dialog->contentItem() != window.contentItem());

        bridge.planPreview = QStringLiteral("Risks / warnings / error details\n")
            + QString(12000, QLatin1Char('x'));
        emit bridge.planPreviewChanged();
        dialog->resize(400, 300);
        QCoreApplication::processEvents();
        QCOMPARE(previewText->property("text").toString(), bridge.planPreview);
        auto *textItem = qobject_cast<QQuickItem *>(previewText);
        QVERIFY(textItem);
        QVERIFY(textItem->width() <= dialog->width());
        QTRY_VERIFY(previewText->property("contentHeight").toReal() > dialog->height());
        QObject *scroll = root->findChild<QObject *>(QStringLiteral("previewPlanScroll"));
        QVERIFY(scroll);
        QObject *flickable = scroll->property("contentItem").value<QObject *>();
        QVERIFY(flickable);
        flickable->setProperty("contentY", 100);
        QVERIFY(flickable->property("contentY").toReal() > 0);

        QVERIFY(dialog->close());
        QVERIFY(!dialog->isVisible());
        QVERIFY(window.isVisible());
        bridge.planPreview = QStringLiteral("Updated plan after reopening.");
        emit bridge.planPreviewChanged();
        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        QCOMPARE(bridge.previewRequests, 2);
        QVERIFY(dialog->isVisible());
        QCOMPARE(previewText->property("text").toString(), bridge.planPreview);
        QObject *closeButtons = root->findChild<QObject *>(QStringLiteral("previewPlanCloseButtons"));
        QVERIFY(closeButtons);
        QVERIFY(QMetaObject::invokeMethod(closeButtons, "rejected"));
        QVERIFY(!dialog->isVisible());
        QVERIFY(window.isVisible());
        delete root;

    }
};

QTEST_MAIN(UpdatePlanPreviewUiTest)
#include "UpdatePlanPreviewUiTest.moc"
