#include "UpdateService.h"

#include "core/zethropol_update_client.h"

#include <QRegularExpression>
#include <QStringList>
#include <QStandardPaths>
#include <QDebug>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QJsonObject>
#include <QJsonArray>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

UpdateService::UpdateService(QObject *parent)
    : UpdateService(parent, true)
{
}

UpdateService::UpdateService(QObject *parent, bool runInitialCheck)
    : QObject(parent)
{
    connect(&m_updateClient, &ZethropolUpdateClient::refreshFinished,
            this, &UpdateService::finishOfficialScan);
    connect(&m_updateClient, &ZethropolUpdateClient::transactionPlanReceived,
            this, &UpdateService::finishPlanEffects);
    connect(&m_updateClient, &ZethropolUpdateClient::transactionPreviewFinished,
            this, &UpdateService::finishTransactionPreview);
    connect(&m_updateClient, &ZethropolUpdateClient::operationInitiated,
            this, [this](const QString &operationId) {
        m_activeOperationId = operationId;
        m_executionEvidence.clear();
        m_verificationRequired = false;
        m_status = QStringLiteral("Update transaction is running...");
        m_output = QStringLiteral("The package manager is applying the approved official-repository plan.");
        m_operationPollTimer.start();
        emit stateChanged();
    });
    connect(&m_updateClient, &ZethropolUpdateClient::operationStatusReceived,
            this, &UpdateService::finishOperationStatus);
    connect(&m_updateClient, &ZethropolUpdateClient::operationEvidenceReceived,
            this, &UpdateService::finishExecutionEvidence);
    connect(&m_updateClient, &ZethropolUpdateClient::operationStatusQueryFailed,
            this, [this](const QString &operationId, const QString &message) {
        if (operationId != m_activeOperationId)
            return;
        m_status = QStringLiteral("Update status is temporarily unavailable.");
        m_output = message + QStringLiteral(
            " The update may still be running; do not start another update yet.");
        emit stateChanged();
    });
    connect(&m_updateClient, &ZethropolUpdateClient::triggerUpdateFailed,
            this, [this](const QString &message) {
        m_installing = false;
        m_failed = true;
        m_status = QStringLiteral("Update transaction did not start.");
        m_output = message;
        m_previewReady = false;
        if(m_batchActive){setSourceState("official","failed",message);m_batchStatus="failed";m_batchActive=false;
            for(const auto &value:m_applyPlan)setSourceState(value.toMap().value("source").toString(),"blocked","Official transaction did not start; no subsequent stage was run");}
        emit stateChanged();
        emit operationError(QStringLiteral("Update failed to start"), message);
    });
    connect(&m_updateClient, &ZethropolUpdateClient::triggerUpdateUncertain,
            this, [this](const QString &message, const QString &id) {
        m_previewReady = false;
        m_failed = true;
        m_installing = true; // Prevent a duplicate request while outcome is unknown.
        m_verificationRequired = true;
        m_activeOperationId = id;
        setSourceState("official","unknown",message);
        m_status = QStringLiteral("Update start outcome requires verification.");
        m_output = message + QStringLiteral("\nThe worker may have started. Do not retry; inspect the operation status or journal first.");
        if (!id.isEmpty()) m_operationPollTimer.start();
        emit stateChanged();
        emit operationError(QStringLiteral("Update start not confirmed"), m_output);
    });
    m_previewExpiryTimer.setSingleShot(true);
    connect(&m_previewExpiryTimer, &QTimer::timeout, this, [this] {
        m_previewReady = false;
        m_approvedRemovalHash.clear();
        m_planHash.clear();
        m_status = QStringLiteral("Preview expired. Create a new Preview Plan before confirming.");
        emit stateChanged();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurSelectionReviewed,this,&UpdateService::finishAurBrokerReview);
    connect(&m_updateClient,&ZethropolUpdateClient::aurOperationStateReceived,this,&UpdateService::finishAurOperation);
    connect(&m_updateClient,&ZethropolUpdateClient::aurBuildContextReceived,this,[this](const QVariantMap &context){
        if(!m_aurReservationPending)return;
        m_aurReservationPending=false;
        if(m_aurRequestSelection!=selectionKey()){
            m_previewing=false;setSourceState("aur","blocked","Selection changed during broker reservation; review again. No installation was requested.");m_updateClient.releaseAurBuild(m_aurOperationId);emit stateChanged();return;
        }
        const auto reviewed=QJsonDocument::fromJson(context.value("review").toString().toUtf8()).object();
        const auto raw=QJsonDocument::fromVariant(m_normalAurReview).toJson(QJsonDocument::Compact);
        if(reviewed.value("source_plan_sha256").toString()!=QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex())){
            m_previewing=false;setSourceState("aur","blocked","Broker review differs from the selected pinned recipe");emit stateChanged();return;
        }
        const QJsonObject plan{{"kind","normal-makepkg"},{"review",QJsonObject::fromVariantMap(m_normalAurReview)}};
        const auto bytes=QJsonDocument(plan).toJson(QJsonDocument::Compact);
        const auto store=QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/aur-build";
        if(!QDir().mkpath(store)||::chmod(QFile::encodeName(store).constData(),0700)){
            m_previewing=false;setSourceState("aur","blocked","Cannot create private build store");emit stateChanged();return;
        }
        const QJsonObject request{{"store",store},{"recipe",m_normalAurReview.value("recipePath").toString()},{"plan",plan},{"plan_sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}};
        startProvisionedBuild(QJsonDocument(request).toJson(QJsonDocument::Compact));
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurReservationCreated,this,[this](const QString &id){
        m_aurOperationId=id;setSourceState("aur","reserved","Broker reservation: "+id+". A pinned recipe will be built without root; no install consent was granted.");emit stateChanged();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurReceiptPreviewFinished,this,[this](bool success,const QString &id,const QVariantList &packages,const QString &hash,const QString &error){
        if(!m_aurBundlePending)return;
        m_aurBundlePending=false;m_aurAttestationPending=false;m_previewing=false;
        m_previewReady=false;m_reviewHash.clear();m_approvedSkipHash.clear();m_approvedRemovalHash.clear();m_previewExpiryTimer.stop();
        m_aurReceiptHash.clear();m_aurReceiptClock.invalidate();
        QMap<QString,QString> selected,observed;
        for(const auto &v:selectedRows(m_aurUpdates)){const auto p=v.toMap();selected[p.value("name").toString()]=p.value("newVersion").toString();}
        for(const auto &v:packages){const auto p=v.toMap();observed[p.value("package_name").toString()]=p.value("new_version").toString();}
        success=success&&m_aurRequestSelection==selectionKey()&&!selected.isEmpty()&&selected==observed&&packages.size()==selected.size();
        if(success){m_aurReceiptHash=hash;m_aurReceiptSelection=selectionKey();m_aurReceiptClock.start();}
        else m_updateClient.discardAurPreview();
        const QString detail=success?"Verified makepkg/native plan "+hash+" for operation "+id+". Installation requires separate confirmation and all broker authorization checks.":(error.isEmpty()?"Package output differs from the selected AUR package/version set":error);
        if(success)m_aurOperationId=id;
        m_transactionPreview=detail+"\n"+QString::fromUtf8(QJsonDocument::fromVariant(packages).toJson(QJsonDocument::Indented));
        m_transactionPreviewReceived=true;setSourceState("aur",success?"preview-verified-awaiting-consent":"blocked",detail);emit stateChanged();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurOperationStarted,this,[this](const QString &id){
        m_aurOperationId=id;m_aurOwnsInstallFlag=true;m_installing=true;setSourceState("aur","running","AUR broker operation: "+id);m_aurOperationPoll.start();emit stateChanged();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurRequestFailed,this,[this](const QString &id,const QString &reason,bool uncertain){
        if(!id.isEmpty()&&id!=m_aurOperationId)return;
        m_aurOperationPoll.stop();m_aurCancelPending=false;
        if(m_aurReservationPending||m_aurProvisioningPending){
            const bool release=m_aurProvisioningPending;m_aurReservationPending=false;m_aurProvisioningPending=false;m_previewing=false;
            if(release&&!m_aurOperationId.isEmpty())m_updateClient.releaseAurBuild(m_aurOperationId);
        }
        if(m_aurBundlePending||m_aurAttestationPending){m_aurBundlePending=false;m_previewing=false;}
        if(m_aurOwnsInstallFlag&&!uncertain){m_aurOwnsInstallFlag=false;m_installing=false;}
        setSourceState("aur",uncertain?"unknown":"blocked",reason+" No automatic retry.");emit stateChanged();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurCancellationAcknowledged,this,[this](const QString &id){
        if(id!=m_aurOperationId)return;
        m_aurCancelPending=false;
        setSourceState("aur","cancellation-requested","Cancellation acknowledged; waiting for persisted terminal state. A completed native commit cannot be undone by cancellation.");
        emit stateChanged();m_updateClient.checkAurOperationStatus(id);
    });
    m_aurOperationPoll.setInterval(1500);
    connect(&m_aurOperationPoll,&QTimer::timeout,this,[this]{m_updateClient.checkAurOperationStatus(m_aurOperationId);});
    connect(&m_sourceReview,&SourceReview::finished,this,&UpdateService::finishSourceReview);
    connect(&m_sourceReview,&SourceReview::progress,this,[this](const QString &message){m_status=message;emit stateChanged();});
    connect(&m_updateClient,&ZethropolUpdateClient::aurProvisioningReceived,this,[this](const QString &id,const QVariantMap &delivery){
        if(!m_aurProvisioningPending||id!=m_aurOperationId)return;
        m_aurProvisioningPending=false;m_previewing=false;
        startProvisionedBuild(delivery.value("request_json").toString().toUtf8());
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurBuildSubmitted,this,[this](const QString &id){
        if(id!=m_aurOperationId)return;
        m_previewing=false;m_aurAttestationPending=true;
        setSourceState("aur","awaiting-native-preview","Unprivileged build complete; native plan preview pending; no installation authorized.");emit stateChanged();
        refreshAurAttestation();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurAttestationUnavailable,this,[this](const QString &id,const QString &reason){
        if(id!=m_aurOperationId)return;
        m_previewing=false;m_aurBundlePending=false;
        setSourceState("aur","awaiting-native-preview",reason+" No installation authorized.");emit stateChanged();
    });
    connect(&m_updateClient,&ZethropolUpdateClient::aurReservationReleased,this,[this](const QString &id,bool success,const QString &reason){
        if(id!=m_aurOperationId)return;
        m_previewing=false;m_aurAttestationPending=false;
        auto row=m_sourceResults.value("aur").toMap();row["reservationReleased"]=success;
        if(success&&QStringList{"awaiting-native-preview","reserving"}.contains(row.value("state").toString()))row["state"]="cancelled";
        row["detail"]=row.value("detail").toString()+(success?" Broker reservation retired; no replay.":" Reservation release unconfirmed: "+reason);
        m_sourceResults["aur"]=row;emit stateChanged();
    });
    connect(&m_sourceRunner,&SourceUpdateRunner::localBuildState,this,[this](const QVariantMap &state){
        const auto execution=state.value("execution").toString();
        if(state.value("stage")=="starting")m_localAurExecution=execution;
        if(execution!=m_localAurExecution)return;
        m_localAurOperation=state.value("operation").toString();
        auto row=m_sourceResults.value("aur").toMap();row["localBuild"]=state;
        row["detail"]="Local AUR build "+execution+" / journal "+m_localAurOperation+": "+state.value("stage",state.value("event")).toString();
        m_sourceResults["aur"]=row;emit stateChanged();
    });
    connect(&m_sourceRunner,&SourceUpdateRunner::finished,this,&UpdateService::finishSourceBatch);
    connect(&m_sourceRunner,&SourceUpdateRunner::progress,this,[this](const QString &message){
        if(m_batchStatus=="aur-build"&&message.startsWith("[AUR]"))return;
        setSourceState(message.startsWith("[AUR]")?"aur":"flatpak","running",message);emit stateChanged();
    });
    connect(&flatpakUserProcess,&QProcess::finished,this,[this]{finishFlatpakScope(flatpakUserProcess,"user");});
    connect(&flatpakUserProcess,&QProcess::errorOccurred,this,[this](QProcess::ProcessError e){
        if(e==QProcess::FailedToStart && m_checking){
            setSourceScan("flatpak-user","failed",flatpakUserProcess.errorString());
            m_flatpakScans["user"]=false;
            if(m_pendingChecks>0)--m_pendingChecks;finishScanIfComplete();emit stateChanged();
        }
    });
    m_operationPollTimer.setInterval(750);
    connect(&m_operationPollTimer, &QTimer::timeout, this, [this]() {
        if (!m_activeOperationId.isEmpty())
            m_updateClient.checkOperationStatus(m_activeOperationId);
    });
    connect(&orphanProcess, &QProcess::finished,
            this, &UpdateService::finishOrphanScan);
    connect(&aurProcess, &QProcess::finished,
            this, &UpdateService::finishAurScan);
    connect(&flatpakProcess, &QProcess::finished,
            this, &UpdateService::finishFlatpakScan);

    connect(&aurProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking && !m_installing)
            handleAurScanStartFailure(aurProcess.errorString());
    });
    connect(&flatpakProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking && !m_installing) {
            setSourceScan("flatpak-system","failed",flatpakProcess.errorString());
            m_flatpakScans["system"]=false;
            handleScanStartFailure(QStringLiteral("Flatpak scan"), flatpakProcess.errorString());
        }
    });
    connect(&orphanProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking && !m_installing)
            handleScanStartFailure(QStringLiteral("Orphan package scan"),
                                   orphanProcess.errorString());
    });

    if (runInitialCheck)
        check();
}

bool UpdateService::checking() const
{
    return m_checking;
}

bool UpdateService::installing() const
{
    return m_installing;
}

bool UpdateService::available() const
{
    return m_available;
}

bool UpdateService::failed() const
{
    return m_failed;
}

int UpdateService::count() const
{
    return m_count;
}

QString UpdateService::status() const
{
    return m_status;
}

QString UpdateService::output() const
{
    return m_output;
}

QVariantList UpdateService::updates() const
{
    return m_updates;
}

QStringList UpdateService::orphanPackages() const
{
    return m_orphanPackages;
}

int UpdateService::orphanCount() const
{
    return m_orphanPackages.size();
}

bool UpdateService::removingOrphans() const
{
    return m_removingOrphans;
}

bool UpdateService::aurAvailable() const
{
    return m_aurAvailable;
}

QString UpdateService::aurScanWarning() const
{
    return m_aurScanWarning;
}

bool UpdateService::previewReady() const
{
    return m_previewReady;
}

bool UpdateService::flatpakAvailable() const
{
    return m_flatpakAvailable;
}

int UpdateService::aurCount() const
{
    return m_aurUpdates.size();
}

int UpdateService::flatpakCount() const
{
    return m_flatpakUpdates.size();
}

QVariantList UpdateService::aurUpdates() const
{
    return m_aurUpdates;
}

QVariantList UpdateService::flatpakUpdates() const
{
    return m_flatpakUpdates;
}

int UpdateService::selectedCount() const
{
    int total = 0;
    const auto countSelected = [](const QVariantList &items) {
        int count = 0;
        for (const QVariant &item : items) {
            if (item.toMap().value(QStringLiteral("selected")).toBool())
                ++count;
        }
        return count;
    };

    total += countSelected(m_updates);
    total += countSelected(m_aurUpdates);
    total += countSelected(m_flatpakUpdates);
    return total;
}

QString UpdateService::planPreview() const
{
    if (m_transactionPreviewReceived)
        return m_transactionPreview;

    return QStringLiteral(
        "No official-repository transaction preview has been loaded. Use Preview Plan to resolve and inspect the complete pacman transaction.");
}

void UpdateService::previewPlan()
{
    if (m_checking || m_installing || m_previewing)
        return;
    m_previewing = true;
    m_officialPreviewDone=false;m_officialPreviewOk=false;m_reviewFinalized=false;
    m_reviewHash.clear();m_approvedSkipHash.clear();m_reviewPlan.clear();m_officialPreviewPackages.clear();
    m_aurBrokerReview.clear();m_aurBrokerReviewDone=selectedRows(m_aurUpdates).isEmpty();
    m_selectionAtReview=selectionKey();
    m_sourceReviewDone=selectedRows(m_aurUpdates).isEmpty()&&selectedRows(m_flatpakUpdates).isEmpty();
    m_batchStatus="review";
    m_previewClock.start();
    m_previewExpiryTimer.stop();
    m_planHash.clear();
    m_planRemovals.clear();
    m_approvedRemovalHash.clear();
    m_transactionPreviewReceived = false;
    m_previewReady = false;
    m_transactionPreview.clear();
    m_failed = false;
    m_transactionPreviewReceived = true;
    m_transactionPreview = QStringLiteral("Preparing the authenticated transaction plan. No package changes are being made...");
    m_status = QStringLiteral("Preparing transaction preview...");
    m_output.clear();
    emit stateChanged();
    auto aur=selectedRows(m_aurUpdates);
    for(auto &value:aur){auto entry=value.toMap();entry["scanValid"]=m_aurScanComplete;value=entry;}
    auto flatpak=selectedRows(m_flatpakUpdates);
    for(auto &value:flatpak){auto entry=value.toMap();entry["scanValid"]=m_flatpakScans.value(entry.value("scope").toString(),false);value=entry;}
    if(!aur.isEmpty())m_updateClient.reviewAurSelection(aur);
    m_sourceReview.start(aur,flatpak);
    if(m_officialSelected)m_updateClient.requestTransactionPreview();
    else finishTransactionPreview(true,{},{});
}

void UpdateService::finishPlanEffects(const QVariantList &removals, const QString &hash)
{
    m_planRemovals = removals;
    m_planHash = hash;
    m_approvedRemovalHash.clear();
}

void UpdateService::confirmRemovals(const QString &hash, bool approved)
{
    m_approvedRemovalHash = approved && m_previewReady && !hash.isEmpty() && hash == m_planHash
        ? hash : QString();
}

void UpdateService::finishTransactionPreview(
    bool success, const QVariantList &packages, const QString &message)
{
    m_officialPreviewDone=true;m_officialPreviewOk=success;m_officialPreviewPackages=packages;
    m_previewing = !m_sourceReviewDone;
    QStringList lines;
    if (!success) {
        m_transactionPreviewReceived = true;
        m_previewReady = false;
        m_transactionPreview = QStringLiteral("TRANSACTION REJECTED: %1 | No package changes were started.").arg(message.isEmpty() ? QStringLiteral("The transaction preview could not be validated.") : message);
        m_failed = true;
        m_status = QStringLiteral("Transaction preview rejected");
        m_output = m_transactionPreview;
        emit stateChanged();
        emit operationError(QStringLiteral("Transaction preview failed"), m_transactionPreview);
        finalizeReview();
        return;
    }

    if(!m_officialSelected){
        m_transactionPreview="Official repositories: not selected; no official transaction was requested.";
        m_transactionPreviewReceived=true;finalizeReview();emit stateChanged();return;
    }
    lines << QStringLiteral("VALIDATED PACMAN FULL-SYSTEM TRANSACTION PLAN");
    lines << QStringLiteral("Packages reported: %1").arg(success ? packages.size() : 0);
    lines << QString();

 {
        const auto isCriticalPackage = [](const QString &name) {
            static const QStringList exact = {
                QStringLiteral("linux"), QStringLiteral("linux-lts"),
                QStringLiteral("linux-cachyos"), QStringLiteral("linux-firmware"),
                QStringLiteral("mkinitcpio"), QStringLiteral("dracut"),
                QStringLiteral("limine"), QStringLiteral("grub"),
                QStringLiteral("systemd"), QStringLiteral("filesystem"),
                QStringLiteral("glibc"), QStringLiteral("pacman"),
                QStringLiteral("efibootmgr"), QStringLiteral("btrfs-progs"),
                QStringLiteral("snapper"), QStringLiteral("grub-btrfs"),
                QStringLiteral("intel-ucode"), QStringLiteral("amd-ucode"),
                QStringLiteral("cryptsetup"), QStringLiteral("lvm2")
            };
            return exact.contains(name)
                || name.startsWith(QStringLiteral("linux-"))
                || name.startsWith(QStringLiteral("grub-"))
                || name.startsWith(QStringLiteral("mkinitcpio-"))
                || name.startsWith(QStringLiteral("systemd-"))
                || name.startsWith(QStringLiteral("limine-"))
                || name.startsWith(QStringLiteral("dracut-"))
                || name.startsWith(QStringLiteral("efibootmgr-"))
                || name.startsWith(QStringLiteral("btrfs-progs-"))
                || name.startsWith(QStringLiteral("snapper-"))
                || name.startsWith(QStringLiteral("cryptsetup-"))
                || name.startsWith(QStringLiteral("lvm2-"))
                || name.startsWith(QStringLiteral("amd-ucode-"))
                || name.startsWith(QStringLiteral("intel-ucode-"));
        };
        int criticalCount = 0;

        if (packages.isEmpty()) {
            lines << QStringLiteral("No upgrade candidates were reported.");
        } else {
            for (const QVariant &value : packages) {
                const QVariantMap package = value.toMap();
                const QString name = package.value(QStringLiteral("package_name")).toString();
                const QString current = package.value(QStringLiteral("current_version")).toString();
                const QString next = package.value(QStringLiteral("new_version")).toString();
                const QString repository = package.value(QStringLiteral("repository")).toString();

                const bool isNew = current == QStringLiteral("not installed (new dependency)");
                QString entry = (isNew ? QStringLiteral("INSTALL: ") : QStringLiteral("UPGRADE: ")) + name;
                if (!current.isEmpty())
                    entry += QStringLiteral("  %1 -> %2").arg(current, next);
                else
                    entry += QStringLiteral("  -> %1 (installed version unavailable)").arg(next);
                entry += QStringLiteral("  [%1]").arg(repository);
                entry += QStringLiteral("\n  SHA-256: %1").arg(package.value(QStringLiteral("sha256")).toString());
                entry += QStringLiteral("\n  Repository metadata: %1").arg(package.value(QStringLiteral("repository_metadata_sha256")).toString());
                if (isCriticalPackage(name)) {
                    entry += QStringLiteral("  [CRITICAL SYSTEM PACKAGE]");
                    ++criticalCount;
                }
                lines << entry;
            }
        }

        lines << QString();
        lines << QStringLiteral("Critical system packages: %1").arg(criticalCount);
        if (criticalCount > 0)
            lines << QStringLiteral("WARNING: Critical packages are included. Review boot and recovery requirements; a root snapshot does not guarantee a bootable system.");
        lines << QString();
        lines << QStringLiteral("SAFETY LIMITATIONS");
        lines << QStringLiteral("libalpm dependency/conflict preparation and repository metadata validation passed. This does not prove ABI compatibility or bootability.");
        lines << QStringLiteral("Archive signatures and downloaded content will be checked again before commit. Recovery and Limine bootability are not certified by this preview.");
        lines << QStringLiteral("No packages have been changed. Installation starts only after the separate confirmation and required recovery snapshot.");
    }

    lines << QString() << QStringLiteral("REMOVALS / REPLACEMENTS: %1").arg(m_planRemovals.size());
    for (const QVariant &value : m_planRemovals) {
        const QVariantMap effect = value.toMap();
        lines << QStringLiteral("REMOVE: %1 %2 | reason: %3 | REPLACED BY: %4")
            .arg(effect.value(QStringLiteral("package_name")).toString(),
                 effect.value(QStringLiteral("current_version")).toString(),
                 effect.value(QStringLiteral("reason")).toString(),
                 effect.value(QStringLiteral("replacement")).toString());
    }
    lines << QStringLiteral("Plan SHA-256: %1").arg(m_planHash);
    lines << QStringLiteral("Authorization: authenticated PreviewPlan. Installation requires a separate PolicyKit decision and the same caller, current plan hash and single-use server token. Preview expires after five minutes; re-preview if expired or changed.");
    lines << QStringLiteral("RECOVERY NOT YET VERIFIED: the worker must validate the Snapper policy and create a pre-update snapshot. Limine/boot checks must not be assumed successful before execution and reboot verification.");
    lines << QStringLiteral("Conflict and unresolved dependency removals are blocked. AUR and Flatpak are separate.");
    m_transactionPreview = lines.join(QStringLiteral("\n"));
    m_transactionPreviewReceived = true;
    m_previewReady = m_sourceReviewDone && !packages.isEmpty() && !m_planHash.isEmpty();
    const qint64 remaining = 300000 - (m_previewClock.isValid() ? m_previewClock.elapsed() : 0);
    if (m_previewReady && remaining > 0) m_previewExpiryTimer.start(int(remaining));
    else if (remaining <= 0) m_previewReady = false;
    m_status = packages.isEmpty() ? QStringLiteral("No official updates remain.")
                                : QStringLiteral("Transaction plan ready for review.");
    m_output = QStringLiteral("No update has started. Review the plan and confirm separately. Recovery snapshot creation and boot verification remain required.");
    emit stateChanged();

    if (!success)
        emit operationError(QStringLiteral("Transaction preview failed"), m_transactionPreview);
    finalizeReview();
}

void UpdateService::finishExecutionEvidence(
    const QString &operationId, const QVariantMap &evidence)
{
    if (operationId != m_activeOperationId)
        return;
    // Older brokers omit these keys. Missing evidence is never success.
    m_executionEvidence.clear();
    for (const QString &key : {QStringLiteral("package_commit"),
                              QStringLiteral("hooks"),
                              QStringLiteral("boot_preparation"),
                              QStringLiteral("kernel_artifacts"),
                              QStringLiteral("dkms_artifacts"),
                              QStringLiteral("initramfs_artifacts"),
                              QStringLiteral("limine_artifacts"),
                              QStringLiteral("new_kernel_boot")}) {
        const QString value = evidence.value(key).toString();
        m_executionEvidence.insert(key, value.isEmpty() ? QStringLiteral("unknown") : value);
    }
    for (const QString &key : {QStringLiteral("pre_snapshot"), QStringLiteral("post_snapshot"),
                              QStringLiteral("running_kernel"), QStringLiteral("installed_kernels"),
                              QStringLiteral("updated_kernels"), QStringLiteral("boot_details"),
                              QStringLiteral("hook_details")})
        m_executionEvidence.insert(key, evidence.value(key).toString());
    for (const QString &key : {QStringLiteral("cleanup_status"), QStringLiteral("cleanup_removed_files"),
                              QStringLiteral("cleanup_removed_bytes"), QStringLiteral("cleanup_reclaimed_bytes"),
                              QStringLiteral("cleanup_details")})
        m_executionEvidence.insert(key, evidence.value(key).toString());

}

void UpdateService::finishOperationStatus(
    const QString &operationId, const QString &status, const QString &message)
{
    if (operationId != m_activeOperationId)
        return;

    m_status = message.isEmpty() ? QStringLiteral("Update status: %1").arg(status) : message;
    m_output = m_status;
    if (status == QStringLiteral("ok") || status == QStringLiteral("ok_with_warnings")) {
        m_failed = false;
        m_installing = false;
        const bool verified =
            m_executionEvidence.value(QStringLiteral("package_commit")).toString() == QStringLiteral("committed") &&
            m_executionEvidence.value(QStringLiteral("hooks")).toString() == QStringLiteral("verified") &&
            m_executionEvidence.value(QStringLiteral("boot_preparation")).toString() == QStringLiteral("verified") &&
            m_executionEvidence.value(QStringLiteral("new_kernel_boot")).toString() == QStringLiteral("verified");
        m_verificationRequired = !verified || status == QStringLiteral("ok_with_warnings");
        if (m_verificationRequired) {
            m_status = QStringLiteral("Update worker finished; verification is required.");
            m_output += QStringLiteral(
                "\nUnverified scripts or boot preparation must not be treated as successful. "
                "A snapshot alone does not prove recovery or bootability.");
        }
    } else if (status == QStringLiteral("failed") || status == QStringLiteral("not-found")) {
        m_status = QStringLiteral("Update worker failed after start.");
        m_output = QStringLiteral("Operation ID: %1\n%2").arg(operationId, message);
        if (message.startsWith(QStringLiteral("plan_changed:"))) {
            m_status = QStringLiteral("Update plan changed; create a new Preview Plan.");
            m_output += QStringLiteral("\nReview the changed fields above and create a fresh Preview Plan before confirming. No automatic retry was made.");
        }
        m_failed = true;
        m_installing = false;
    } else {
        m_installing = true;
    }

    if (!m_installing) {
        const QString commit = m_executionEvidence.value(QStringLiteral("package_commit")).toString();
        m_output += commit == QStringLiteral("committed")
            ? QStringLiteral("\nPackages: commit completed.")
            : commit == QStringLiteral("not-started")
                ? QStringLiteral("\nPackages: commit not started.")
                : QStringLiteral("\nPackages: outcome uncertain or failed; partial changes may exist.");
        const auto verificationText = [](const QString &value) {
            return value == QStringLiteral("verified") ? QStringLiteral("verified")
                 : value == QStringLiteral("failed") ? QStringLiteral("failed")
                 : QStringLiteral("unverified");
        };
        m_output += QStringLiteral("\nPackage scripts: %1.\nBoot preparation: %2.")
            .arg(verificationText(m_executionEvidence.value(QStringLiteral("hooks")).toString()),
                 verificationText(m_executionEvidence.value(QStringLiteral("boot_preparation")).toString()));
        for (const auto &item : {qMakePair(QStringLiteral("Kernel artifacts"), QStringLiteral("kernel_artifacts")),
                                 qMakePair(QStringLiteral("DKMS artifacts"), QStringLiteral("dkms_artifacts")),
                                 qMakePair(QStringLiteral("Initramfs artifacts"), QStringLiteral("initramfs_artifacts")),
                                 qMakePair(QStringLiteral("Limine Linux entries"), QStringLiteral("limine_artifacts"))}) {
            m_output += QStringLiteral("\n%1: %2.").arg(item.first,
                verificationText(m_executionEvidence.value(item.second).toString()));
        }
        const auto kernelList = [this](const QString &key) {
            const QString value = m_executionEvidence.value(key).toString();
            return value.isEmpty() ? QStringLiteral("not determined") : value;
        };
        m_output += QStringLiteral("\nRunning kernel: %1.\nInstalled kernels (on disk): %2.\nKernel targets in this transaction: %3.")
            .arg(kernelList(QStringLiteral("running_kernel")), kernelList(QStringLiteral("installed_kernels")),
                 kernelList(QStringLiteral("updated_kernels")));
        m_output += QStringLiteral("\nA successful boot of a newly installed kernel has not been observed. Artifact checks do not replace a reboot test.");
        for (const QString &key : {QStringLiteral("hook_details"), QStringLiteral("boot_details")}) {
            const QString details = m_executionEvidence.value(key).toString();
            if (!details.isEmpty())
                m_output += QStringLiteral("\n") + details;
        }
        const QString cleanup = m_executionEvidence.value(QStringLiteral("cleanup_status")).toString();
        m_output += QStringLiteral("\nCache cleanup: %1.")
            .arg(cleanup.isEmpty() ? QStringLiteral("not reported by this service") : cleanup);
        if (!cleanup.isEmpty()) {
            m_output += QStringLiteral("\nRemoved files: %1; removed archive/signature bytes: %2; observed available disk space increase: %3 bytes.")
                .arg(m_executionEvidence.value(QStringLiteral("cleanup_removed_files")).toString(),
                     m_executionEvidence.value(QStringLiteral("cleanup_removed_bytes")).toString(),
                     m_executionEvidence.value(QStringLiteral("cleanup_reclaimed_bytes")).toString());
            const QString details = m_executionEvidence.value(QStringLiteral("cleanup_details")).toString();
            if (!details.isEmpty()) m_output += QStringLiteral("\nCleanup details: ") + details;
            m_output += QStringLiteral("\nCleanup warnings do not change the package commit result. CoW may defer physical space reclamation.");
        }
        m_operationPollTimer.stop();
        m_activeOperationId.clear();
        m_previewReady = false;
        if (m_failed)
            emit operationError(message.startsWith(QStringLiteral("plan_changed:"))
                ? QStringLiteral("Update plan changed") : QStringLiteral("Update transaction failed"), m_output);
        else if (!m_verificationRequired && !m_batchActive)
            m_updateClient.refreshUpdateState();
    }
    if(m_batchActive && !m_installing){
        const bool committed=m_executionEvidence.value("package_commit")=="committed";
        const bool safe=committed && m_executionEvidence.value("hooks")=="verified"
            && m_executionEvidence.value("boot_preparation")=="verified" && !m_failed;
        setSourceState("official",m_failed?"failed":(m_verificationRequired?"completed-with-warnings":"completed"),m_output);
        if(safe) startExternalStages();
        else {
            for(const auto &value:m_applyPlan)setSourceState(value.toMap().value("source").toString(),"blocked","Official transaction failed or post-commit checks are unverified; no subsequent stage was run");
            m_batchStatus="blocked";m_batchActive=false;
        }
    }
    emit stateChanged();
}

void UpdateService::removeOrphans()
{
    m_removingOrphans = false;
    m_failed = true;
    m_status = QStringLiteral(
        "Orphan package removal is disabled until transaction preflight is implemented.");
    m_output = QStringLiteral("No package removal was started.");
    emit stateChanged();
}

void UpdateService::install()
{
    if(m_installing||m_previewing)return;
    if(m_previewClock.isValid()&&m_previewClock.elapsed()>=300000)m_previewReady=false;
    if(!m_previewReady||!m_transactionPreviewReceived||m_reviewHash.isEmpty()||m_selectionAtReview!=selectionKey()){
        m_failed=true;m_status="Update was not started.";
        m_output="Preview the selected sources again before applying. Source selections or review data may have changed.";
        emit stateChanged();emit operationError("Update preview required",m_output);return;
    }
    if(blockedSources()&&m_approvedSkipHash!=m_reviewHash){
        emit operationError("Selected sources are blocked","Review the source-specific blockers. Deselect those targets or explicitly acknowledge continuing only with supported stages. No source was silently skipped.");return;
    }
    if(!applicableSources()){
        m_status="No selected source can be applied.";
        m_output="The AUR review is informational until the reviewed build and native broker plan pass. No update request was sent.";
        setSourceState("aur","blocked",m_output);emit stateChanged();emit operationError("Selected sources are blocked",m_output);return;
    }
    if(officialTransactionRequired()&&(m_planHash.isEmpty()||(removalApprovalRequired()&&m_approvedRemovalHash!=m_planHash))){
        emit operationError("Explicit removal approval required","Review and approve the current official plan. No update was started.");return;
    }
    const QString submittedHash=m_planHash;
    const bool approveRemovals=removalApprovalRequired()&&m_approvedRemovalHash==m_planHash;
    m_applyPlan=m_reviewPlan;m_previewReady=false;m_previewExpiryTimer.stop();
    m_approvedRemovalHash.clear();m_approvedSkipHash.clear();
    m_installing=true;m_failed=false;m_batchActive=true;m_executionEvidence.clear();m_verificationRequired=false;
    if(officialTransactionRequired()){
        m_batchStatus="official";setSourceState("official","running","Full official-repository transaction; partial repository upgrades are not allowed");
        m_status="Authorizing the full official-repository transaction...";
        m_output="The official plan will be revalidated by the existing broker before commit.";
        emit stateChanged();m_updateClient.triggerSystemUpdate(submittedHash,approveRemovals);
    }else{
        setSourceState("official",m_officialSelected?"no-updates":"not-selected","No official transaction was requested");
        startExternalStages();
    }
}

void UpdateService::setPackageSelected(
    const QString &source, const QString &name, bool selected)
{
    if(m_installing||m_previewing)return;
    if(source=="official"){selectOfficialSource(selected);return;}
    QVariantList *items = nullptr;

    if (source == QStringLiteral("official"))
        items = &m_updates;
    else if (source == QStringLiteral("aur"))
        items = &m_aurUpdates;
    else if (source == QStringLiteral("flatpak"))
        items = &m_flatpakUpdates;
    else
        return;

    for (QVariant &item : *items) {
        QVariantMap package = item.toMap();
        if (package.value("selectionId",package.value("name")).toString() != name)
            continue;

        package.insert(QStringLiteral("selected"), selected);
        item = package;
        m_selections[source+":"+name]=selected;
        invalidateReview();
        emit stateChanged();
        return;
    }
}

void UpdateService::check()
{
    if (m_checking || m_installing || m_previewing ||
        orphanProcess.state() != QProcess::NotRunning ||
        aurProcess.state() != QProcess::NotRunning ||
        flatpakProcess.state() != QProcess::NotRunning || flatpakUserProcess.state() != QProcess::NotRunning)
        return;

    m_checking = true;
    m_installing = false;
    m_failed = false;
    m_transactionPreviewReceived = false;
    m_previewReady = false;
    m_transactionPreview.clear();
    m_status = QStringLiteral("Checking for updates...");
    m_output.clear();
    rememberSelections("official",m_updates);rememberSelections("aur",m_aurUpdates);rememberSelections("flatpak",m_flatpakUpdates);
    m_updates.clear();
    m_aurScanComplete=false;m_flatpakScans.clear();
    invalidateReview();
    setSourceScan("official","pending");setSourceScan("aur","pending");
    setSourceScan("flatpak-user","pending");setSourceScan("flatpak-system","pending");
    m_aurAvailable = !m_aurUpdates.isEmpty();
    m_aurScanWarning.clear();
    m_aurScanDiagnostic.clear();
    m_flatpakAvailable = !m_flatpakUpdates.isEmpty();
    m_pendingChecks = 2;
    m_scanErrors.clear();
    m_scanWarnings.clear();

    m_aurHelper.clear();

    const QStringList aurHelpers = {
        QStringLiteral("paru"),
        QStringLiteral("yay"),
        QStringLiteral("pikaur")
    };

    for (const QString &helper : aurHelpers) {
        const QString path = QStandardPaths::findExecutable(helper);
        if (!path.isEmpty()) {
            m_aurHelper = path;
            break;
        }
    }

    const QString flatpakPath =
        QStandardPaths::findExecutable(QStringLiteral("flatpak"));

    if (m_aurHelper.isEmpty()) {
        m_scanWarnings.append(QStringLiteral("AUR helper unavailable"));
        recordAurScanFailure(
            QStringLiteral("No supported AUR helper was found in PATH."));
    }
    if (flatpakPath.isEmpty()) {
        m_scanWarnings.append(QStringLiteral("Flatpak unavailable"));
        setSourceScan("flatpak-user","unavailable");setSourceScan("flatpak-system","unavailable");
    }

    emit stateChanged();

    m_updateClient.refreshUpdateState();

    orphanProcess.start(QStringLiteral("/usr/bin/pacman"), {
        QStringLiteral("-Qtdq")
    });

    if (!m_aurHelper.isEmpty()) {
        ++m_pendingChecks;
        aurProcess.start(m_aurHelper, {
            QStringLiteral("-Qua"), QStringLiteral("--color"), QStringLiteral("never")
        });
    }

    if (!flatpakPath.isEmpty()) {
        m_pendingChecks+=2;
        flatpakProcess.start(flatpakPath,{"remote-ls","--system","--updates","--columns=ref,origin"});
        flatpakUserProcess.start(flatpakPath,{"remote-ls","--user","--updates","--columns=ref,origin"});
    }
}

void UpdateService::finishOfficialScan(bool success, const QString &message)
{
    if (!m_checking) {
        if (!success) {
            m_failed = true;
            m_status = QStringLiteral("Official update status refresh failed.");
            m_output = message;
        } else {
            QVariantList normalized;
            const QVariantList pending = m_updateClient.pendingUpdates();
            normalized.reserve(pending.size());
            for (const QVariant &entry : pending) {
                const QVariantMap source = entry.toMap();
                QVariantMap update;
                update.insert(QStringLiteral("name"), source.value(QStringLiteral("package_name")));
                update.insert(QStringLiteral("currentVersion"), source.value(QStringLiteral("current_version")));
                update.insert(QStringLiteral("newVersion"), source.value(QStringLiteral("new_version")));
                update.insert(QStringLiteral("repository"), source.value(QStringLiteral("repository")));
                update.insert(QStringLiteral("download_size"), source.value(QStringLiteral("download_size")));
                update.insert(QStringLiteral("selected"), false);
                normalized.append(update);
            }
            restoreSelections("official",normalized);
            m_updates = normalized;
            setSourceScan("official","complete");
            m_count = m_updates.size();
            m_available = !m_updates.isEmpty();
            m_failed = false;
            m_status = m_available
                ? QStringLiteral("Official repository updates are available.")
                : QStringLiteral("System is up to date.");
        }
        emit stateChanged();
        return;
    }

    if (m_pendingChecks <= 0)
        return;

    if (!success) {
        setSourceScan("official","failed",message);
        m_scanErrors.append(
            QStringLiteral("Official repository scan failed: %1")
                .arg(message.isEmpty()
                    ? QStringLiteral("Update D-Bus request failed.")
                    : message));
        m_failed = true;
        m_updates.clear();
        m_count = 0;
        m_available = false;
    } else {
        const QVariantList pending = m_updateClient.pendingUpdates();
        QVariantList normalized;
        normalized.reserve(pending.size());

        for (const QVariant &entry : pending) {
            const QVariantMap source = entry.toMap();
            QVariantMap update;
            update.insert(QStringLiteral("name"),
                          source.value(QStringLiteral("package_name")));
            update.insert(QStringLiteral("currentVersion"),
                          source.value(QStringLiteral("current_version")));
            update.insert(QStringLiteral("newVersion"),
                          source.value(QStringLiteral("new_version")));
            update.insert(QStringLiteral("repository"),
                          source.value(QStringLiteral("repository")));
            update.insert(QStringLiteral("download_size"),
                          source.value(QStringLiteral("download_size")));
            update.insert(QStringLiteral("selected"), false);
            normalized.append(update);
        }

        restoreSelections("official",normalized);
        m_updates = normalized;
        setSourceScan("official","complete");
        m_count = m_updates.size();
        m_available = !m_updates.isEmpty();
    }

    --m_pendingChecks;
    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::handleScanStartFailure(
    const QString &source, const QString &error)
{
    if (!m_checking || m_installing || m_pendingChecks <= 0)
        return;

    m_scanErrors.append(QStringLiteral("%1 could not start: %2")
                            .arg(source, error));
    m_failed = true;
    --m_pendingChecks;
    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::handleAurScanStartFailure(const QString &error)
{
    if (!m_checking || m_installing || m_pendingChecks <= 0)
        return;

    recordAurScanFailure(
        QStringLiteral("AUR helper could not start: %1").arg(error));
    --m_pendingChecks;
    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::recordAurScanFailure(const QString &diagnostic)
{
    m_aurScanComplete=false;
    m_aurAvailable = !m_aurUpdates.isEmpty();
    setSourceScan("aur","failed",diagnostic);
    m_aurScanWarning = QStringLiteral("AUR updates could not be checked.");
    m_aurScanDiagnostic = diagnostic;
    qWarning().noquote() << "AUR source unavailable:" << m_aurScanDiagnostic;
}

void UpdateService::finishScanIfComplete()
{
    if (!m_checking || m_pendingChecks != 0)
        return;

    m_checking = false;

    if (!m_scanErrors.isEmpty()) {
        m_failed = true;
        m_status = QStringLiteral("Update scan incomplete: %1")
                       .arg(m_scanErrors.join(QStringLiteral("; ")));
    } else {
        m_failed = false;
        if (m_available || m_aurAvailable || m_flatpakAvailable) {
            m_status = QStringLiteral("Updates are available.");
        } else if (!m_aurScanWarning.isEmpty()) {
            m_status = QStringLiteral("AUR update status is unavailable.");
        } else {
            m_status = QStringLiteral("System is up to date.");
        }
    }

    if (!m_scanWarnings.isEmpty())
        m_status += QStringLiteral(" Unavailable sources: %1.")
                        .arg(m_scanWarnings.join(QStringLiteral(", ")));

    if (!m_scanErrors.isEmpty()) {
        emit operationError(QStringLiteral("Update scan failed"),
                            m_scanErrors.join(QStringLiteral("\n")));
    }
}

void UpdateService::finishOrphanScan()
{
    if (!m_checking || m_installing)
        return;

    const QString output =
        QString::fromLocal8Bit(orphanProcess.readAllStandardOutput()).trimmed();
    const QString error =
        QString::fromLocal8Bit(orphanProcess.readAllStandardError()).trimmed();
    const bool normalExit = orphanProcess.exitStatus() == QProcess::NormalExit;
    const int exitCode = orphanProcess.exitCode();

    // pacman -Qtdq exits with code 1 when no orphan packages match.
    if (normalExit && (exitCode == 0 || (exitCode == 1 && output.isEmpty()))) {
        m_orphanPackages = output.isEmpty()
            ? QStringList()
            : output.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                           Qt::SkipEmptyParts);
    } else {
        m_orphanPackages.clear();
        m_scanErrors.append(QStringLiteral("Orphan package scan failed: %1")
            .arg(error.isEmpty()
                ? QStringLiteral("Process exited with code %1.").arg(exitCode)
                : error));
        m_failed = true;
    }

    if (m_pendingChecks > 0)
        --m_pendingChecks;

    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::finishAurScan()
{
    const QString output =
        QString::fromLocal8Bit(aurProcess.readAllStandardOutput()).trimmed();

    const QString error=QString::fromLocal8Bit(aurProcess.readAllStandardError()).trimmed();
    if (aurProcess.exitStatus() != QProcess::NormalExit ||
        aurProcess.exitCode() != 0 || !error.isEmpty()) {
        QString diagnostic = aurProcess.exitStatus() == QProcess::NormalExit
            ? QStringLiteral("Process exited with code %1.").arg(aurProcess.exitCode())
            : QStringLiteral("Process terminated abnormally (reported exit code %1).")
                  .arg(aurProcess.exitCode());
        if (!error.isEmpty())
            diagnostic += QStringLiteral(" stderr: %1").arg(error);
        recordAurScanFailure(diagnostic);
        if (m_pendingChecks > 0)
            --m_pendingChecks;
        finishScanIfComplete();
        emit stateChanged();
        return;
    }

    QVariantList updates;

    const QStringList lines = output.split(
        QRegularExpression(QStringLiteral("[\r\n]+")),
        Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        const QStringList fields = line.split(
            QRegularExpression(QStringLiteral("[[:space:]]+")),
            Qt::SkipEmptyParts);

        if (fields.size()!=4 || fields.at(2)!="->" || !SourceReview::validName(fields.at(0))) {
            recordAurScanFailure("Unrecognized AUR helper output; scan is incomplete: "+line.left(500));
            if(m_pendingChecks>0)--m_pendingChecks;finishScanIfComplete();emit stateChanged();return;
        }

        QVariantMap update;
        update.insert(QStringLiteral("name"), fields.at(0));
        update.insert(QStringLiteral("currentVersion"), fields.at(1));
        update.insert(QStringLiteral("newVersion"), fields.at(3));
        update.insert(QStringLiteral("selected"), false);
        updates.append(update);
    }

    restoreSelections("aur",updates);
    for(auto &value:updates){auto row=value.toMap();row["scanValid"]=true;value=row;}
    m_aurScanComplete=true;setSourceScan("aur","complete");
    m_aurUpdates = updates;
    m_aurAvailable = !m_aurUpdates.isEmpty();

    if (m_pendingChecks > 0)
        --m_pendingChecks;

    finishScanIfComplete();

    emit stateChanged();
}

void UpdateService::finishFlatpakScan(){finishFlatpakScope(flatpakProcess,"system");}

void UpdateService::finishFlatpakScope(QProcess &process,const QString &scope)
{
    if(!m_checking)return;
    const QString output=QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    const QString error=QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    bool valid=process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0;
    QVariantList rows;
    for(const QString &line:output.split(QRegularExpression("[\r\n]+"),Qt::SkipEmptyParts)){
        const auto fields=line.split('\t');
        QVariantMap row{{"source","flatpak"},{"scope",scope}};
        if(fields.size()==2){row["ref"]=fields[0].trimmed();row["name"]=fields[0].trimmed();row["origin"]=fields[1].trimmed();row["selectionId"]=scope+":"+fields[0].trimmed();}
        if(!SourceReview::validFlatpak(row)){valid=false;break;}
        row["scanValid"]=true;rows<<row;
    }
    if(valid){
        QVariantList retained;for(const auto &value:m_flatpakUpdates)if(value.toMap().value("scope").toString()!=scope)retained<<value;
        restoreSelections("flatpak",rows);retained+=rows;m_flatpakUpdates=retained;
        setSourceScan("flatpak-"+scope,"complete");
    }else{
        for(auto &value:m_flatpakUpdates){auto row=value.toMap();if(row.value("scope").toString()==scope)row["scanValid"]=false;value=row;}
        const QString diagnostic=error.isEmpty()?"Invalid or incomplete Flatpak scan":error;
        setSourceScan("flatpak-"+scope,"failed",diagnostic);m_scanWarnings<<"Flatpak "+scope+" scan incomplete";
    }
    m_flatpakScans[scope]=valid;m_flatpakAvailable=!m_flatpakUpdates.isEmpty();
    if(m_pendingChecks>0)--m_pendingChecks;finishScanIfComplete();emit stateChanged();
}

QVariantList UpdateService::selectedRows(const QVariantList &rows) const {
    QVariantList result;for(const auto &value:rows)if(value.toMap().value("selected").toBool())result<<value;return result;
}
QString UpdateService::selectionKey() const {
    QVariantMap selection{{"official",m_officialSelected},{"aur",selectedRows(m_aurUpdates)},{"flatpak",selectedRows(m_flatpakUpdates)}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(selection).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
void UpdateService::rememberSelections(const QString &source,const QVariantList &rows){
    for(const auto &value:rows){const auto row=value.toMap();m_selections[source+":"+row.value("selectionId",row.value("name")).toString()]=row.value("selected").toBool();}
}
void UpdateService::restoreSelections(const QString &source,QVariantList &rows){
    for(auto &value:rows){auto row=value.toMap();row["source"]=source;const auto key=source+":"+row.value("selectionId",row.value("name")).toString();row["selected"]=source=="official"?m_officialSelected:m_selections.value(key,false);value=row;}
}
void UpdateService::invalidateReview(){m_aurConsentPending=false;m_aurReceiptHash.clear();m_aurReceiptClock.invalidate();m_updateClient.discardAurPreview();m_previewReady=false;m_reviewHash.clear();m_approvedSkipHash.clear();m_approvedRemovalHash.clear();m_previewExpiryTimer.stop();}
void UpdateService::selectOfficialSource(bool selected){
    if(m_installing||m_previewing)return;m_officialSelected=selected;
    for(auto &value:m_updates){auto row=value.toMap();row["selected"]=selected;value=row;}invalidateReview();emit stateChanged();
}
void UpdateService::setSourceState(const QString &source,const QString &state,const QString &detail){auto row=m_sourceResults.value(source).toMap();row["state"]=state;row["detail"]=detail;m_sourceResults[source]=row;}
void UpdateService::setSourceScan(const QString &source,const QString &state,const QString &detail){auto row=m_sourceResults.value(source).toMap();row["scan"]=state;row["scanError"]=detail;m_sourceResults[source]=row;}
QString UpdateService::sourceReport() const {
    QStringList lines;for(auto it=m_sourceResults.cbegin();it!=m_sourceResults.cend();++it){const auto row=it.value().toMap();QString text=it.key()+": scan="+row.value("scan","not checked").toString();if(row.contains("state"))text+="; result="+row.value("state").toString();
        for(const auto &key:{QStringLiteral("scanError"),QStringLiteral("detail")})if(!row.value(key).toString().isEmpty())text+="\n"+row.value(key).toString();lines<<text;}
    return lines.join("\n");
}
bool UpdateService::blockedSources() const {for(const auto &value:m_reviewPlan)if(value.toMap().value("state")!="ready")return true;return false;}
void UpdateService::confirmSkippedSources(const QString &hash,bool approved){m_approvedSkipHash=approved && m_previewReady && !hash.isEmpty()&&hash==m_reviewHash?hash:QString();}
void UpdateService::finishSourceReview(const QVariantList &plan){m_reviewPlan=plan;m_sourceReviewDone=true;finalizeReview();}
void UpdateService::finalizeReview(){
    if(!m_officialPreviewDone||!m_sourceReviewDone||!m_aurBrokerReviewDone||m_reviewFinalized)return;
    for(auto &value:m_reviewPlan){auto row=value.toMap();if(row.value("source")=="aur"){
        row["broker"]=m_aurBrokerReview;row["state"]="blocked";
        if(!m_aurBrokerReview.isEmpty())row["reason"]=row.value("reason").toString()+"\nBroker: "+m_aurBrokerReview.value("reason").toString();
        row["installCapability"]="blocked";value=row;
    }}
    m_reviewFinalized=true;m_previewing=false;
    QStringList details;
    for(const auto &value:m_reviewPlan){const auto row=value.toMap();details<<"SOURCE: "+row.value("source").toString()+" / "+row.value("name",row.value("ref")).toString();
        details<<QString::fromUtf8(QJsonDocument::fromVariant(row).toJson(QJsonDocument::Indented));}
    if(!details.isEmpty())m_transactionPreview+="\n\nSELECTED EXTERNAL SOURCES (separate stages)\n"+details.join("\n");
    m_reviewHash=QString::fromLatin1(QCryptographicHash::hash(m_planHash.toUtf8()+m_selectionAtReview.toUtf8()+QJsonDocument::fromVariant(m_reviewPlan).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    m_previewReady=m_officialPreviewOk && (!m_officialPreviewPackages.isEmpty()||!m_reviewPlan.isEmpty())
        && (!officialTransactionRequired()||!m_planHash.isEmpty()) && (!m_previewClock.isValid()||m_previewClock.elapsed()<300000);
    if(m_previewReady){m_previewExpiryTimer.start(int(300000-(m_previewClock.isValid()?m_previewClock.elapsed():0)));m_status=blockedSources()?"Preview ready: selected sources have explicit blockers":"Preview ready for selected sources";}
    emit stateChanged();
}
void UpdateService::startExternalStages(){
    m_installing=true;m_batchStatus="external";
    for(const auto &source:{QStringLiteral("aur"),QStringLiteral("flatpak")}){
        bool found=false;for(const auto &value:m_applyPlan)if(value.toMap().value("source")==source)found=true;
        if(!found)setSourceState(source,"not-selected","No selected targets; no operation started");
    }
    emit stateChanged();m_sourceRunner.start(m_applyPlan);
}
void UpdateService::finishSourceBatch(const QVariantList &results){
    if(m_batchStatus=="aur-build"){
        const auto result=results.value(0).toMap();const auto build=result.value("buildResult").toMap();
        const auto state=result.value("state").toString();
        auto row=m_sourceResults.value("aur").toMap();row["buildResult"]=build;row["state"]=state;
        row["detail"]=result.value("reason");m_sourceResults["aur"]=row;
        m_localAurExecution.clear();m_installing=state=="unknown";m_batchStatus=state;m_batchActive=false;
        if(state=="built-untrusted"&&build.value("cleanup")=="verified"){
            m_aurAttestationPending=false;m_aurBundlePending=true;m_previewing=true;
            m_updateClient.submitAurBuild(m_aurOperationId,build.value("archives").toList());
        }else if(!m_aurOperationId.isEmpty()){
            m_previewing=true;m_updateClient.releaseAurBuild(m_aurOperationId);
        }
        emit stateChanged();return;
    }

    bool problem=false;QHash<QString,QStringList> messages;QHash<QString,bool> failed;
    for(const auto &value:results){const auto row=value.toMap();const auto source=row.value("source").toString();const bool ok=row.value("state")=="completed";
        failed[source]=failed.value(source)||!ok;problem|=!ok;
        messages[source]<<row.value("name",row.value("ref")).toString()+": "+row.value("state").toString()+" — "+row.value("reason").toString();}
    for(auto it=messages.cbegin();it!=messages.cend();++it)setSourceState(it.key(),failed.value(it.key())?"incomplete":"completed",it.value().join("\n"));
    m_installing=false;m_batchActive=false;m_batchStatus=problem?"incomplete":"completed";
    m_status=problem?"Selected-source flow finished with blocked or failed stages; see individual results":"Selected-source stages completed";
    emit stateChanged();
}


bool UpdateService::applicableSources() const {
    if(officialTransactionRequired())return true;
    for(const auto &value:m_reviewPlan){const auto row=value.toMap();if(row.value("source")=="flatpak"&&row.value("state")=="ready")return true;}
    return false;
}
void UpdateService::finishAurBrokerReview(bool success,const QVariantMap &report){
    m_aurBrokerReview=success?report:QVariantMap{};m_aurBrokerReviewDone=true;
    setSourceState("aur",report.value("state","blocked").toString(),report.value("reason","AUR broker review unavailable").toString());
    finalizeReview();emit stateChanged();
}
void UpdateService::inspectAurOperation(const QString &id){
    if((m_installing||m_previewing||m_aurOperationPoll.isActive())&&id!=m_aurOperationId){
        emit operationError("AUR status is being tracked","Inspect the current operation before changing its status view.");return;
    }
    if(!QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(id).hasMatch()){
        emit operationError("Invalid AUR operation","Enter the broker operation UUID.");return;
    }
    if(id!=m_aurOperationId)invalidateReview();
    m_aurOperationId=id;m_updateClient.checkAurOperationStatus(id);
}
void UpdateService::finishAurOperation(const QString &id,const QVariantMap &state){
    if(id!=m_aurOperationId)return;
    const auto status=state.value("status").toString();
    const auto commit=state.value("package_commit").toString();
    if(!QStringList{"pending","running","ok","ok_with_warnings","failed"}.contains(status)
        ||!QStringList{"not-started","in-progress","committed","failed-may-be-partial","unknown"}.contains(commit)){
        m_aurOperationPoll.stop();setSourceState("aur","unknown","Invalid broker state; inspect persisted status. No automatic retry.");emit stateChanged();return;
    }
    const bool running=status=="pending"||status=="running";
    if(running)m_aurOperationPoll.start();else m_aurOperationPoll.stop();
    const bool uncertainCommit=commit=="unknown"||commit=="in-progress"||commit=="failed-may-be-partial";
    const QString result=uncertainCommit?"unknown":status;

    if(m_aurOwnsInstallFlag&&!running&&!uncertainCommit){
        m_aurOwnsInstallFlag=false;m_installing=false;
    }
    QString detail=QString("AUR %1: %2; native commit=%3; pre/post snapshots=%4/%5. %6")
        .arg(id,status,commit,state.value("pre_snapshot").toString(),state.value("post_snapshot").toString(),state.value("message").toString());
    if(commit=="committed"&&!state.value("updated_kernels").toString().trimmed().isEmpty()
        &&state.value("new_kernel_boot").toString()!="verified")
        detail+=" Reboot required to use the updated kernel; a successful boot has not been verified.";
    setSourceState("aur",result,detail);
    auto row=m_sourceResults.value("aur").toMap();row["evidence"]=state;row["operationId"]=id;m_sourceResults["aur"]=row;
    // Official executionEvidence, cleanup popup and reboot consent remain scoped
    // to their existing flow; an AUR warning is never official success.
    emit stateChanged();
}

void UpdateService::reserveAurBuild(const QString &digest){
    Q_UNUSED(digest);emit operationError("Manual AUR delivery disabled","Use the current pinned recipe review.");
}
void UpdateService::previewAurBundle(const QString &path){
    Q_UNUSED(path);emit operationError("Manual AUR delivery disabled","Use the reviewed makepkg build output.");
}
void UpdateService::startAurBuild(){
    if(m_installing||m_previewing||m_checking)return;
    if(selectedRows(m_aurUpdates).size()!=1||!m_previewReady||m_reviewHash.isEmpty()||m_selectionAtReview!=selectionKey()
        ||!m_previewClock.isValid()||m_previewClock.elapsed()>=300000){
        emit operationError("Current AUR review required","Select one recipe and create a current Preview Plan.");return;
    }
    if(!m_aurBrokerReviewDone||m_aurBrokerReview.value("api_version")!="2"||m_aurBrokerReview.value("state")!="review-required"){
        emit operationError("AUR build blocked",m_aurBrokerReview.value("reason","Current broker review required").toString());return;
    }
    m_normalAurReview.clear();const auto selected=selectedRows(m_aurUpdates).first().toMap();
    for(const auto &v:m_reviewPlan){const auto row=v.toMap();if(row.value("source")=="aur"&&row.value("name")==selected.value("name"))m_normalAurReview=row;}
    if(m_normalAurReview.value("recipeChecks")!="declarations-checked"||m_normalAurReview.value("state")!="reviewed"){
        emit operationError("AUR recipe blocked",m_normalAurReview.value("reason","Review the pinned recipe first").toString());return;
    }
    m_aurRequestSelection=selectionKey();m_aurReservationPending=true;m_previewing=true;
    setSourceState("aur","reserving","Reserving the reviewed recipe; no package installation authorized");emit stateChanged();
    m_updateClient.prepareAurSelection({m_normalAurReview});
}
void UpdateService::refreshAurAttestation(){
    if(m_installing||m_previewing||m_checking||!m_aurAttestationPending)return;
    if(m_aurRequestSelection!=selectionKey()){
        emit operationError("AUR selection changed","The build belongs to the original selection; create a new reviewed operation.");return;
    }
    m_aurBundlePending=true;m_previewing=true;m_updateClient.fetchAurAttestation(m_aurOperationId);emit stateChanged();
}
void UpdateService::buildAurRequest(const QString &path){
    Q_UNUSED(path);emit operationError("Manual AUR delivery disabled","Use the current pinned recipe selection.");
}
void UpdateService::startProvisionedBuild(const QByteArray &bytes){
    if(m_installing||m_aurRequestSelection!=selectionKey()||m_selectionAtReview!=selectionKey()||::geteuid()==0||bytes.isEmpty()||bytes.size()>1024*1024){
        m_previewing=false;setSourceState("aur","blocked","Pinned build selection changed or desktop-user request invalid");emit stateChanged();return;
    }
    QVariantMap row=m_normalAurReview;row["state"]="build-ready";row["buildRequestJson"]=bytes;
    m_previewing=false;m_installing=true;m_batchStatus="aur-build";m_localAurExecution.clear();
    setSourceState("aur","building","Non-root isolated makepkg build; native plan and PolicyKit approval are still required");emit stateChanged();m_sourceRunner.start({row});
}
void UpdateService::requestAurInstallConfirmation(){
    if(!aurInstallReady()){
        emit operationError("AUR installation unavailable","A current native package plan is required before installation consent.");return;
    }
    if(m_installing||m_previewing||m_aurReceiptHash.isEmpty()||m_aurReceiptSelection!=selectionKey()
        ||!m_aurReceiptClock.isValid()||m_aurReceiptClock.elapsed()>=300000){
        emit operationError("Current AUR plan required","Preview the reviewed makepkg artifacts for the current selection before requesting installation.");return;
    }
    m_aurConsentPending=true;emit aurInstallConfirmationRequested(m_aurReceiptHash,m_transactionPreview);
}
void UpdateService::confirmAurInstall(const QString &hash,bool approved){
    const bool requested=m_aurConsentPending;m_aurConsentPending=false;
    if(!approved)return;
    if(!aurInstallReady()||!requested||m_aurCancelPending||m_installing||m_previewing||hash.isEmpty()||hash!=m_aurReceiptHash||m_aurReceiptSelection!=selectionKey()
        ||!m_aurReceiptClock.isValid()||m_aurReceiptClock.elapsed()>=300000){
        emit operationError("AUR consent expired","Selection or native preview changed; preview and confirm again.");return;
    }
    m_aurReceiptHash.clear();m_aurOwnsInstallFlag=true;m_installing=true;
    setSourceState("aur","authorizing","Explicit AUR request sent to the broker security gate; installation is not yet authorized.");emit stateChanged();
    m_updateClient.installPreparedAur(hash);
}
bool UpdateService::aurInstallReady() const {
    return m_aurBrokerReviewDone&&m_aurBrokerReview.value("api_version")=="2"
        &&!m_aurReceiptHash.isEmpty()&&m_aurReceiptSelection==selectionKey()
        &&m_aurReceiptClock.isValid()&&m_aurReceiptClock.elapsed()<300000;
}
void UpdateService::requestAurCancellation(){
    if(m_batchStatus=="aur-build"&&m_installing){
        emit operationError("Broker cancellation unavailable","The local build journal and broker operation are separate. This action cannot cancel the local sandbox build.");return;
    }
    if(m_aurOperationId.isEmpty()||m_aurCancelPending)return;
    emit aurCancellationConfirmationRequested(m_aurOperationId);
}
void UpdateService::confirmAurCancellation(const QString &id,bool approved){
    if(!approved||id!=m_aurOperationId||id.isEmpty()||m_aurCancelPending||m_previewing)return;
    invalidateReview();
    if(m_aurAttestationPending){m_previewing=true;m_updateClient.releaseAurBuild(id);return;}
    m_aurCancelPending=true;m_updateClient.cancelAur(id);
}

void UpdateService::requestLocalAurCancellation(){
    if(m_batchStatus!="aur-build"||m_localAurExecution.isEmpty())return;
    emit localAurCancellationConfirmationRequested(m_localAurExecution);
}
void UpdateService::confirmLocalAurCancellation(const QString &execution,bool approved){
    if(!approved||m_batchStatus!="aur-build"||execution!=m_localAurExecution)return;
    if(!m_sourceRunner.cancelLocalBuild(execution)){
        setSourceState("aur","unknown","Local build cancellation unconfirmed; inspect the durable journal. No automatic retry.");emit stateChanged();
    }
}
