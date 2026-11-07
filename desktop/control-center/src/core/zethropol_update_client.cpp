#include "zethropol_update_client.h"
#include "update_state_decoder.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QMetaType>
#include <QDBusMetaType>
#include <QThread>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSharedPointer>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <QRegularExpression>
#include "replacement_plan_decoder.h"
#include "operation_result_decoder.h"

namespace {
QString dbusError(const QDBusMessage &reply)
{
    return reply.errorMessage().isEmpty()
        ? QStringLiteral("Update D-Bus çağrısı beklenen yanıtı döndürmedi.")
        : reply.errorMessage();
}
}

ZethropolUpdateClient::ZethropolUpdateClient(QObject *parent)
    : QObject(parent)
{
    m_interface = new QDBusInterface(
        QStringLiteral("org.zethropol.Update"),
        QStringLiteral("/org/zethropol/Update"),
        QStringLiteral("org.zethropol.Update.v1"),
        QDBusConnection::systemBus(), this);

    if (!m_interface->isValid()) {
        emit errorOccurred(m_interface->lastError().message());
        return;
    }

    QDBusConnection::systemBus().connect(
        QStringLiteral("org.zethropol.Update"),
        QStringLiteral("/org/zethropol/Update"),
        QStringLiteral("org.zethropol.Update.v1"),
        QStringLiteral("UpdateStatusChanged"),
        this, SLOT(handleUpdateSignal(QVariant)));
}

void ZethropolUpdateClient::refreshUpdateState()
{
    if (!m_interface || !m_interface->isValid()) {
        const QString message = m_interface
            ? m_interface->lastError().message()
            : QStringLiteral("Update D-Bus arayüzü oluşturulamadı.");
        emit errorOccurred(message);
        m_pendingUpdates.clear();
        emit refreshFinished(false, message);
        emit pendingUpdatesChanged();
        return;
    }

    const QDBusMessage stateReply = m_interface->call(QStringLiteral("GetUpdateState"));
    if (stateReply.type() != QDBusMessage::ReplyMessage || stateReply.arguments().isEmpty()) {
        const QString message = dbusError(stateReply);
        emit errorOccurred(message);
        m_pendingUpdates.clear();
        emit refreshFinished(false, message);
        emit pendingUpdatesChanged();
        return;
    }

    QVariantMap state;
    QString stateError;
    if (!ZethropolUpdateWire::decodeState(stateReply.arguments(), &state, &stateError)) {
        emit errorOccurred(stateError);
        m_pendingUpdates.clear();
        emit refreshFinished(false, stateError);
        emit pendingUpdatesChanged();
        return;
    }
    m_updateState = state;
    emit updateStateChanged();

    const QDBusMessage listReply = m_interface->call(QStringLiteral("ListPendingUpdates"));
    if (listReply.type() != QDBusMessage::ReplyMessage || listReply.arguments().isEmpty()) {
        const QString message = dbusError(listReply);
        emit errorOccurred(message);
        m_pendingUpdates.clear();
        emit refreshFinished(false, message);
        emit pendingUpdatesChanged();
        return;
    }

    QVariantList pendingUpdates;
    QString parseError;
    if (!ZethropolUpdateWire::decodePendingPackages(listReply.arguments().constFirst(), &pendingUpdates, &parseError)) {
        emit errorOccurred(parseError);
        m_pendingUpdates.clear();
        emit refreshFinished(false, parseError);
        emit pendingUpdatesChanged();
        return;
    }

    m_pendingUpdates = pendingUpdates;
    emit pendingUpdatesChanged();
    emit refreshFinished(true, QString());
}

void ZethropolUpdateClient::requestTransactionPreview()
{
    if (!m_interface || !m_interface->isValid()) {
        const QString message = QStringLiteral("Update D-Bus arayüzü kullanılamıyor.");
        emit transactionPreviewFinished(false, QVariantList(), message);
        emit errorOccurred(message);
        return;
    }

    const auto request = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Update"), QStringLiteral("/org/zethropol/Update"),
        QStringLiteral("org.zethropol.Update.v1"), QStringLiteral("PreviewPlan"));
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(request, 90000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *finished) {
        const QDBusMessage reply = finished->reply();
        finished->deleteLater();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            const QString message = dbusError(reply);
            emit transactionPreviewFinished(false, QVariantList(), message);
            emit errorOccurred(message);
            return;
        }

        QVariantList packages, removals;
        QString hash, error;
        if (!ZethropolUpdateWire::decodePlan(reply.arguments(), &packages, &removals, &hash, &error)) {
            emit transactionPreviewFinished(false, {}, error);
            return;
        }
        emit transactionPlanReceived(removals, hash);
        emit transactionPreviewFinished(true, packages, QString());
    });
}

void ZethropolUpdateClient::triggerSystemUpdate(const QString &planHash, bool approveRemovals)
{
    if (!m_interface || !m_interface->isValid()) {
        emit triggerUpdateFailed(QStringLiteral("No update request was sent.\nD-Bus error: %1\n%2")
            .arg(m_interface ? m_interface->lastError().name() : QStringLiteral("interface_unavailable"),
                 m_interface ? m_interface->lastError().message() : QStringLiteral("Update D-Bus interface is unavailable.")));
        return;
    }

    auto *watcher = new QDBusPendingCallWatcher(
        m_interface->asyncCall(QStringLiteral("TriggerUpdate"), planHash, approveRemovals), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *finished) {
        const QDBusMessage reply = finished->reply();
        finished->deleteLater();
        handleTriggerReply(reply);
    });
}

void ZethropolUpdateClient::handleTriggerReply(const QDBusMessage &reply)
{
    if (reply.type() == QDBusMessage::ErrorMessage) {
        const QString details = QStringLiteral("D-Bus error: %1\n%2\nA new Preview Plan is required after a rejected or consumed request. No automatic retry was made.")
            .arg(reply.errorName(), dbusError(reply).left(8192));
        if (ZethropolUpdateWire::triggerRejectedBeforeStart(reply.errorName(), reply.errorMessage()))
            emit triggerUpdateFailed(QStringLiteral("No new update worker was started by this request.\n") + details);
        else
            emit triggerUpdateUncertain(details, QString());
        return;
    }
    QVariantMap result;
    QString error;
    if (reply.type() != QDBusMessage::ReplyMessage
        || !ZethropolUpdateWire::decodeOperationResult(reply.arguments(), &result, &error)) {
        emit triggerUpdateUncertain(error + QStringLiteral("\nReply signature: ") + reply.signature(), QString());
        return;
    }
    const QString id = result.value(QStringLiteral("operation_id")).toString();
    if (!result.value(QStringLiteral("success")).toBool()) {
        const QString details = QStringLiteral("Service code: %1\n%2\nOperation ID: %3")
            .arg(result.value(QStringLiteral("code")).toString(), result.value(QStringLiteral("message")).toString().left(8192), id);
        if (!id.isEmpty()) emit triggerUpdateUncertain(details, id);
        else if (result.value(QStringLiteral("code")).toString() == QStringLiteral("no_updates"))
            emit triggerUpdateFailed(QStringLiteral("No update was started.\n") + details);
        else emit triggerUpdateUncertain(details, QString());
        return;
    }
    emit operationInitiated(id);
}

void ZethropolUpdateClient::checkOperationStatus(const QString &operationId)
{
    if (!m_interface || !m_interface->isValid()) {
        emit operationStatusQueryFailed(
            operationId, QStringLiteral("Update D-Bus arayüzü kullanılamıyor."));
        return;
    }

    const QDBusMessage reply = m_interface->call(
        QStringLiteral("GetOperationState"), operationId);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        emit operationStatusQueryFailed(operationId, dbusError(reply));
        return;
    }

    const QVariantMap result = ZethropolUpdateWire::decodeMap(reply.arguments().constFirst());
    emit operationEvidenceReceived(operationId, result);
    emit operationStatusReceived(
        operationId,
        result.value(QStringLiteral("status")).toString(),
        result.value(QStringLiteral("message")).toString());
}

void ZethropolUpdateClient::handleUpdateSignal(const QVariant &newState)
{
    QVariantMap state;
    QString error;
    if (!ZethropolUpdateWire::decodeState({newState}, &state, &error)) {
        emit errorOccurred(error);
        return;
    }
    m_updateState = state;
    emit updateStateChanged();
}


namespace {
bool aurMap(const QDBusMessage &reply,QVariantMap *map) {
    if(reply.type()!=QDBusMessage::ReplyMessage||reply.signature()!=QStringLiteral("a{ss}")||reply.arguments().size()!=1)return false;
    const auto values=qdbus_cast<QMap<QString,QString>>(reply.arguments().first());
    if(values.isEmpty()||values.size()>64)return false;
    qsizetype total=0;
    for(auto i=values.cbegin();i!=values.cend();++i){total+=i.value().size();if(i.key().size()>256||i.value().size()>262144||total>1048576)return false;map->insert(i.key(),i.value());}
    return true;
}
bool aurDigest(const QString &hash){return QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch();}
bool aurId(const QString &id){return QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(id).hasMatch();}
bool aurPairs(const QList<QDBusUnixFileDescriptor> &archives,const QList<QDBusUnixFileDescriptor> &signatures){
    if(archives.isEmpty()||archives.size()>64||archives.size()!=signatures.size())return false;
    for(const auto &fd:archives)if(!fd.isValid())return false;
    for(const auto &fd:signatures)if(!fd.isValid())return false;
    return true;
}
QDBusMessage aurCall(const QString &method,const QVariantList &arguments){
    auto call=QDBusMessage::createMethodCall("org.zethropol.Update","/org/zethropol/Update","org.zethropol.Update.v1",method);
    call.setArguments(arguments);return call;
}
}
void ZethropolUpdateClient::reviewAurSelection(const QVariantList &selection){
    QStringList names,versions;
    for(const auto &value:selection){const auto row=value.toMap();names<<row.value("name").toString();versions<<row.value("newVersion").toString();}
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(aurCall("ReviewAurSelection",{names,versions}),15000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this](QDBusPendingCallWatcher *done){
        const auto reply=done->reply();done->deleteLater();QVariantMap report;
        const bool valid=aurMap(reply,&report)&&report.value("api_version")=="2"&&report.value("workflow")=="isolated-makepkg-alpm"&&QStringList{"blocked","review-required"}.contains(report.value("state").toString());
        if(!valid)report={{"state","blocked"},{"reason","AUR broker API unavailable or incompatible: "+dbusError(reply)},{"installation","closed"}};
        emit aurSelectionReviewed(valid,report);
    });
}
void ZethropolUpdateClient::aurStringRequest(const QString &method,const QVariantList &args,const QString &operationId){
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(aurCall(method,args),90000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,method,operationId](QDBusPendingCallWatcher *done){
        const auto reply=done->reply();done->deleteLater();
        if(reply.type()!=QDBusMessage::ReplyMessage){
            const bool rejected=QStringList{"org.freedesktop.DBus.Error.NotSupported","org.freedesktop.DBus.Error.AccessDenied","org.freedesktop.DBus.Error.InvalidArgs","org.freedesktop.DBus.Error.UnknownMethod"}.contains(reply.errorName());
            emit aurRequestFailed(operationId,dbusError(reply)+" No automatic retry.",!rejected);return;
        }
        if(method=="CancelAur"){
            if(!reply.arguments().isEmpty()||!reply.signature().isEmpty()){
                emit aurRequestFailed(operationId,"Invalid cancellation acknowledgement; inspect persisted status",true);return;
            }
            emit aurCancellationAcknowledged(operationId);return;
        }
        const QString id=reply.arguments().size()==1?reply.arguments()[0].toString():QString();
        if(reply.signature()!="s"||!aurId(id)){emit aurRequestFailed(operationId,"Invalid AUR operation reply; inspect persisted status. No automatic retry.",true);return;}
        if(method=="ReserveAurBuild"||method=="PrepareAurSelection"){
            emit aurReservationCreated(id);
            auto *context=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(aurCall("GetAurBuildContext",{id}),90000),this);
            connect(context,&QDBusPendingCallWatcher::finished,this,[this,id](QDBusPendingCallWatcher *done){
                const auto reply=done->reply();done->deleteLater();QVariantMap report;
                if(!aurMap(reply,&report)||report.value("operation_id")!=id){emit aurRequestFailed(id,"Cannot verify broker reservation context; no install request was sent",false);return;}
                emit aurBuildContextReceived(report);
            });
        }else emit aurOperationStarted(id);
    });
}
void ZethropolUpdateClient::reserveAurBuild(const QString &hash){
    Q_UNUSED(hash);emit aurRequestFailed({},"Manual source-review reservation disabled; use reviewed selection provisioning",false);
}

void ZethropolUpdateClient::previewAurReceipt(const QString &id,const QDBusUnixFileDescriptor &receipt,
    const QDBusUnixFileDescriptor &receiptSignature,const QDBusUnixFileDescriptor &manifest,
    const QDBusUnixFileDescriptor &manifestSignature,const QList<QDBusUnixFileDescriptor> &archives,
    const QList<QDBusUnixFileDescriptor> &signatures){
    Q_UNUSED(receipt);Q_UNUSED(receiptSignature);Q_UNUSED(manifest);Q_UNUSED(manifestSignature);Q_UNUSED(archives);Q_UNUSED(signatures);
    emit aurReceiptPreviewFinished(false,id,{},{},"Detached manifest/receipt delivery retired; use the reviewed makepkg output handoff");
}

void ZethropolUpdateClient::discardAurPreview(){
    m_aurPreparedId.clear();m_aurPreparedHash.clear();m_aurPreparedClock.invalidate();
    m_aurPreparedArchives.clear();m_aurPreparedSignatures.clear();
}
void ZethropolUpdateClient::installPreparedAur(const QString &hash){
    if(!m_aurPreparedClock.isValid()||m_aurPreparedClock.elapsed()>=300000){
        discardAurPreview();emit aurRequestFailed({},"AUR native preview expired; preview again",false);return;
    }
    const auto archives=m_aurPreparedArchives,signatures=m_aurPreparedSignatures;
    installAur(hash,archives,signatures);
}
void ZethropolUpdateClient::installAur(const QString &hash,const QList<QDBusUnixFileDescriptor> &archives,const QList<QDBusUnixFileDescriptor> &signatures){
    if(!signatures.isEmpty()||!m_aurPreparedClock.isValid()||m_aurPreparedClock.elapsed()>=300000||!aurDigest(hash)||archives.isEmpty()||archives.size()>64||hash!=m_aurPreparedHash||!aurId(m_aurPreparedId)){
        emit aurRequestFailed({},"A current native plan and matching sealed package archives are required",false);return;
    }
    for(const auto &fd:archives)if(!fd.isValid()){emit aurRequestFailed({},"Invalid package descriptor",false);return;}
    const QString operation=m_aurPreparedId;discardAurPreview();qDBusRegisterMetaType<QList<QDBusUnixFileDescriptor>>();
    aurStringRequest("InstallAur",{hash,QVariant::fromValue(archives)},operation);
}
void ZethropolUpdateClient::checkAurOperationStatus(const QString &id){
    if(!aurId(id)){emit aurRequestFailed(id,"Valid AUR operation UUID required",false);return;}
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(aurCall("GetAurOperationState",{id}),15000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,id](QDBusPendingCallWatcher *done){
        const auto reply=done->reply();done->deleteLater();QVariantMap state;
        if(!aurMap(reply,&state)||!state.contains("status")){emit aurRequestFailed(id,dbusError(reply),true);return;}
        emit aurOperationStateReceived(id,state);
    });
}
void ZethropolUpdateClient::cancelAur(const QString &id){
    if(!aurId(id)){emit aurRequestFailed(id,"Valid AUR operation UUID required",false);return;}
    aurStringRequest("CancelAur",{id},id);
}


namespace {
struct AurBundle {
    QString id,error;
    QList<QDBusUnixFileDescriptor> archives,signatures;
};
QDBusUnixFileDescriptor sealAurFile(const QString &path,quint64 maximum,quint64 &total,QString &error){
    QDBusUnixFileDescriptor input,output;
    if(!path.startsWith('/')){error="Absolute desktop-user artifact path required";return output;}
    input.giveFileDescriptor(::open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    struct stat info{};
    if(!input.isValid()||::fstat(input.fileDescriptor(),&info)||!S_ISREG(info.st_mode)||info.st_size<=0||quint64(info.st_size)>maximum||total+quint64(info.st_size)>1024ULL*1024*1024){error="Invalid artifact type, size or aggregate quota";return output;}
    output.giveFileDescriptor(::memfd_create("aur-client-handoff",MFD_CLOEXEC|MFD_ALLOW_SEALING));
    if(!output.isValid()){error="Sealed FD support unavailable";return {};}
    char buffer[65536];quint64 offset=0;
    while(offset<quint64(info.st_size)){
        const auto count=::pread(input.fileDescriptor(),buffer,qMin<quint64>(sizeof(buffer),quint64(info.st_size)-offset),off_t(offset));
        if(count<=0){error="Artifact changed or read failed";return {};}
        ssize_t written=0;
        while(written<count){const auto n=::write(output.fileDescriptor(),buffer+written,size_t(count-written));if(n<=0){error="Sealed snapshot write failed";return {};}written+=n;}
        offset+=quint64(count);
    }
    if(::fcntl(output.fileDescriptor(),F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK)){error="Cannot seal artifact snapshot";return {};}
    total+=quint64(info.st_size);return output;
}
}
void ZethropolUpdateClient::previewAurBundle(const QString &path){
    Q_UNUSED(path);
    emit aurReceiptPreviewFinished(false,{}, {},{},"Manual bundles retired; use the reviewed makepkg output");
}

void ZethropolUpdateClient::prepareAurSelection(const QVariantList &selection){
    if(selection.size()!=1){emit aurRequestFailed({},"One reviewed recipe is required",false);return;}
    aurStringRequest("ReserveAurBuild",{QString::fromUtf8(QJsonDocument::fromVariant(selection[0]).toJson(QJsonDocument::Compact))},{});
}
void ZethropolUpdateClient::requestAurProvisioning(const QString &id){emit aurRequestFailed(id,"Signed-root provisioning retired; use the pinned recipe",false);}
void ZethropolUpdateClient::submitAurBuild(const QString &id,const QVariantList &outputs){
    if(!aurId(id)||outputs.isEmpty()||outputs.size()>64){emit aurRequestFailed(id,"Invalid build output handoff",false);return;}
    auto bundle=QSharedPointer<AurBundle>::create();
    auto *thread=QThread::create([bundle,outputs]{
        quint64 total=0;
        for(const auto &value:outputs){
            if(total>256ULL*1024*1024){bundle->error="Build handoff quota exceeded";return;}
            bundle->archives<<sealAurFile(value.toMap().value("path").toString(),256ULL*1024*1024,total,bundle->error);
            if(!bundle->error.isEmpty())return;
        }
        if(total>256ULL*1024*1024)bundle->error="Build handoff quota exceeded";
    });
    connect(thread,&QThread::finished,this,[this,id,bundle]{
        if(!bundle->error.isEmpty()){emit aurRequestFailed(id,bundle->error,false);return;}
        qDBusRegisterMetaType<QList<QDBusUnixFileDescriptor>>();
        auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(aurCall("PreviewAurReceipt",{id,QVariant::fromValue(bundle->archives)}),90000),this);
        connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,id,bundle](QDBusPendingCallWatcher *done){
            const auto reply=done->reply();done->deleteLater();QVariantList packages,removals;QString hash,error;
            bool valid=reply.type()==QDBusMessage::ReplyMessage&&ZethropolUpdateWire::decodePlan(reply.arguments(),&packages,&removals,&hash,&error)&&removals.isEmpty()&&!packages.isEmpty();
            for(const auto &v:packages)valid=valid&&v.toMap().value("repository")=="aur-local";
            if(valid){m_aurPreparedId=id;m_aurPreparedHash=hash;m_aurPreparedArchives=bundle->archives;m_aurPreparedSignatures.clear();m_aurPreparedClock.start();}
            emit aurReceiptPreviewFinished(valid,id,valid?packages:QVariantList{},valid?hash:QString{},valid?QString{}:(error.isEmpty()?dbusError(reply):error));
        });
    });
    connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();
}
void ZethropolUpdateClient::fetchAurAttestation(const QString &id){emit aurAttestationUnavailable(id,"Independent project attestation retired; makepkg artifacts are previewed directly by libalpm");}
void ZethropolUpdateClient::releaseAurBuild(const QString &id){
    auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(aurCall("CancelAur",{id}),15000),this);
    connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,id](QDBusPendingCallWatcher *done){
        const auto reply=done->reply();done->deleteLater();
        const bool ok=reply.type()==QDBusMessage::ReplyMessage&&reply.signature().isEmpty();
        emit aurReservationReleased(id,ok,ok?QString{}:dbusError(reply));
    });
}
