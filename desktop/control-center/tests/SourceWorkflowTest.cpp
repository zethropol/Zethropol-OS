#include "SourceReview.h"
#include "SourceUpdateRunner.h"
#include <QSignalSpy>
#include <QTest>
#include <QSysInfo>
#include <QJsonDocument>

// Fake native executor: no package command or network request can run here.
class FakeRunner : public SourceUpdateRunner {
public:
    QStringList last;
    QList<QStringList> commands;
    void answer(int code,const QByteArray &out={},const QByteArray &error={}) {
        reply(code,QProcess::NormalExit,out,error);
    }
protected:
    bool desktopUser() const override { return true; }
    void dispatch(const QStringList &args) override { last=args;commands<<args; }
};
class SourceWorkflowTest : public QObject {
    Q_OBJECT
    QVariantMap flatpak() const {
        return {{"source","flatpak"},{"scope","user"},{"origin","flathub"},
            {"ref","app/org.example.Demo/x86_64/stable"},{"state","ready"},
            {"commit",QString(64,'b')},{"currentCommit",QString(64,'a')}};
    }
    QByteArray recipeInfo() const {
        return "pkgbase = demo\npkgname = demo\npkgver = 2\npkgrel = 1\narch = any\nsource = https://example.org/demo.tar.gz\nsha256sums = "+QByteArray(64,'a')+"\ndepends = libc>=1\nconflicts = old-demo\n";
    }
private slots:
    void managedAurProtocolBindsProgressAndCancellationResult() {
        SourceUpdateRunner runner;
        runner.m_step="aur-build";runner.m_execution="11111111-1111-4111-8111-111111111111";
        runner.m_current={{"source","aur"},{"buildRequestJson",QByteArray("{\"plan_sha256\":\"")+QByteArray(64,'a')+"\"}"}};
        QSignalSpy events(&runner,&SourceUpdateRunner::localBuildState),done(&runner,&SourceUpdateRunner::finished);
        const auto line=[&](QVariantMap event){event["execution"]=runner.m_execution;return QJsonDocument::fromVariant(event).toJson(QJsonDocument::Compact)+'\n';};
        const QString operation="22222222-2222-4222-8222-222222222222";
        runner.consumeBuildOutput(line({{"event","started"},{"operation",operation},{"plan_sha256",QString(64,'a')}}));
        const auto progress=line({{"event","progress"},{"operation",operation},{"stage","building"}});
        runner.consumeBuildOutput(progress.left(10));QCOMPARE(events.count(),1);
        runner.consumeBuildOutput(progress.mid(10));QCOMPARE(events.count(),2);
        QVERIFY(!runner.cancelLocalBuild("33333333-3333-4333-8333-333333333333"));
        runner.consumeBuildOutput(line({{"event","result"},{"result",QVariantMap{{"operation",operation},{"state","cancelled"},{"cleanup","verified"},{"reason","Cancelled"}}}}));
        runner.reply(1,QProcess::NormalExit,{},{});QCOMPARE(done.count(),1);
        QCOMPARE(done[0][0].toList()[0].toMap().value("state").toString(),QString("cancelled"));
    }
    void managedAurLateCancelWithholdsAlreadyBuiltOutputs() {
        SourceUpdateRunner runner;runner.m_step="aur-build";runner.m_execution="11111111-1111-4111-8111-111111111111";
        runner.m_localCancelRequested=true;
        runner.m_buildResult={{"state","built-untrusted"},{"cleanup","verified"},{"reason","built"}};
        QSignalSpy done(&runner,&SourceUpdateRunner::finished);runner.reply(0,QProcess::NormalExit,{},{});
        const auto row=done[0][0].toList()[0].toMap();QCOMPARE(row.value("state").toString(),QString("cancelled"));
        QCOMPARE(row.value("buildResult").toMap().value("state").toString(),QString("built-untrusted"));
    }
    void managedAurProtocolRejectsForeignExecutionAndUnknownCleanup() {
        SourceUpdateRunner runner;runner.m_step="aur-build";runner.m_execution="11111111-1111-4111-8111-111111111111";
        runner.consumeBuildOutput("{\"event\":\"progress\",\"execution\":\"foreign\",\"stage\":\"building\"}\n");
        QVERIFY(runner.m_buildProtocolFailed);
        QSignalSpy done(&runner,&SourceUpdateRunner::finished);runner.reply(0,QProcess::NormalExit,{},{});
        QCOMPARE(done[0][0].toList()[0].toMap().value("state").toString(),QString("unknown"));
    }
    void liveAurMetadataOnly() {
        if(qEnvironmentVariable("ZETHROPOL_TEST_AUR_METADATA")!="1")QSKIP("Explicit opt-in for read-only AUR metadata; never builds or installs");
        SourceReview reviewer;QSignalSpy done(&reviewer,&SourceReview::finished);
        reviewer.start({QVariantMap{{"name","google-chrome"},{"newVersion",qEnvironmentVariable("ZETHROPOL_TEST_AUR_VERSION")},{"scanValid",true}}},{});
        QVERIFY(done.wait(180000));const auto row=done[0][0].toList()[0].toMap();
        qInfo().noquote()<<QJsonDocument::fromVariant(row).toJson(QJsonDocument::Indented);
        QCOMPARE(row.value("state").toString(),QString("blocked"));
        QVERIFY2(row.contains("pkgbuild_sha256"),qPrintable(row.value("reason").toString()));
        QCOMPARE(row.value("recipeChecks").toString(),QString("declarations-checked"));
        QVERIFY(!row.value("srcinfo").toString().isEmpty());
        QCOMPARE(row.value("installCapability").toString(),QString("unavailable"));
    }
    void noUpdatesAndUnselectedTargetsDispatchNothing() {
        SourceReview reviewer;QSignalSpy reviewed(&reviewer,&SourceReview::finished);
        reviewer.start({},{});QCOMPARE(reviewed.count(),1);QVERIFY(reviewed[0][0].toList().isEmpty());
        FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);
        runner.start({});QCOMPARE(done.count(),1);QVERIFY(runner.commands.isEmpty());
    }
    void availableAurIsPinnedButCannotSilentlyRunAnUnreviewedBuild() {
        const auto plan=SourceReview::inspectAur({{"name","demo"},{"newVersion","2-1"}},"demo",QString(40,'a'),"build() { echo untrusted; }",recipeInfo());
        QCOMPARE(plan.value("recipeChecks").toString(),QString("declarations-checked"));
        QCOMPARE(plan.value("state").toString(),QString("blocked"));
        QCOMPARE(plan.value("pkgbuild_sha256").toString().size(),64);
        QVERIFY(plan.value("reason").toString().contains("isolated build worker"));
        FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);runner.start({plan});
        QCOMPARE(done.count(),1);QVERIFY(runner.commands.isEmpty());
        QCOMPARE(done[0][0].toList()[0].toMap().value("state").toString(),QString("blocked"));
    }
    void recipeSecurityFailures_data() {
        QTest::addColumn<QByteArray>("info");QTest::addColumn<QString>("reason");
        QTest::newRow("version-changed")<<QByteArray(recipeInfo()).replace("pkgver = 2","pkgver = 3")<<QString("version changed");
        QTest::newRow("source-checksum-skip")<<QByteArray(recipeInfo()).replace(QByteArray(64,'a'),"SKIP")<<QString("checksum");
        QTest::newRow("unsafe-source")<<QByteArray(recipeInfo()).replace("https://example.org/demo.tar.gz","git+https://example.org/demo")<<QString("unsupported source");
        QTest::newRow("dependency-error")<<QByteArray(recipeInfo()).replace("libc>=1","bad dependency")<<QString("dependency");
        QTest::newRow("conflict-error")<<QByteArray(recipeInfo()).replace("old-demo","bad conflict")<<QString("conflict");
    }
    void recipeSecurityFailures() {
        QFETCH(QByteArray,info);QFETCH(QString,reason);
        auto plan=SourceReview::inspectAur({{"name","demo"},{"newVersion","2-1"}},"demo",QString(40,'a'),"recipe",info);
        QCOMPARE(plan.value("state").toString(),QString("blocked"));
        QCOMPARE(plan.value("recipeChecks").toString(),QString("failed"));
        QVERIFY2(plan.value("reason").toString().contains(reason),qPrintable(plan.value("reason").toString()));
    }
    void failedOrIncompleteAurScanCannotBecomeNoUpdates() {
        SourceReview reviewer;QSignalSpy done(&reviewer,&SourceReview::finished);
        reviewer.start({QVariantMap{{"name","demo"},{"scanValid",false}}},{});
        QCOMPARE(done.count(),1);const auto row=done[0][0].toList()[0].toMap();
        QCOMPARE(row.value("state").toString(),QString("blocked"));
        QVERIFY(row.value("reason").toString().contains("scan incomplete"));
    }
    void unsupportedAurCannotBeSentToFlatpakEvenIfMarkedReady() {
        auto row=flatpak();row["source"]="aur";
        FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);runner.start({row});
        QCOMPARE(done.count(),1);QVERIFY(runner.commands.isEmpty());
    }
    void blockedAurAndSuccessfulFlatpakRemainSeparate() {
        FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);
        runner.start({QVariantMap{{"source","aur"},{"name","demo"},
            {"state","blocked"},{"reason","Authenticated broker unavailable"}},flatpak()});
        runner.answer(0,"flathub");runner.answer(0,QByteArray(64,'a'));
        runner.answer(0);runner.answer(0,QByteArray(64,'b'));
        QCOMPARE(done.count(),1);
        const auto rows=done[0][0].toList();QCOMPARE(rows.size(),2);
        QCOMPARE(rows[0].toMap().value("source").toString(),QString("aur"));
        QCOMPARE(rows[0].toMap().value("state").toString(),QString("blocked"));
        QCOMPARE(rows[1].toMap().value("state").toString(),QString("completed"));
    }
    void nativeFailureIsNotSuccess_data() {
        QTest::addColumn<QString>("message");
        QTest::newRow("dependency-error")<<QString("Dependency resolution failed");
        QTest::newRow("conflict-error")<<QString("Conflicting deployment");
        QTest::newRow("authorization-denied")<<QString("Not authorized");
    }
    void nativeFailureIsNotSuccess() {
        QFETCH(QString,message);FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);
        runner.start({flatpak()});runner.answer(0,"flathub\n");runner.answer(0,QByteArray(64,'a'));
        QVERIFY(runner.last.contains("--commit="+QString(64,'b')));
        QVERIFY(runner.last.contains("--noninteractive"));QVERIFY(!runner.last.contains("--assumeyes"));
        QVERIFY(!runner.last.contains("--no-deps"));QVERIFY(!runner.last.contains("--no-gpg-verify"));
        runner.answer(1,{},message.toUtf8());QCOMPARE(done.count(),1);
        auto row=done[0][0].toList()[0].toMap();QCOMPARE(row.value("state").toString(),QString("failed"));
        QVERIFY(row.value("reason").toString().contains(message));
    }
    void changedInstalledStateBlocksApply() {
        FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);runner.start({flatpak()});
        runner.answer(0,"flathub");runner.answer(0,QByteArray(64,'c'));
        QCOMPARE(runner.commands.size(),2);QCOMPARE(done.count(),1);
        QCOMPARE(done[0][0].toList()[0].toMap().value("state").toString(),QString("blocked"));
    }
    void nativeSuccessRequiresReadback() {
        FakeRunner runner;QSignalSpy done(&runner,&SourceUpdateRunner::finished);runner.start({flatpak()});
        runner.answer(0,"flathub");runner.answer(0,QByteArray(64,'a'));runner.answer(0);
        QCOMPARE(done.count(),0);runner.answer(0,QByteArray(64,'c'));
        QCOMPARE(done[0][0].toList()[0].toMap().value("state").toString(),QString("unknown"));
    }
};
QTEST_GUILESS_MAIN(SourceWorkflowTest)
#include "SourceWorkflowTest.moc"
