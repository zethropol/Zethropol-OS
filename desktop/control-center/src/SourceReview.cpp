#include "SourceReview.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUrl>
#include <QUuid>

bool SourceReview::validName(const QString &name) {
    return QRegularExpression("^[a-zA-Z0-9][a-zA-Z0-9@._+-]{0,199}$").match(name).hasMatch();
}
bool SourceReview::validFlatpak(const QVariantMap &entry) {
    return QStringList{"user","system"}.contains(entry.value("scope").toString())
        && QRegularExpression("^(app|runtime)/[A-Za-z0-9_][A-Za-z0-9_.-]*/[A-Za-z0-9_-]+/[A-Za-z0-9._-]+$")
            .match(entry.value("ref").toString()).hasMatch()
        && validName(entry.value("origin").toString());
}
QVariantMap SourceReview::inspectAur(const QVariantMap &candidate,const QString &base,const QString &commit,
    const QByteArray &pkgbuild,const QByteArray &srcinfo) {
    QVariantMap result=candidate;
    result["source"]="aur"; result["state"]="blocked";
    result["packageBase"]=base; result["commit"]=commit;
    result["pkgbuild_sha256"]=QString::fromLatin1(QCryptographicHash::hash(pkgbuild,QCryptographicHash::Sha256).toHex());
    result["srcinfo_sha256"]=QString::fromLatin1(QCryptographicHash::hash(srcinfo,QCryptographicHash::Sha256).toHex());
    result["pkgbuild"]=QString::fromUtf8(pkgbuild);
    result["srcinfo"]=QString::fromUtf8(srcinfo);
    result["buildCapability"]="isolated-makepkg";
    result["installCapability"]="native-plan-and-polkit";
    QStringList issues;
    QMap<QString,QStringList> values;
    for(const QString &line:QString::fromUtf8(srcinfo).split('\n')) {
        const QString trimmed=line.trimmed();
        if(trimmed.isEmpty()||trimmed.startsWith('#'))continue;
        const int separator=trimmed.indexOf(" = ");
        if(separator<1){issues<<"Malformed .SRCINFO";continue;}
        values[trimmed.left(separator)]<<trimmed.mid(separator+3);
    }
    const QString name=candidate.value("name").toString();
    const QString epoch=values.value("epoch").value(0);
    const QString version=(epoch.isEmpty()||epoch=="0"?QString():epoch+":")+values.value("pkgver").value(0)+"-"+values.value("pkgrel").value(0);
    if(!validName(name)||!validName(base)||!QRegularExpression("^[0-9a-f]{40}$").match(commit).hasMatch()
        ||pkgbuild.isEmpty()||pkgbuild.size()>1024*1024||srcinfo.isEmpty()||srcinfo.size()>1024*1024)
        issues<<"Invalid or missing pinned recipe identity";
    if(values.value("pkgbase").value(0)!=base||!values.value("pkgname").contains(name))issues<<"Recipe package/base mismatch";
    if(values.value("pkgver").isEmpty()||values.value("pkgrel").isEmpty()||version!=candidate.value("newVersion").toString())
        issues<<"Recipe version changed; rescan and create a new Preview Plan";
    QString arch=QSysInfo::currentCpuArchitecture();
    if(arch=="amd64")arch="x86_64";
    if(!values.value("arch").contains(arch)&&!values.value("arch").contains("any"))issues<<"Unsupported build architecture";
    QStringList sources=values.value("source")+values.value("source_"+arch);
    if(sources.isEmpty())issues<<"No source declaration";
    for(const QString &source:sources){
        const QString location=source.contains("::")?source.section("::",1):source;
        const QUrl url(location);
        if(location.contains('$')||location.contains('`')||location.startsWith('/')||location==".."
            ||(url.scheme().isEmpty()? !QRegularExpression("^[A-Za-z0-9_+.-]+$").match(location).hasMatch()
                :url.scheme()!="https"||url.host().isEmpty()))issues<<"Unpinned/unsupported source: "+source;
    }
    bool checksums=false;
    for(const QString &algorithm:{QStringLiteral("sha256sums"),QStringLiteral("sha512sums"),QStringLiteral("b2sums")}){
        auto sums=values.value(algorithm)+values.value(algorithm+"_"+arch);
        const int length=algorithm=="sha256sums"?64:128;
        bool valid=!sources.isEmpty()&&sums.size()==sources.size();
        for(const auto &sum:sums)valid=valid&&QRegularExpression(QString("^[0-9a-fA-F]{%1}$").arg(length)).match(sum).hasMatch();
        checksums=checksums||valid;
    }
    if(!checksums)issues<<"Strong complete source checksum declarations required; SKIP is not accepted";
    QStringList dependencies;
    for(const QString &key:{QStringLiteral("depends"),QStringLiteral("makedepends"),QStringLiteral("checkdepends")})
        dependencies+=values.value(key)+values.value(key+"_"+arch);
    const QRegularExpression dependency("^[A-Za-z0-9][A-Za-z0-9@._+-]*(?:[<>=]{1,2}[^\\s]+)?$");
    for(const auto &dep:dependencies)if(!dependency.match(dep).hasMatch())issues<<"Invalid dependency declaration";
    const QStringList conflicts=values.value("conflicts")+values.value("conflicts_"+arch);
    for(const auto &dep:conflicts)if(!dependency.match(dep).hasMatch())issues<<"Invalid conflict declaration";
    result["dependencies"]=dependencies;result["conflicts"]=conflicts;result["sources"]=sources;
    result["recipeChecks"]=issues.isEmpty()?"declarations-checked":"failed";
    result["state"]=issues.isEmpty()?"reviewed":"blocked";
    if(issues.isEmpty())issues<<"Pinned recipe declarations checked. An isolated non-root makepkg build, native dependency/conflict checks and explicit PolicyKit installation authorization are required.";
    result["reason"]=issues.join("\n");
    return result;
}
SourceReview::SourceReview(QObject *parent):QObject(parent){
    m_timeout.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,[this]{m_process.kill();});
    connect(&m_process,&QProcess::finished,this,&SourceReview::completed);
    connect(&m_process,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart)finishEntry("Metadata command unavailable: "+m_process.errorString());
    });
}
SourceReview::~SourceReview(){m_process.disconnect(this);m_timeout.stop();if(m_process.state()!=QProcess::NotRunning){m_process.kill();m_process.waitForFinished(1000);}}
void SourceReview::start(const QVariantList &aur,const QVariantList &flatpak){
    m_queue.clear();m_results.clear();
    for(auto value:aur){auto item=value.toMap();item["source"]="aur";m_queue<<item;}
    for(auto value:flatpak){auto item=value.toMap();item["source"]="flatpak";m_queue<<item;}
    next();
}
void SourceReview::next(){
    if(m_queue.isEmpty()){emit finished(m_results);return;}
    m_current=m_queue.takeFirst().toMap();m_done=false;m_missing.clear();m_conflicts.clear();m_pkgbuild.clear();m_srcinfo.clear();
    if(!m_current.value("scanValid",false).toBool()){finishEntry("Source scan incomplete; selected item cannot be applied");return;}
    if(m_current.value("source")=="flatpak"){
        if(!validFlatpak(m_current)){finishEntry("Ambiguous Flatpak ref/scope/origin; rescan required");return;}
        command("flatpak-commit","/usr/bin/flatpak",{"remote-info","--"+m_current.value("scope").toString(),"--show-commit","--",m_current.value("origin").toString(),m_current.value("ref").toString()});
    }else{
        const auto name=m_current.value("name").toString();
        if(!validName(name)||!m_temp.isValid()){finishEntry("Invalid AUR target or private metadata directory");return;}
        command("rpc","/usr/bin/curl",{"--fail","--silent","--show-error","--location","--proto","=https","--proto-redir","=https","--max-time","30","--max-filesize","1048576","--","https://aur.archlinux.org/rpc/v5/info?arg%5B%5D="+name});
    }
}
void SourceReview::command(const QString &step,const QString &program,const QStringList &args){
    m_step=step;
    auto env=QProcessEnvironment::systemEnvironment();
    for(const auto &key:env.keys())if(key.startsWith("GIT_"))env.remove(key);
    env.insert("LC_ALL","C");env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL","/dev/null");
    env.insert("GIT_TERMINAL_PROMPT","0");env.insert("GIT_ALLOW_PROTOCOL","https");
    m_process.setProcessEnvironment(env);
    emit progress(m_current.value("source").toString()+": "+m_current.value("name").toString()+" — "+step);
    m_timeout.start(45000);m_process.start(program,args);
}
void SourceReview::completed(int code,QProcess::ExitStatus status){
    m_timeout.stop();if(m_done)return;
    const auto output=m_process.readAllStandardOutput();const auto error=m_process.readAllStandardError();
    if(output.size()>2*1024*1024||status!=QProcess::NormalExit){finishEntry("Metadata inspection failed or timed out: "+QString::fromUtf8(error).left(2048));return;}
    if(m_step=="deps"&&code==127){m_missing=QString::fromUtf8(output).trimmed().split('\n',Qt::SkipEmptyParts);}
    else if(m_step=="conflicts"&&(code==0||code==1)){m_conflicts=QString::fromUtf8(output).trimmed().split('\n',Qt::SkipEmptyParts);m_conflicts.removeAll(m_current.value("name").toString());}
    else if(code!=0){finishEntry(QString("%1 failed (%2): %3").arg(m_step).arg(code).arg(QString::fromUtf8(error).left(2048)));return;}
    if(m_step=="flatpak-commit"){
        const auto commit=QString::fromUtf8(output).trimmed();
        if(!QRegularExpression("^[0-9a-f]{64}$").match(commit).hasMatch()){finishEntry("Flatpak target commit not available");return;}
        m_current["commit"]=commit;
        command("flatpak-current","/usr/bin/flatpak",{"info","--"+m_current.value("scope").toString(),"--show-commit","--",m_current.value("ref").toString()});
    }else if(m_step=="flatpak-current"){
        const auto current=QString::fromUtf8(output).trimmed();
        if(!QRegularExpression("^[0-9a-f]{64}$").match(current).hasMatch()){finishEntry("Flatpak installed commit unavailable");return;}
        m_current["currentCommit"]=current;
        command("flatpak-origin","/usr/bin/flatpak",{"info","--"+m_current.value("scope").toString(),"--show-origin","--",m_current.value("ref").toString()});
    }else if(m_step=="flatpak-origin"){
        if(QString::fromUtf8(output).trimmed()!=m_current.value("origin").toString()){finishEntry("Flatpak origin changed during preview");return;}
        m_current["state"]="ready";m_current["reason"]="Native Flatpak verification and authorization required; required runtimes may be resolved by Flatpak. No force/no-deps/no-GPG bypass; interactive-only decisions fail instead of being auto-approved.";finishEntry();
    }else if(m_step=="rpc"){
        const auto object=QJsonDocument::fromJson(output).object();const auto list=object.value("results").toArray();
        if(object.value("type").toString()=="error"||list.size()!=1){finishEntry("AUR metadata is missing or ambiguous");return;}
        const auto info=list.first().toObject();m_base=info.value("PackageBase").toString();
        if(info.value("Name").toString()!=m_current.value("name").toString()||info.value("Version").toString()!=m_current.value("newVersion").toString()||!validName(m_base)){finishEntry("AUR version/source changed; run Check for Updates again");return;}
        m_repo=m_temp.path()+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        command("clone","/usr/bin/git",{"-c","core.hooksPath=/dev/null","clone","--quiet","--no-checkout","--depth=1","--","https://aur.archlinux.org/"+m_base+".git",m_repo});
    }else if(m_step=="clone")command("commit","/usr/bin/git",{"-C",m_repo,"rev-parse","HEAD"});
    else if(m_step=="commit"){
        m_commit=QString::fromUtf8(output).trimmed();
        if(!QRegularExpression("^[0-9a-f]{40}$").match(m_commit).hasMatch()){finishEntry("Unpinned AUR Git revision");return;}
        command("pkgbuild","/usr/bin/git",{"-C",m_repo,"show","--no-ext-diff","--no-textconv",m_commit+":PKGBUILD"});
    }else if(m_step=="pkgbuild"){
        m_pkgbuild=output;command("srcinfo","/usr/bin/git",{"-C",m_repo,"show","--no-ext-diff","--no-textconv",m_commit+":.SRCINFO"});
    }else if(m_step=="srcinfo"){
        m_srcinfo=output;m_current=inspectAur(m_current,m_base,m_commit,m_pkgbuild,m_srcinfo);m_current["recipePath"]=m_repo;
        if(m_current.value("recipeChecks")!="declarations-checked"){finishEntry();return;}
        const auto deps=m_current.value("dependencies").toStringList();
        if(!deps.isEmpty()){QStringList args{"-T","--"};args+=deps;command("deps","/usr/bin/pacman",args);return;}
        m_step="deps";completed(0,QProcess::NormalExit);
    }else if(m_step=="deps"){
        const auto conflicts=m_current.value("conflicts").toStringList();
        if(!conflicts.isEmpty()){
            QStringList args{"-Qq","--"};for(const auto &dep:conflicts)args<<dep.section(QRegularExpression("[<>=]"),0,0);
            command("conflicts","/usr/bin/pacman",args);return;
        }
        m_step="conflicts";completed(0,QProcess::NormalExit);
    }else if(m_step=="conflicts"){
        QString reason=m_current.value("reason").toString();
        if(!m_missing.isEmpty())reason+="\nMissing build/runtime dependencies (not installed on host): "+m_missing.join(", ");
        if(!m_conflicts.isEmpty())reason+="\nInstalled conflict targets require a reviewed transaction: "+m_conflicts.join(", ");
        if(!m_missing.isEmpty()||!m_conflicts.isEmpty())m_current["state"]="blocked";m_current["reason"]=reason;finishEntry();
    }
}
void SourceReview::finishEntry(const QString &error){
    if(m_done)return;m_done=true;m_timeout.stop();
    if(!error.isEmpty()){m_current["state"]="blocked";m_current["reason"]=error;}
    m_results<<m_current;next();
}
