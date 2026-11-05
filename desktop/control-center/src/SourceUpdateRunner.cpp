#include "SourceUpdateRunner.h"
#include "SourceReview.h"
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <unistd.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
SourceUpdateRunner::SourceUpdateRunner(QObject *parent):QObject(parent){
    m_timeout.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,[this]{if(m_step=="aur-build")cancelLocalBuild(m_execution);else m_process.kill();});
    connect(&m_process,&QProcess::finished,this,[this](int code,QProcess::ExitStatus status){reply(code,status,m_process.readAllStandardOutput(),m_process.readAllStandardError());});
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){if(e==QProcess::FailedToStart)finishEntry("blocked",m_process.errorString());});
    connect(&m_process,&QProcess::readyReadStandardOutput,this,[this]{
        if(m_step=="aur-build"){consumeBuildOutput(m_process.readAllStandardOutput());return;}
        if(m_step!="apply")return; // Query output is consumed only by reply().
        const QString output=QString::fromLocal8Bit(m_process.readAllStandardOutput());
        m_current["log"]=(m_current.value("log").toString()+output.left(8192)).right(65536);
        emit progress("[Flatpak] "+output.left(4096));
    });
    connect(&m_process,&QProcess::readyReadStandardError,this,[this]{
        const auto output=m_process.readAllStandardError();
        auto log=m_current.value("log").toString();log+=(QString::fromLocal8Bit(output).left(8192));
        m_current["log"]=log.right(65536);emit progress((m_current.value("source")=="aur"?"[AUR] ":"[Flatpak] ")+QString::fromLocal8Bit(output).left(4096));
    });
}
bool SourceUpdateRunner::desktopUser() const { return geteuid()!=0; }
SourceUpdateRunner::~SourceUpdateRunner(){
    m_timeout.stop();m_process.disconnect(this);
    if(m_process.state()!=QProcess::NotRunning){
        if(m_step=="aur-build"){
            cancelLocalBuild(m_execution);m_process.terminate();
            if(!m_process.waitForFinished(45000)){
                m_process.kill();m_process.waitForFinished(1000);
#ifdef ZETHROPOL_AUR_PIPELINE
                const auto request=QJsonDocument::fromJson(m_current.value("buildRequestJson").toByteArray()).object();
                QProcess cleanup;
                QStringList args{"-B",QStringLiteral(ZETHROPOL_AUR_PIPELINE),"--recover",request.value("store").toString()};
                if(!m_journalOperation.isEmpty())args<<m_journalOperation;
                cleanup.start("/usr/bin/python3",args);
                if(!cleanup.waitForFinished(30000)){cleanup.kill();cleanup.waitForFinished(1000);}
#endif
            }
        }else{m_process.kill();m_process.waitForFinished(1000);}
    }
}
void SourceUpdateRunner::start(const QVariantList &plan){if(m_process.state()!=QProcess::NotRunning)return;m_queue=plan;m_results.clear();next();}
void SourceUpdateRunner::next(){
    if(m_queue.isEmpty()){emit finished(m_results);return;}
    m_current=m_queue.takeFirst().toMap();m_done=false;
    if(m_current.value("source")=="aur"&&m_current.value("state")=="build-ready"){
        const QByteArray request=m_current.value("buildRequestJson").toByteArray();
        if(!desktopUser()||request.isEmpty()||request.size()>1024*1024){finishEntry("blocked","Private reviewed build request and desktop user required");return;}
        m_execution=QUuid::createUuid().toString(QUuid::WithoutBraces);m_journalOperation.clear();m_buildBuffer.clear();m_buildResult.clear();m_buildProtocolFailed=false;m_localCancelRequested=false;
        m_step="aur-build";emit localBuildState({{"execution",m_execution},{"stage","starting"}});emit progress("[AUR] Building in the existing signed-root sandbox; no installation consent");
        m_timeout.start(1800000);
#ifdef ZETHROPOL_AUR_PIPELINE
        const auto object=QJsonDocument::fromJson(request).object();
        m_process.start("/usr/bin/python3",{"-B",QStringLiteral(ZETHROPOL_AUR_PIPELINE),"--managed"});
        m_process.write(QJsonDocument(QJsonObject{{"execution",m_execution},{"request",object}}).toJson(QJsonDocument::Compact)+'\n');
#else
        finishEntry("blocked","Signed-root build pipeline is not packaged");
#endif
        return;
    }
    if(m_current.value("state")!="ready"){finishEntry("blocked",m_current.value("reason").toString());return;}
    if(m_current.value("source")!="flatpak"||!SourceReview::validFlatpak(m_current)
        ||!QRegularExpression("^[0-9a-f]{64}$").match(m_current.value("commit").toString()).hasMatch()
        ||!QRegularExpression("^[0-9a-f]{64}$").match(m_current.value("currentCommit").toString()).hasMatch()){
        finishEntry("blocked","No safe executor for this reviewed source/identity");return;
    }
    if(!desktopUser()){finishEntry("blocked","External source execution must run as the desktop user, not root");return;}
    m_step="origin";
    dispatch({"info","--"+m_current.value("scope").toString(),"--show-origin","--",m_current.value("ref").toString()});
}
void SourceUpdateRunner::dispatch(const QStringList &args){
    auto env=QProcessEnvironment::systemEnvironment();env.insert("LC_ALL","C");
    m_process.setProcessEnvironment(env);
    emit progress("[Flatpak] "+m_current.value("ref").toString()+" — "+m_step);
    m_timeout.start(m_step=="apply"?900000:45000);
    m_process.start("/usr/bin/flatpak",args);m_process.closeWriteChannel();
}
void SourceUpdateRunner::reply(int code,QProcess::ExitStatus status,const QByteArray &output,const QByteArray &error){
    m_timeout.stop();if(m_done)return;
    if(m_step=="aur-build"){
        consumeBuildOutput(output);
        if(status!=QProcess::NormalExit||m_buildProtocolFailed||m_buildResult.isEmpty()||!m_buildBuffer.isEmpty()){
            reconcileLocalBuild();finishEntry("unknown","AUR supervisor interrupted or invalid protocol; cleanup must be reconciled from the build journal. No install request or automatic retry.");return;
        }
        m_current["buildResult"]=m_buildResult;
        const auto state=m_buildResult.value("state").toString();
        if(!QStringList{"built-untrusted","cancelled","failed","blocked","interrupted"}.contains(state)
            ||m_buildResult.value("cleanup")!="verified"||(state=="built-untrusted"&&code!=0)){
            reconcileLocalBuild();finishEntry("unknown","AUR cleanup or outcome unverified; inspect durable journal. No installation authorized.");return;
        }
        if(m_localCancelRequested&&state=="built-untrusted"){
            finishEntry("cancelled","Build finished before cancellation was observed; artifact delivery cancelled. Original build result retained in journal.");return;
        }
        finishEntry(state,m_buildResult.value("reason").toString());return;
    }
    if(status!=QProcess::NormalExit){finishEntry("unknown","Flatpak process interrupted; outcome is uncertain. No automatic retry.");return;}
    if(code!=0){finishEntry("failed",QString("Flatpak %1 failed (%2): %3\n%4").arg(m_step).arg(code).arg(QString::fromLocal8Bit(error).left(8192),m_current.value("log").toString()));return;}
    if(m_step=="origin"){
        if(QString::fromUtf8(output).trimmed()!=m_current.value("origin").toString()){finishEntry("blocked","Flatpak origin changed; rescan and review again");return;}
        m_step="current";dispatch({"info","--"+m_current.value("scope").toString(),"--show-commit","--",m_current.value("ref").toString()});
    }else if(m_step=="current"){
        const auto current=QString::fromUtf8(output).trimmed();
        if(current==m_current.value("commit").toString()){finishEntry("completed","Already at the reviewed commit; no update needed");return;}
        if(current!=m_current.value("currentCommit").toString()){finishEntry("blocked","Installed Flatpak commit changed; create a new Preview Plan");return;}
        m_step="apply";
        dispatch({"update","--"+m_current.value("scope").toString(),"--noninteractive","--no-related","--commit="+m_current.value("commit").toString(),"--",m_current.value("ref").toString()});
    }else if(m_step=="apply"){
        // Exit status alone is not a claim of deployment. Read back its commit.
        m_step="verify";dispatch({"info","--"+m_current.value("scope").toString(),"--show-commit","--",m_current.value("ref").toString()});
    }else if(m_step=="verify"){
        if(QString::fromUtf8(output).trimmed()==m_current.value("commit").toString())finishEntry("completed","Native Flatpak deployment verified at reviewed commit");
        else finishEntry("unknown","Flatpak returned success but reviewed commit is not deployed; inspect native state before retrying");
    }
}
void SourceUpdateRunner::finishEntry(const QString &state,const QString &reason){if(m_done)return;m_done=true;m_timeout.stop();m_current.remove("buildRequestJson");m_current["execution"]=m_execution;m_current["journalOperation"]=m_journalOperation;m_current["state"]=state;m_current["reason"]=reason;m_results<<m_current;next();}

bool SourceUpdateRunner::cancelLocalBuild(const QString &execution){
    if(m_step!="aur-build"||m_done||execution.isEmpty()||execution!=m_execution||m_process.state()==QProcess::NotRunning)return false;
    m_localCancelRequested=true;
    if(m_process.write(QJsonDocument(QJsonObject{{"cancel",execution}}).toJson(QJsonDocument::Compact)+'\n')<0)return false;
    emit localBuildState({{"execution",execution},{"operation",m_journalOperation},{"stage","cancelling"}});
    return true;
}
void SourceUpdateRunner::consumeBuildOutput(const QByteArray &output){
    if(m_buildProtocolFailed)return;
    m_buildBuffer+=output;
    if(m_buildBuffer.size()>2*1024*1024){m_buildProtocolFailed=true;cancelLocalBuild(m_execution);return;}
    while(m_buildBuffer.contains('\n')){
        const auto newline=m_buildBuffer.indexOf('\n');const auto line=m_buildBuffer.left(newline);m_buildBuffer.remove(0,newline+1);
        const auto event=QJsonDocument::fromJson(line).object();
        if(line.size()>1024*1024||event.isEmpty()||event.value("execution").toString()!=m_execution||!m_buildResult.isEmpty()){
            m_buildProtocolFailed=true;cancelLocalBuild(m_execution);return;
        }
        const auto kind=event.value("event").toString();
        if(kind=="started"){
            const auto operation=event.value("operation").toString();
            const auto request=QJsonDocument::fromJson(m_current.value("buildRequestJson").toByteArray()).object();
            if(!m_journalOperation.isEmpty()||QUuid(operation).isNull()||event.value("plan_sha256")!=request.value("plan_sha256")){
                m_buildProtocolFailed=true;cancelLocalBuild(m_execution);return;
            }
            m_journalOperation=operation;
        }else if(kind=="result"){
            m_buildResult=event.value("result").toObject().toVariantMap();
            if(!m_journalOperation.isEmpty()&&m_buildResult.value("operation").toString()!=m_journalOperation)m_buildProtocolFailed=true;
        }else if(kind!="progress")m_buildProtocolFailed=true;
        if(!event.value("operation").toString().isEmpty()&&event.value("operation").toString()!=m_journalOperation)m_buildProtocolFailed=true;
        auto state=event.toVariantMap();state["execution"]=m_execution;state["operation"]=m_journalOperation;
        emit localBuildState(state);
        if(m_buildProtocolFailed){cancelLocalBuild(m_execution);return;}
    }
}

void SourceUpdateRunner::reconcileLocalBuild(){
#ifdef ZETHROPOL_AUR_PIPELINE
    const auto request=QJsonDocument::fromJson(m_current.value("buildRequestJson").toByteArray()).object();
    if(request.value("store").toString().isEmpty())return;
    QProcess cleanup;
    QStringList args{"-B",QStringLiteral(ZETHROPOL_AUR_PIPELINE),"--recover",request.value("store").toString()};
    if(!m_journalOperation.isEmpty())args<<m_journalOperation;
    cleanup.start("/usr/bin/python3",args);
    if(!cleanup.waitForFinished(30000)){cleanup.kill();cleanup.waitForFinished(1000);}
#endif
}
