#include "PostUpdateFlow.h"
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class FakePostUpdateFlow : public PostUpdateFlow {
public:
    explicit FakePostUpdateFlow(const QString &path) : PostUpdateFlow(nullptr,path) {}
    int reads=0, powerCalls=0;
    bool rebootCheck=false;
    QString requestedId;
    void feed(const QVariantMap &state,bool recheck=false) { deliverState(state,recheck); }
    quint64 generation() const { return requestGeneration(); }
    void feedRequest(const QVariantMap &state, quint64 generation) { deliverState(state,false,generation); }
    void errorRequest(quint64 generation) { deliverReadError("timeout","old request",false,generation); }
    void powerReply(bool success,const QString &code,const QString &message) { deliverRebootReply(success,code,message); }
    void readError(const QString &name) { deliverReadError(name,"fixture error",false); }
protected:
    void fetchState(const QString &id,bool forReboot) override { ++reads; requestedId=id; rebootCheck=forReboot; }
    void callPowerReboot() override { ++powerCalls; }
};
class PostUpdateFlowTest : public QObject {
    Q_OBJECT
    static QVariantMap state(const QString &cleanup="completed",const QString &reboot="required") {
        return {{"post_update_protocol","1"},{"operation_id","1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e"},
            {"terminal","true"},{"worker_busy","false"},{"status","ok_with_warnings"},{"package_commit","committed"},
            {"cleanup_status",cleanup},{"cleanup_removed_files","3"},{"cleanup_removed_bytes","8000"},
            {"cleanup_reclaimed_bytes","4096"},{"cleanup_details",QString(10000,'x')},
            {"reboot_requirement",reboot},{"reboot_reason","Verified updated kernel targets await boot."}};
    }
private slots:
    void noPopupOrPowerDuringWorkOrWithoutCommitProof() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.rebootNow(); QCOMPARE(flow.powerCalls,0); QCOMPARE(flow.reads,0);
        for (const char *field : {"terminal","worker_busy"}) {
            auto value=state();
            value[field] = QString(field)=="worker_busy" ? "true" : "unknown";
            flow.feed(value); QCOMPARE(flow.phase(),QStringLiteral("idle"));
        }
        flow.rebootNow(); QCOMPARE(flow.powerCalls,0); QCOMPARE(flow.reads,0);
    }
    void uncertainResults_data() {
        QTest::addColumn<QString>("commit");
        QTest::newRow("unknown") << QStringLiteral("unknown");
        QTest::newRow("partial") << QStringLiteral("failed-may-be-partial");
    }
    void uncertainResults() {
        QFETCH(QString,commit);
        QTemporaryDir dir; const auto file=dir.filePath("state.ini");
        auto value=state(); value["package_commit"]=commit; value["status"]="failed";
        value["message"]="disk failure <b>not markup</b>";
        value["hook_details"]="hook interrupted"; value["boot_details"]="boot unknown";
        FakePostUpdateFlow flow(file); flow.feed(value);
        QCOMPARE(flow.phase(),QStringLiteral("uncertain"));
        QVERIFY(flow.details().contains(value["operation_id"].toString()));
        for (const char *key : {"package_commit","message","hook_details","boot_details"})
            QVERIFY(flow.details().contains(value[key].toString()));
        QVERIFY(flow.details().contains("Servis sonucu: failed"));
        QVERIFY(!flow.canReviewReboot());
        const auto details=flow.details();
        flow.acknowledgeCleanup(); flow.reviewReboot(); flow.rebootNow();
        QCOMPARE(flow.phase(),QStringLiteral("uncertain")); QCOMPARE(flow.powerCalls,0);
        flow.refresh(); flow.feed(value); QCOMPARE(flow.details(),details);
        FakePostUpdateFlow reopened(file); reopened.refresh(); reopened.feed(value);
        QCOMPARE(reopened.phase(),QStringLiteral("uncertain")); QCOMPARE(reopened.details(),details);
        QCOMPARE(reopened.powerCalls,0);
        value["worker_busy"]="true"; flow.feed(value);
        QCOMPARE(flow.phase(),QStringLiteral("idle")); QVERIFY(!flow.canReviewReboot());
    }
    void errorsAndMalformedResponsesInvalidatePresentation() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        auto value=state(); value["package_commit"]="unknown";
        flow.feed(value); QCOMPARE(flow.phase(),QStringLiteral("uncertain"));
        flow.readError("org.freedesktop.DBus.Error.Timeout");
        QCOMPARE(flow.phase(),QStringLiteral("idle")); QVERIFY(flow.details().isEmpty());
        QVERIFY(flow.operationId().isEmpty()); QVERIFY(!flow.canReviewReboot());
        QVERIFY(flow.statusText().contains("Timeout"));
        flow.refresh(); flow.feed(value);
        QCOMPARE(flow.phase(),QStringLiteral("uncertain"));
        flow.feed(state()); flow.acknowledgeCleanup(); QVERIFY(flow.canReviewReboot());
        flow.feed({}); QVERIFY(!flow.canReviewReboot()); QVERIFY(flow.details().isEmpty());
        flow.feed(state()); value["operation_id"]="invalid"; flow.feed(value);
        QVERIFY(flow.details().isEmpty()); QVERIFY(!flow.canReviewReboot());
    }
    void staleResponsesCannotReplaceNewState() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.refresh(); const auto old=flow.generation();
        flow.setUpdateBusy(true); flow.setUpdateBusy(false);
        auto value=state(); value["package_commit"]="failed-may-be-partial";
        flow.feedRequest(value,flow.generation());
        QCOMPARE(flow.phase(),QStringLiteral("uncertain"));
        const auto details=flow.details();
        flow.feedRequest(state(),old); flow.errorRequest(old);
        QCOMPARE(flow.phase(),QStringLiteral("uncertain")); QCOMPARE(flow.details(),details);
        QVERIFY(!flow.canReviewReboot()); flow.rebootNow(); QCOMPARE(flow.powerCalls,0);
    }
    void explicitRebootNeedsAcknowledgementAndFreshServiceCheck() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.feed(state()); QCOMPARE(flow.phase(),QStringLiteral("cleanup"));
        flow.feed(state(),true); QCOMPARE(flow.powerCalls,0); // A reply cannot invent user consent.
        flow.rebootNow(); QCOMPARE(flow.reads,0); QCOMPARE(flow.powerCalls,0);
        flow.acknowledgeCleanup(); QCOMPARE(flow.phase(),QStringLiteral("reboot"));
        flow.rebootNow(); QCOMPARE(flow.reads,1); QVERIFY(flow.rebootCheck); QCOMPARE(flow.powerCalls,0);
        flow.feed(state(),true); QCOMPARE(flow.powerCalls,1);
        QSignalSpy errors(&flow,&PostUpdateFlow::operationError);
        flow.powerReply(false,"org.freedesktop.DBus.Error.AccessDenied","Authentication denied");
        QCOMPARE(errors.size(),1); QVERIFY(flow.statusText().contains("AccessDenied"));
        QVERIFY(flow.statusText().contains("Authentication denied"));
        QVERIFY(!flow.busy()); QCOMPARE(flow.powerCalls,1);
    }
    void localUpdateStartHidesCompletionAndBlocksRestart() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.feed(state()); QCOMPARE(flow.phase(),QStringLiteral("cleanup"));
        flow.setUpdateBusy(true); QCOMPARE(flow.phase(),QStringLiteral("idle"));
        flow.feed(state()); QCOMPARE(flow.phase(),QStringLiteral("idle"));
        flow.acknowledgeCleanup(); flow.rebootNow(); QCOMPARE(flow.powerCalls,0);
        QVERIFY(!flow.canReviewReboot());
    }
    void freshRunningStatePreventsPowerCall() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.feed(state()); flow.acknowledgeCleanup(); flow.rebootNow();
        auto changed=state(); changed["worker_busy"]="true";
        flow.feed(changed,true); QCOMPARE(flow.powerCalls,0); QVERIFY(!flow.busy());
    }
    void pendingSkippedFailedAreNeverReportedAsCompleted_data() {
        QTest::addColumn<QString>("cleanup");
        for (const char *value : {"skipped-unverified","failed","pending","warnings","not-run"})
            QTest::newRow(value)<<QString::fromLatin1(value);
    }
    void pendingSkippedFailedAreNeverReportedAsCompleted() {
        QFETCH(QString,cleanup);
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.feed(state(cleanup)); QCOMPARE(flow.phase(),QStringLiteral("cleanup"));
        QVERIFY(!flow.title().contains(QStringLiteral("tamamlandı")));
        QVERIFY(flow.details().contains(cleanup));
        flow.acknowledgeCleanup(); QCOMPARE(flow.phase(),QStringLiteral("reboot"));
        QCOMPARE(flow.powerCalls,0);
    }
    void absentOrNegativeRebootRequirementNeverPrompts_data() {
        QTest::addColumn<QString>("required");
        QTest::newRow("not-required")<<QStringLiteral("not-required");
        QTest::newRow("unknown")<<QStringLiteral("unknown");
        QTest::newRow("missing")<<QString();
    }
    void absentOrNegativeRebootRequirementNeverPrompts() {
        QFETCH(QString,required);
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.feed(state("completed",required)); flow.acknowledgeCleanup();
        QCOMPARE(flow.phase(),QStringLiteral("idle"));
        flow.reviewReboot(); flow.rebootNow(); QCOMPARE(flow.powerCalls,0);
    }
    void dedupAndDeferralSurviveApplicationReopen() {
        QTemporaryDir dir; const QString file=dir.filePath("state.ini");
        {
            FakePostUpdateFlow flow(file); flow.feed(state());
            flow.acknowledgeCleanup(); flow.deferReboot();
            const auto id=flow.operationId();
            for(int i=0;i<3;++i) flow.feed(state());
            QCOMPARE(flow.phase(),QStringLiteral("deferred")); QCOMPARE(flow.operationId(),id);
        }
        FakePostUpdateFlow reopened(file); reopened.feed(state());
        QCOMPARE(reopened.phase(),QStringLiteral("deferred")); QCOMPARE(reopened.powerCalls,0);
        reopened.reviewReboot(); QCOMPARE(reopened.phase(),QStringLiteral("reboot"));
        reopened.feed(state()); QCOMPARE(reopened.phase(),QStringLiteral("reboot"));
    }
    void acknowledgedCleanupResumesOnlyThePendingConfirmation() {
        QTemporaryDir dir; const QString file=dir.filePath("state.ini");
        { FakePostUpdateFlow first(file); first.feed(state()); first.acknowledgeCleanup(); }
        FakePostUpdateFlow reopened(file); reopened.feed(state());
        QCOMPARE(reopened.phase(),QStringLiteral("reboot")); QCOMPARE(reopened.powerCalls,0);
    }
    void acceptedPowerRequestNeverAutoRepeats() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.feed(state()); flow.acknowledgeCleanup(); flow.rebootNow(); flow.feed(state(),true);
        QCOMPARE(flow.powerCalls,1);
        flow.powerReply(true,"ok","Reboot requested");
        QCOMPARE(flow.phase(),QStringLiteral("deferred"));
        flow.feed(state()); QCOMPARE(flow.phase(),QStringLiteral("deferred"));
        QCOMPARE(flow.powerCalls,1);
    }
    void missingProductionProtocolCannotInventCompletionOrReboot() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        flow.start(); flow.readError("org.freedesktop.DBus.Error.UnknownMethod");
        QCOMPARE(flow.phase(),QStringLiteral("idle"));
        QVERIFY(flow.statusText().contains("GetPostUpdateState"));
        flow.rebootNow(); QCOMPARE(flow.powerCalls,0);
    }
    void failedAcknowledgementStorageDoesNotOpenRebootOrRepeatPopup() {
        QTemporaryDir dir;
        FakePostUpdateFlow flow(dir.path()); // A directory cannot be a settings file.
        flow.feed(state()); flow.acknowledgeCleanup();
        QCOMPARE(flow.phase(),QStringLiteral("error"));
        flow.feed(state()); QCOMPARE(flow.phase(),QStringLiteral("error"));
        flow.rebootNow(); QCOMPARE(flow.powerCalls,0); QVERIFY(!flow.canReviewReboot());
    }
    void malformedCompletedCountsNeverCreateSuccessMessage() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        auto malformed=state(); malformed.remove("cleanup_removed_files");
        flow.feed(malformed); QVERIFY(!flow.title().contains(QStringLiteral("tamamlandı")));
        QVERIFY(flow.details().contains(QStringLiteral("bildirilmedi")));
    }
    void nativeWindowsCloseInOrderAndScroll() {
        QTemporaryDir dir; FakePostUpdateFlow flow(dir.filePath("state.ini"));
        QQuickWindow host;
        QQmlEngine engine; QQmlComponent component(&engine,QUrl::fromLocalFile(POST_UPDATE_QML_PATH));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> root(component.createWithInitialProperties({{"flow",QVariant::fromValue(&flow)}}));
        QVERIFY2(root,qPrintable(component.errorString()));
        auto *item=qobject_cast<QQuickItem *>(root.data()); QVERIFY(item);
        item->setParentItem(host.contentItem()); host.show();
        QCoreApplication::processEvents();
        auto *cleanup=root->findChild<QQuickWindow *>("cleanupResultWindow");
        auto *reboot=root->findChild<QQuickWindow *>("postUpdateRebootWindow");
        QVERIFY(cleanup); QVERIFY(reboot);
        flow.feed(state()); QTRY_VERIFY(cleanup->isVisible()); QVERIFY(!reboot->isVisible());
        cleanup->resize(400,300);
        QObject *text=root->findChild<QObject *>("cleanupResultText"); QVERIFY(text);
        QTRY_VERIFY(text->property("contentHeight").toReal()>cleanup->height());
        QObject *scroll=root->findChild<QObject *>("cleanupResultScroll"); QVERIFY(scroll);
        QObject *flickable=scroll->property("contentItem").value<QObject *>(); QVERIFY(flickable);
        flickable->setProperty("contentY",100); QVERIFY(flickable->property("contentY").toReal()>0);
        bool overlap=false;
        connect(&flow,&PostUpdateFlow::changed,root.data(),[&]{ if(flow.phase()=="reboot" && cleanup->isVisible()) overlap=true; });
        QObject *ok=root->findChild<QObject *>("acknowledgeCleanupButton"); QVERIFY(ok);
        QVERIFY(QMetaObject::invokeMethod(ok,"click"));
        QTRY_VERIFY(!cleanup->isVisible()); QTRY_VERIFY(reboot->isVisible()); QVERIFY(!overlap);
        QVERIFY(host.isVisible());
        QCOMPARE(flow.powerCalls,0);
        QObject *later=root->findChild<QObject *>("rebootLaterButton"); QVERIFY(later);
        QVERIFY(QMetaObject::invokeMethod(later,"click")); QTRY_VERIFY(!reboot->isVisible());
        flow.feed(state()); QVERIFY(!cleanup->isVisible()); QVERIFY(!reboot->isVisible());
        QCOMPARE(flow.powerCalls,0);
    }
};
QTEST_MAIN(PostUpdateFlowTest)
#include "PostUpdateFlowTest.moc"
