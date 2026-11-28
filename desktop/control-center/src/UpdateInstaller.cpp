#include "UpdateInstaller.h"
#include "UpdateScanner.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUuid>

namespace {

QVariant decodeDbusVariantRecursive(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return decodeDbusVariantRecursive(value.value<QDBusVariant>().variant());

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto arg = value.value<QDBusArgument>();
    if (arg.currentType() == QDBusArgument::MapType) {
        const QString sig = arg.currentSignature();
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QString key;
            if (sig == QStringLiteral("a{ss}")) {
                QString val;
                arg >> key >> val;
                map.insert(key, val);
            } else {
                QVariant val;
                arg >> key >> val;
                map.insert(key, decodeDbusVariantRecursive(val));
            }
            arg.endMapEntry();
        }
        arg.endMap();
        return map;
    }

    if (arg.currentType() == QDBusArgument::ArrayType) {
        QVariantList list;
        arg.beginArray();
        while (!arg.atEnd()) {
            QVariant item;
            arg >> item;
            list.append(decodeDbusVariantRecursive(item));
        }
        arg.endArray();
        return list;
    }

    if (arg.currentType() == QDBusArgument::StructureType) {
        QVariantMap map;
        bool ok = false;
        QString code, message, opId;
        arg.beginStructure();
        arg >> ok >> code >> message >> opId;
        arg.endStructure();
        map.insert(QStringLiteral("success"), ok);
        map.insert(QStringLiteral("code"), code);
        map.insert(QStringLiteral("message"), message);
        map.insert(QStringLiteral("operation_id"), opId);
        return map;
    }

    return value;
}

void runDefaultRecoveryComplete(const QString &updateOpId, const QString &outcome)
{
    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected() || updateOpId.isEmpty())
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Recovery"),
        QStringLiteral("/org/zethropol/Recovery"),
        QStringLiteral("org.zethropol.Recovery.v1"),
        QStringLiteral("CompletePreUpdateExecution"));
    msg << updateOpId << outcome;
    bus.call(msg, QDBus::Block, 5000);
}

PreUpdateGateResult runDefaultRecoveryPreGate(const QString &updateOpId, bool criticalBootUpdate)
{
    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        return {false, QStringLiteral("not-available"),
                QStringLiteral("Sistem D-Bus bağlantısı kurulamadı; güncelleme engellendi."), 0};
    }

    auto callRecovery = [&](const QString &method, const QList<QVariant> &args, int timeoutMs = 10000) -> QDBusMessage {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.zethropol.Recovery"),
            QStringLiteral("/org/zethropol/Recovery"),
            QStringLiteral("org.zethropol.Recovery.v1"),
            method);
        msg.setArguments(args);
        return bus.call(msg, QDBus::Block, timeoutMs);
    };

    const QDBusMessage createReply = callRecovery(
        QStringLiteral("CreatePreUpdateRecoveryPoint"),
        {updateOpId, QStringLiteral("Pre-update safety snapshot")},
        125000);
    bool createOk = false;
    QString createCode, createMsg, recoveryOpId;
    if (!UpdateInstaller::decodeRecoveryOperationResult(createReply, createOk, createCode, createMsg, recoveryOpId)) {
        return {false, QStringLiteral("not-available"),
                QStringLiteral("Recovery servisi erişilemez veya geçersiz yanıt döndürdü; güncelleme engellendi."), 0};
    }
    if (!createOk || recoveryOpId.isEmpty()) {
        return {false, createCode, createMsg.isEmpty() ? QStringLiteral("Güncelleme öncesi snapshot başlatılamadı.") : createMsg, 0};
    }

    quint32 snapshotId = 0;
    for (int i = 0; i < 120; ++i) {
        const QDBusMessage stateReply = callRecovery(
            QStringLiteral("GetOperationState"), {recoveryOpId}, 5000);
        const QVariantMap opMap = UpdateInstaller::decodeRecoveryStringMap(stateReply);
        if (opMap.isEmpty()) {
            runDefaultRecoveryComplete(updateOpId, QStringLiteral("failed"));
            return {false, QStringLiteral("not-available"),
                    QStringLiteral("Recovery işlem durumu çözümlenemedi; güncelleme engellendi."), 0};
        }
        const QString status = opMap.value(QStringLiteral("status")).toString();
        const QString code = opMap.value(QStringLiteral("code")).toString();
        const QString message = opMap.value(QStringLiteral("message")).toString();
        if (status == QStringLiteral("ok")) {
            snapshotId = opMap.value(QStringLiteral("snapshot_id")).toString().toUInt();
            break;
        }
        if (status != QStringLiteral("running")) {
            runDefaultRecoveryComplete(updateOpId, QStringLiteral("failed"));
            return {false, code, message.isEmpty() ? QStringLiteral("Güncelleme öncesi snapshot doğrulanamadı.") : message, 0};
        }
        QThread::msleep(250);
    }
    if (snapshotId == 0) {
        runDefaultRecoveryComplete(updateOpId, QStringLiteral("failed"));
        return {false, QStringLiteral("verification-failed"),
                QStringLiteral("Güncelleme öncesi snapshot kimliği doğrulanamadı."), 0};
    }

    const QDBusMessage verifyReply = callRecovery(
        QStringLiteral("VerifyPreUpdateSnapshot"),
        {updateOpId, QVariant::fromValue(snapshotId), criticalBootUpdate},
        10000);
    bool verifyOk = false;
    QString verifyCode, verifyMsg, verifyOpId;
    if (!UpdateInstaller::decodeRecoveryOperationResult(verifyReply, verifyOk, verifyCode, verifyMsg, verifyOpId)
        || !verifyOk) {
        runDefaultRecoveryComplete(updateOpId, QStringLiteral("failed"));
        return {false,
                verifyCode.isEmpty() ? QStringLiteral("verification-failed") : verifyCode,
                verifyMsg.isEmpty()
                    ? QStringLiteral("Snapshot son doğrulama kapısı başarısız; güncelleme engellendi.")
                    : verifyMsg,
                snapshotId};
    }

    const QDBusMessage beginReply = callRecovery(
        QStringLiteral("BeginPreUpdateExecution"),
        {updateOpId, QVariant::fromValue(snapshotId)},
        5000);
    bool beginOk = false;
    QString beginCode, beginMsg, beginOpId;
    if (!UpdateInstaller::decodeRecoveryOperationResult(beginReply, beginOk, beginCode, beginMsg, beginOpId)
        || !beginOk) {
        runDefaultRecoveryComplete(updateOpId, QStringLiteral("failed"));
        return {false,
                beginCode.isEmpty() ? QStringLiteral("invalid-state") : beginCode,
                beginMsg.isEmpty()
                    ? QStringLiteral("Rezervasyon aktif güncelleme durumuna geçirilemedi.")
                    : beginMsg,
                snapshotId};
    }

    return {true, QStringLiteral("ok"), QStringLiteral("Doğrulandı"), snapshotId};
}

PostUpdateHealthEvidence runDefaultPostUpdateHealthProbe(
    const QString &updateOpId, quint32 snapshotId, const QString &sessionStatus)
{
    PostUpdateHealthEvidence ev;
    ev.pacmanLockPresent = QFile::exists(QStringLiteral("/var/lib/pacman/db.lck"));

    auto cleanEnv = QProcessEnvironment::systemEnvironment();
    cleanEnv.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    cleanEnv.insert(QStringLiteral("SYSTEMD_COLORS"), QStringLiteral("0"));

    const QString systemctlBin = QFile::exists(QStringLiteral("/usr/bin/systemctl"))
        ? QStringLiteral("/usr/bin/systemctl")
        : QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (!systemctlBin.isEmpty()) {
        QProcess sysProc;
        sysProc.setProcessEnvironment(cleanEnv);
        sysProc.start(systemctlBin, {QStringLiteral("--failed"), QStringLiteral("--no-legend"), QStringLiteral("--plain")});
        if (sysProc.waitForFinished(3000) && sysProc.exitStatus() == QProcess::NormalExit && sysProc.exitCode() == 0) {
            const QString out = QString::fromUtf8(sysProc.readAllStandardOutput());
            const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            ev.systemdChecked = true;
            ev.failedSystemdUnits = lines.size();
        } else {
            ev.systemdDetail = QStringLiteral("systemctl --failed sorgusu başarısız veya zaman aşımına uğradı.");
        }
    } else {
        ev.systemdDetail = QStringLiteral("systemctl aracı bulunamadı.");
    }

    const QString pacmanBin = QFile::exists(QStringLiteral("/usr/bin/pacman"))
        ? QStringLiteral("/usr/bin/pacman")
        : QStandardPaths::findExecutable(QStringLiteral("pacman"));
    if (!pacmanBin.isEmpty()) {
        QProcess dbProc;
        dbProc.setProcessEnvironment(cleanEnv);
        dbProc.setProcessChannelMode(QProcess::MergedChannels);
        dbProc.start(pacmanBin, {QStringLiteral("-Dk")});
        if (dbProc.waitForFinished(5000) && dbProc.exitStatus() == QProcess::NormalExit) {
            ev.pacmanDbChecked = true;
            ev.pacmanDbOk = (dbProc.exitCode() == 0);
            if (!ev.pacmanDbOk)
                ev.pacmanDbDetail = QString::fromUtf8(dbProc.readAllStandardOutput()).trimmed().left(512);
        } else {
            ev.pacmanDbDetail = QStringLiteral("pacman -Dk veritabanı bütünlük kontrolü tamamlanamadı.");
        }
    } else {
        ev.pacmanDbDetail = QStringLiteral("pacman aracı bulunamadı.");
    }

    const QString kernelRel = QSysInfo::kernelVersion().trimmed();
    if (!kernelRel.isEmpty()) {
        ev.activeKernelCheckDone = true;
        ev.activeKernelImagePresent = QFile::exists(QStringLiteral("/usr/lib/modules/%1/vmlinuz").arg(kernelRel));
    }

    const bool realLocalHealthOk = !ev.pacmanLockPresent
        && ev.systemdChecked && ev.failedSystemdUnits == 0
        && ev.activeKernelCheckDone && ev.activeKernelImagePresent
        && (!ev.pacmanDbChecked || ev.pacmanDbOk);

    const QDBusConnection bus = QDBusConnection::systemBus();
    if (bus.isConnected() && snapshotId > 0 && !updateOpId.isEmpty()) {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.zethropol.Recovery"),
            QStringLiteral("/org/zethropol/Recovery"),
            QStringLiteral("org.zethropol.Recovery.v1"),
            QStringLiteral("AssessPostUpdateRecovery"));
        const QString recoveryOutcome = (sessionStatus == QStringLiteral("finished"))
            ? QStringLiteral("ok")
            : (sessionStatus == QStringLiteral("unknown") ? QStringLiteral("unknown") : QStringLiteral("failed"));
        msg << updateOpId << QVariant::fromValue(snapshotId) << recoveryOutcome << realLocalHealthOk;
        const QDBusMessage reply = bus.call(msg, QDBus::Block, 5000);
        const QVariantMap data = UpdateInstaller::decodeRecoveryEnvelopeData(reply);
        if (!data.isEmpty()) {
            ev.recoveryAssessmentStatus = data.value(QStringLiteral("assessmentStatus")).toString();
            ev.recoveryAssessmentSummary = data.value(QStringLiteral("summary")).toString();
            ev.recoverySnapshotVerified = data.value(QStringLiteral("snapshotVerified")).toBool();
            ev.recoveryRecommendedLevel = data.value(QStringLiteral("recommendedLevel")).toString();
            const QString preflightOverall = data.value(QStringLiteral("preflightOverallStatus")).toString();
            ev.bootVerified = (preflightOverall == QStringLiteral("ready"));
            ev.bootDegraded = (preflightOverall == QStringLiteral("blocked"));
        }

        // When the privileged update helper created the Snapper pre-snapshot directly
        // (single Polkit prompt without a separate Recovery D-Bus reservation),
        // fall back to read-only PreflightRollback(snapshotId) to verify snapshot + boot integrity.
        if (ev.recoveryAssessmentStatus.isEmpty()
            || ev.recoveryAssessmentStatus == QStringLiteral("snapshot-unavailable")) {
            QDBusMessage pfMsg = QDBusMessage::createMethodCall(
                QStringLiteral("org.zethropol.Recovery"),
                QStringLiteral("/org/zethropol/Recovery"),
                QStringLiteral("org.zethropol.Recovery.v1"),
                QStringLiteral("PreflightRollback"));
            pfMsg << QVariant::fromValue(snapshotId);
            const QDBusMessage pfReply = bus.call(pfMsg, QDBus::Block, 8000);
            const QVariantMap pfData = UpdateInstaller::decodeRecoveryEnvelopeData(pfReply);
            const QString pfOverall = pfData.value(QStringLiteral("overallStatus")).toString();
            const bool pfCanProceed = pfData.value(QStringLiteral("canProceed")).toBool();
            if (pfOverall == QStringLiteral("ready") && pfCanProceed) {
                ev.recoverySnapshotVerified = true;
                ev.bootVerified = true;
                ev.bootDegraded = false;
                if (sessionStatus == QStringLiteral("finished") && realLocalHealthOk) {
                    ev.recoveryAssessmentStatus = QStringLiteral("healthy");
                    ev.recoveryRecommendedLevel = QStringLiteral("none");
                    ev.recoveryAssessmentSummary = QStringLiteral(
                        "Recovery ön kontrolü (PreflightRollback #%1) ve önyükleyici bütünlüğü doğrulandı.")
                        .arg(snapshotId);
                } else {
                    ev.recoveryAssessmentStatus = QStringLiteral("recovery-recommended");
                    ev.recoveryRecommendedLevel = QStringLiteral("guided-recovery");
                    ev.recoveryAssessmentSummary = QStringLiteral(
                        "Snapshot #%1 kurtarma için hazır; işlem sonrası ek inceleme önerilir.")
                        .arg(snapshotId);
                }
            } else if (pfOverall == QStringLiteral("blocked")) {
                ev.bootVerified = false;
                ev.bootDegraded = true;
                ev.recoveryAssessmentStatus = QStringLiteral("recovery-recommended");
                ev.recoveryAssessmentSummary = QStringLiteral(
                    "Recovery ön kontrolü (PreflightRollback #%1) önyükleme/snapshot engeli bildirdi.")
                    .arg(snapshotId);
            }
        }
    }
    if (ev.recoveryAssessmentStatus.isEmpty())
        ev.recoveryAssessmentStatus = QStringLiteral("unknown");

    return ev;
}

} // namespace

bool UpdateInstaller::decodeRecoveryOperationResult(
    const QDBusMessage &reply, bool &ok, QString &code, QString &message, QString &operationId)
{
    ok = false;
    code.clear();
    message.clear();
    operationId.clear();

    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;

    const QVariantList args = reply.arguments();
    if (args.size() >= 3 && args.at(0).userType() == QMetaType::Bool) {
        ok = args.at(0).toBool();
        code = args.at(1).toString();
        message = args.at(2).toString();
        if (args.size() >= 4)
            operationId = args.at(3).toString();
        return true;
    }

    const QVariant decoded = decodeDbusVariantRecursive(args.constFirst());
    if (decoded.userType() == QMetaType::QVariantMap) {
        const QVariantMap map = decoded.toMap();
        if (!map.contains(QStringLiteral("success")))
            return false;
        ok = map.value(QStringLiteral("success")).toBool();
        code = map.value(QStringLiteral("code")).toString();
        message = map.value(QStringLiteral("message")).toString();
        operationId = map.value(QStringLiteral("operation_id")).toString();
        if (operationId.isEmpty())
            operationId = map.value(QStringLiteral("operationId")).toString();
        return true;
    }
    return false;
}

QVariantMap UpdateInstaller::decodeRecoveryStringMap(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return {};
    const QVariant decoded = decodeDbusVariantRecursive(reply.arguments().constFirst());
    return decoded.userType() == QMetaType::QVariantMap ? decoded.toMap() : QVariantMap{};
}

QVariantMap UpdateInstaller::decodeRecoveryEnvelopeData(const QDBusMessage &reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return {};
    const QVariantMap top = decodeDbusVariantRecursive(reply.arguments().constFirst()).toMap();
    if (top.contains(QStringLiteral("data"))) {
        const QVariantMap inner = decodeDbusVariantRecursive(top.value(QStringLiteral("data"))).toMap();
        if (!inner.isEmpty())
            return inner;
    }
    return top;
}

bool UpdateInstaller::officialRepoReady() const
{
    return isOfficialRepoScanComplete(m_scanner);
}

bool UpdateInstaller::officialUpdatesAvailable() const
{
    if (!m_scanner || m_scanner->reports().isEmpty())
        return false;
    return !m_scanner->reports().at(0).toMap().value(QStringLiteral("packages")).toList().isEmpty();
}

bool UpdateInstaller::aurUpdatesAvailable() const
{
    if (!m_scanner || m_scanner->reports().size() < 2)
        return false;
    return !m_scanner->reports().at(1).toMap().value(QStringLiteral("packages")).toList().isEmpty();
}

bool UpdateInstaller::flatpakUpdatesAvailable() const
{
    if (!m_scanner || m_scanner->reports().size() < 3)
        return false;
    return !m_scanner->reports().at(2).toMap().value(QStringLiteral("packages")).toList().isEmpty();
}

bool UpdateInstaller::orphanCleanupAvailable() const
{
    return m_scanner && m_scanner->removableOrphanCount() > 0;
}

bool UpdateInstaller::isOfficialRepoScanComplete(const UpdateScanner *scanner, QString *reason)
{
    if (!scanner) {
        if (reason)
            *reason = QStringLiteral("Güncelleme tarayıcısı kullanılamıyor; resmî depo durumu doğrulanamadı.");
        return false;
    }

    const QVariantList reports = scanner->reports();
    if (reports.isEmpty()) {
        if (reason)
            *reason = QStringLiteral("Resmî depo tarama raporu eksik; tam sistem yükseltmesi başlatılamaz.");
        return false;
    }

    const QVariantMap official = reports.at(0).toMap();
    const QString state = official.value(QStringLiteral("state")).toString();
    if (state != QStringLiteral("complete")) {
        if (reason) {
            const QString detail = official.value(QStringLiteral("detail")).toString().trimmed();
            if (state == QStringLiteral("pending") || state == QStringLiteral("scanning")) {
                *reason = QStringLiteral("Resmî depo taraması henüz tamamlanmadı; tam sistem yükseltmesi başlatılamaz.");
            } else if (state == QStringLiteral("disabled")) {
                *reason = QStringLiteral("Resmî depo taraması kapalı veya eksik; tam sistem yükseltmesi doğrulanamıyor.");
            } else {
                *reason = detail.isEmpty()
                    ? QStringLiteral("Resmî depo taraması doğrulanamadı (%1); kısmi veya kör yükseltmeye izin verilmiyor.").arg(state)
                    : QStringLiteral("Resmî depo taraması doğrulanamadı (%1); kısmi veya kör yükseltmeye izin verilmiyor. %2").arg(state, detail);
            }
        }
        return false;
    }
    return true;
}

bool UpdateInstaller::isCriticalBootUpdate(const UpdateScanner *scanner)
{
    if (!scanner)
        return true;

    static const QRegularExpression criticalPkgRe(
        QStringLiteral("^(?:linux(?:-[a-z0-9._-]+)?|linux-firmware.*|amd-ucode|intel-ucode|mkinitcpio.*|dracut.*|limine.*|grub.*|efibootmgr|shim.*|sbctl|cryptsetup|lvm2|systemd.*|btrfs-progs|snapper|snap-pac|limine-snapper-sync|pacman|glibc)$"),
        QRegularExpression::CaseInsensitiveOption);

    const QVariantList reports = scanner->reports();
    if (reports.isEmpty())
        return true;

    for (const QVariant &repVal : reports) {
        const QVariantMap rep = repVal.toMap();
        const QString state = rep.value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("complete") && state != QStringLiteral("disabled"))
            return true;

        const QVariantList pkgs = rep.value(QStringLiteral("packages")).toList();
        for (const QVariant &pkgVal : pkgs) {
            const QString pkgName = pkgVal.typeId() == QMetaType::QVariantMap
                ? pkgVal.toMap().value(QStringLiteral("name")).toString()
                : pkgVal.toString().section(QLatin1Char(' '), 0, 0);
            if (criticalPkgRe.match(pkgName.trimmed()).hasMatch())
                return true;
        }
    }
    return false;
}

UpdateInstaller::UpdateInstaller(UpdateScanner *scanner, QObject *parent)
    : QObject(parent), m_scanner(scanner),
      m_sessionTool(QStandardPaths::findExecutable("script")),
      m_updater(QStandardPaths::findExecutable("arch-update")),
      m_recoveryGate(runDefaultRecoveryPreGate),
      m_recoveryComplete(runDefaultRecoveryComplete),
      m_healthProbe(runDefaultPostUpdateHealthProbe)
{
    if (m_updater.isEmpty())
        m_updater = QStandardPaths::findExecutable("cachy-update");

    if (m_scanner)
        connect(m_scanner, &UpdateScanner::changed, this, &UpdateInstaller::changed);

    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &UpdateInstaller::readOutput);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_busy)
            finish("error", "Kurulum başlatılamadı: " + m_process.errorString());
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus state) {
        readOutput();
        if (!m_busy)
            return;
        const bool isOrphanScope = (m_activeScope == QStringLiteral("orphans"));
        const bool isHelperScope = (m_activeScope == QStringLiteral("official") || isOrphanScope);
        if (state != QProcess::NormalExit) {
            finish("unknown", isOrphanScope
                ? QStringLiteral("Yetim paket temizliği beklenmedik biçimde sonlandı. Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.")
                : QStringLiteral("Kurulum işlemi beklenmedik biçimde sonlandı. Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin."));
        } else if (code == 0) {
            static const QRegularExpression verifiedErrorRe(
                QStringLiteral("(?:an error has occurred during the update process"
                               "|güncelleme sırasında bir hata oluştu"
                               "|failed to commit transaction"
                               "|failed to prepare transaction"
                               "|could not satisfy dependencies"
                               "|unable to lock database"
                               "|invalid or corrupted package"
                               "|signature from .* is unknown trust"
                               "|failed to synchronize all databases"
                               "|==>\\s*(?:ERROR|HATA):"
                               "|error:\\s*Failed to (?:install|update)"
                               "|işlem gerçekleştirilemedi"
                               "|bağımlılıklar karşılanamadı"
                               "|veritabanı kilitlenemedi)"),
                QRegularExpression::CaseInsensitiveOption);
            if (isOrphanScope) {
                if (m_orphanCleanupNoop) {
                    finish("finished", QStringLiteral("Kaldırılabilir yetim paket bulunmadı; değişiklik yapılmadı."));
                } else if (verifiedErrorRe.match(m_output).hasMatch()) {
                    finish("error", QStringLiteral("Yetim paket temizliği çıktısında işlem hatası tespit edildi. Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin."));
                } else if (m_verifiedSnapshotId > 0 && m_postSnapshotId > 0) {
                    finish("finished",
                           QStringLiteral("Yetim paket temizliği başarıyla tamamlandı (Pre-Snapshot #%1 → Post-Snapshot #%2).")
                               .arg(m_verifiedSnapshotId)
                               .arg(m_postSnapshotId));
                } else {
                    finish("finished", QStringLiteral("Yetim paket temizliği tamamlandı. Paket durumu yenilenen rapordan kontrol edilebilir."));
                }
            } else if (verifiedErrorRe.match(m_output).hasMatch()) {
                finish("error", "Kurulum çıktısında işlem hatası tespit edildi. Bazı paketler değişmiş veya güncellenmemiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.");
            } else if (m_output.contains(QRegularExpression(QStringLiteral("(?:reboot required|yeniden başlatma gerekiyor|rebooting to finalize|yeniden başlatmayı düşünün)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Güncellemeler uygulandı. Çekirdek güncellemesinin tamamlanması için sistemi yeniden başlatmanız gerekir.");
            } else if (m_activeScope == QStringLiteral("official") && m_verifiedSnapshotId > 0 && m_postSnapshotId > 0) {
                finish("finished",
                       QStringLiteral("Resmî güncellemeler başarıyla uygulandı (Pre-Snapshot #%1 → Post-Snapshot #%2).")
                           .arg(m_verifiedSnapshotId)
                           .arg(m_postSnapshotId));
            } else if (m_output.contains(QRegularExpression(QStringLiteral("(?:the update has been applied|güncelleme uygulandı)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Güncellemeler başarıyla kuruldu. Paket durumu yenilenen raporu inceleyerek kontrol edilebilir.");
            } else {
                finish("finished", "Kurulum oturumu sona erdi. Paket durumunu yenilenen rapordan ve işlem çıktısından kontrol edin.");
            }
        } else if ((isHelperScope && code == 17) || m_pacmanLockBlocked) {
            m_pacmanLockBlocked = true;
            finish("blocked",
                   (isOrphanScope
                        ? QStringLiteral("Aktif pacman kilidi (%1) tespit edildi; başka bir paket işlemi sürerken yetim paket temizliği güvenle ertelendi.")
                        : QStringLiteral("Aktif pacman kilidi (%1) tespit edildi; başka bir paket işlemi sürerken yükseltme güvenle ertelendi."))
                       .arg(m_pacmanLockPath));
        } else if (isHelperScope && code == 11) {
            finish("error", isOrphanScope
                ? QStringLiteral("Yetim paket temizliği öncesi zorunlu Snapper anlık görüntüsü alınamadı; kaldırma işlemi güvenlik nedeniyle başlatılmadı.")
                : QStringLiteral("Güncelleme öncesi zorunlu Snapper anlık görüntüsü alınamadı; paket yükseltmesi güvenlik nedeniyle başlatılmadı."));
        } else if (isHelperScope && code == 12) {
            finish("error", isOrphanScope
                ? QStringLiteral("Yetim paket temizliği tamamlandı ancak işlem sonrası Snapper anlık görüntüsü doğrulanamadı; çıktıyı ve Recovery sekmesini inceleyin.")
                : QStringLiteral("Paket yükseltmesi tamamlandı ancak güncelleme sonrası Snapper anlık görüntüsü doğrulanamadı; çıktıyı ve Recovery sekmesini inceleyin."));
        } else if (code == 4 || code == 17) {
            finish("cancelled", "Güncelleme işlemi kullanıcı tarafından iptal edildi veya başka bir oturum nedeniyle durduruldu. Rapor yeniden taramayla güncellenir.");
        } else if (code == 2 || code == 3 || code == 126 || code == 127) {
            finish("error", "Yönetici yetkisi alınamadı (yetkilendirme başarısız oldu veya reddedildi).");
        } else if (code == 5) {
            finish("error", isOrphanScope
                ? QStringLiteral("Yetim paket kaldırma (pacman -Rns) sırasında hata oluştu. Bazı paketler değişmiş olabilir; işlem çıktısını ve Recovery sekmesini inceleyin.")
                : QStringLiteral("Resmî paket yükseltmesi sırasında hata oluştu. Bazı paketler değişmiş olabilir; işlem çıktısını ve Recovery sekmesini inceleyin."));
        } else if (code == 6) {
            finish("error", "Yeniden başlatma işlemi başlatılamadı. Sistemi elle yeniden başlatabilirsiniz.");
        } else {
            finish("error", QString("Kurulum hata veya kesintiyle sonlandı (çıkış kodu %1). Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.").arg(code));
        }
    });
}

UpdateInstaller::~UpdateInstaller()
{
    if (m_busy) {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.terminate();
            if (!m_process.waitForFinished(1000)) {
                m_process.kill();
                m_process.waitForFinished(1000);
            }
        }
        if (m_usedRecoveryGate && m_recoveryGateEnabled && m_recoveryComplete && !m_updateOperationId.isEmpty()) {
            m_recoveryComplete(m_updateOperationId, QStringLiteral("unknown"));
        }
    }
}

void UpdateInstaller::cancel()
{
    if (!m_busy || m_process.state() == QProcess::NotRunning)
        return;
    m_process.terminate();
    if (!m_process.waitForFinished(2000))
        m_process.kill();
}

void UpdateInstaller::start()
{
    startScoped(QStringLiteral("all"));
}

void UpdateInstaller::startScoped(const QString &scope)
{
    if (m_busy || !m_scanner || m_scanner->busy())
        return;

    const QString normScope = scope.trimmed().toLower();
    if (normScope != QStringLiteral("all")
        && normScope != QStringLiteral("official")
        && normScope != QStringLiteral("aur")
        && normScope != QStringLiteral("flatpak")) {
        m_status = QStringLiteral("error");
        m_message = QStringLiteral("Geçersiz güncelleme kapsamı.");
        emit changed();
        emit sessionFinished();
        return;
    }

    if (normScope == QStringLiteral("all") && !m_scanner->hasUpdates())
        return;
    if (normScope == QStringLiteral("official") && !officialUpdatesAvailable())
        return;
    if (normScope == QStringLiteral("aur") && !aurUpdatesAvailable())
        return;
    if (normScope == QStringLiteral("flatpak") && !flatpakUpdatesAvailable())
        return;

    const bool needsRootRepo = (normScope != QStringLiteral("flatpak"));
    const bool usePrivilegedHelper = (normScope == QStringLiteral("official"));
    QString targetProgram;
    QStringList targetArgs;

    if (needsRootRepo) {
        QString officialRepoReason;
        if (!isOfficialRepoScanComplete(m_scanner, &officialRepoReason)) {
            m_status = QStringLiteral("error");
            m_progressStage = QStringLiteral("error");
            m_message = officialRepoReason;
            emit changed();
            emit sessionFinished();
            return;
        }

        if (!m_pacmanLockPath.isEmpty() && QFile::exists(m_pacmanLockPath)) {
            m_pacmanLockBlocked = true;
            m_status = QStringLiteral("blocked");
            m_progressStage = QStringLiteral("blocked");
            m_progressPercent = 0;
            m_message = QStringLiteral("Aktif pacman kilidi (%1) tespit edildi; başka bir paket işlemi sürerken yükseltme güvenle ertelendi.")
                .arg(m_pacmanLockPath);
            emit changed();
            emit sessionFinished();
            return;
        }
    }

    if (!usePrivilegedHelper) {
        if (m_sessionTool.isEmpty())
            m_sessionTool = QStandardPaths::findExecutable("script");
        if (m_sessionTool.isEmpty()) {
            m_status = "error";
            m_progressStage = QStringLiteral("error");
            m_message = "Kurulum için gerekli oturum aracı (script) bulunamadı.";
            emit changed();
            emit sessionFinished();
            return;
        }
    }

    if (usePrivilegedHelper) {
        const QString pkexecBin = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        const QFileInfo helperInfo(m_updateHelperPath);
        if (pkexecBin.isEmpty()) {
            m_status = QStringLiteral("error");
            m_progressStage = QStringLiteral("error");
            m_message = QStringLiteral("Resmî depo yükseltmesi için pkexec bulunamadı.");
            emit changed();
            emit sessionFinished();
            return;
        }
        if (!helperInfo.exists() || !helperInfo.isExecutable()) {
            m_status = QStringLiteral("error");
            m_progressStage = QStringLiteral("error");
            m_message = QStringLiteral("Yetkili güncelleme yardımcısı (%1) bulunamadı veya çalıştırılabilir değil.")
                            .arg(m_updateHelperPath);
            emit changed();
            emit sessionFinished();
            return;
        }
        targetProgram = pkexecBin;
        targetArgs = {m_updateHelperPath, QStringLiteral("apply-official")};
    } else if (normScope == QStringLiteral("aur")) {
        if (officialUpdatesAvailable()) {
            m_status = QStringLiteral("error");
            m_progressStage = QStringLiteral("error");
            m_message = QStringLiteral("Önce bekleyen resmî depo yükseltmesi tamamlanmadan AUR güncellemeleri başlatılamaz.");
            emit changed();
            emit sessionFinished();
            return;
        }
        QString aurHelper;
        for (const auto &candidate : {"paru", "yay", "pikaur"}) {
            aurHelper = QStandardPaths::findExecutable(candidate);
            if (!aurHelper.isEmpty())
                break;
        }
        if (aurHelper.isEmpty()) {
            m_status = QStringLiteral("error");
            m_progressStage = QStringLiteral("error");
            m_message = QStringLiteral("AUR güncellemeleri için AUR yardımcısı bulunamadı.");
            emit changed();
            emit sessionFinished();
            return;
        }
        targetProgram = aurHelper;
        targetArgs = {QStringLiteral("--aur"), QStringLiteral("-Sua")};
    } else if (normScope == QStringLiteral("flatpak")) {
        const QString flatpakBin = QStandardPaths::findExecutable(QStringLiteral("flatpak"));
        if (flatpakBin.isEmpty()) {
            m_status = QStringLiteral("error");
            m_progressStage = QStringLiteral("error");
            m_message = QStringLiteral("Flatpak aracı bulunamadı.");
            emit changed();
            emit sessionFinished();
            return;
        }
        targetProgram = flatpakBin;
        targetArgs = {QStringLiteral("update")};
    } else {
        if (m_updater.isEmpty()) {
            m_updater = QStandardPaths::findExecutable("arch-update");
            if (m_updater.isEmpty())
                m_updater = QStandardPaths::findExecutable("cachy-update");
        }
        if (m_updater.isEmpty()) {
            m_status = "error";
            m_progressStage = QStringLiteral("error");
            m_message = "Kurulum için gerekli sistem araçları bulunamadı.";
            emit changed();
            return;
        }
        targetProgram = m_updater;
    }

    m_activeScope = normScope;
    m_updateOperationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_verifiedSnapshotId = 0;
    m_postSnapshotId = 0;
    m_pacmanLockBlocked = false;
    m_limineSyncWarning = false;
    m_orphanCleanupNoop = false;
    m_usedRecoveryGate = false;
    m_progressStage = QStringLiteral("preflight");
    m_progressPercent = 5;
    m_recoveryGuidance.clear();
    m_healthStatus.clear();
    m_healthSummary.clear();
    m_cleanupStatus.clear();
    m_cleanupSummary.clear();
    m_rebootStatus.clear();
    m_rebootRequired = false;
    m_rebootSafetyNotice.clear();

    if (usePrivilegedHelper) {
        if (!m_liveExecutionEnabled) {
            m_status = QStringLiteral("blocked");
            m_progressStage = QStringLiteral("blocked");
            m_progressPercent = 0;
            m_message = QStringLiteral("Canlı güncelleme yürütmesi şu anda kapalıdır.");
            emit changed();
            emit sessionFinished();
            return;
        }
    } else if (needsRootRepo && m_recoveryGateEnabled) {
        const bool criticalBoot = isCriticalBootUpdate(m_scanner);
        const PreUpdateGateResult gate = m_recoveryGate
            ? m_recoveryGate(m_updateOperationId, criticalBoot)
            : PreUpdateGateResult{false, QStringLiteral("not-available"),
                                  QStringLiteral("Recovery ön-kapısı yapılandırılmadı."), 0};
        if (!gate.ok || gate.snapshotId == 0) {
            m_status = "error";
            m_progressStage = QStringLiteral("error");
            m_progressPercent = 0;
            m_message = gate.message.isEmpty()
                ? QStringLiteral("Güncelleme öncesi Recovery doğrulaması başarısız; işlem engellendi.")
                : gate.message;
            emit changed();
            emit sessionFinished();
            return;
        }
        m_verifiedSnapshotId = gate.snapshotId;
        m_usedRecoveryGate = true;

        if (!m_liveExecutionEnabled) {
            if (m_recoveryComplete)
                m_recoveryComplete(m_updateOperationId, QStringLiteral("aborted"));
            m_status = "blocked";
            m_progressStage = QStringLiteral("blocked");
            m_progressPercent = 0;
            m_message = QStringLiteral("Güncelleme öncesi snapshot #%1 doğrulandı, ancak canlı güncelleme yürütmesi şu anda kapalıdır.")
                            .arg(m_verifiedSnapshotId);
            emit changed();
            emit sessionFinished();
            return;
        }
    } else if (!needsRootRepo && !m_liveExecutionEnabled) {
        m_status = QStringLiteral("blocked");
        m_progressStage = QStringLiteral("blocked");
        m_progressPercent = 0;
        m_message = QStringLiteral("Canlı güncelleme yürütmesi şu anda kapalıdır.");
        emit changed();
        emit sessionFinished();
        return;
    }

    m_rawOutput.clear();
    m_output.clear();
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    emit outputChanged();

    auto environment = QProcessEnvironment::systemEnvironment();
    // Strip both snap-pac and legacy autosnap skip variables; pin arch-update libdir to root-owned system path.
    environment.remove(QStringLiteral("SKIP_AUTOSNAP"));
    environment.remove(QStringLiteral("SNAP_PAC_SKIP"));
    environment.insert(QStringLiteral("ARCH_UPDATE_LIBDIR"), QStringLiteral("/usr/share/arch-update/lib"));
    environment.insert("TERM", "dumb");
    environment.insert("NO_COLOR", "1");
    m_process.setProcessEnvironment(environment);

    if (usePrivilegedHelper) {
        m_interactiveSession = false;
        m_process.setProgram(targetProgram);
        m_process.setArguments(targetArgs);
        m_progressStage = QStringLiteral("preflight");
        m_progressPercent = 10;
        m_message = QStringLiteral("Yönetici onayı (Polkit) ve güncelleme öncesi kontroller bekleniyor…");
    } else {
        m_interactiveSession = true;
        m_process.setProgram(m_sessionTool);
        QStringList scriptArgs{"--quiet", "--return", "--flush", "--echo", "never",
                               "/dev/null", "--", targetProgram};
        scriptArgs.append(targetArgs);
        m_process.setArguments(scriptArgs);
        m_progressStage = QStringLiteral("upgrading");
        m_progressPercent = 25;
        m_message = "Kurulum Control Center üzerinde sürüyor. İşlem çıktısındaki soruları aşağıdaki yanıt alanından cevaplayın.";
    }

    m_busy = true;
    m_scanner->m_installationRunning = true;
    m_status = "running";
    emit changed();
    m_process.start();
}

void UpdateInstaller::startOrphanCleanup(bool userConfirmed)
{
    if (m_busy)
        return;

    if (!userConfirmed) {
        m_status = QStringLiteral("error");
        m_progressStage = QStringLiteral("error");
        m_progressPercent = 0;
        m_message = QStringLiteral("Yetim paket temizliği için açık kullanıcı onayı gereklidir.");
        emit changed();
        emit sessionFinished();
        return;
    }

    if (!m_scanner || m_scanner->busy())
        return;

    if (!orphanCleanupAvailable()) {
        m_status = QStringLiteral("error");
        m_progressStage = QStringLiteral("error");
        m_progressPercent = 0;
        m_message = QStringLiteral("Kaldırılabilir yetim paket bulunamadı.");
        emit changed();
        emit sessionFinished();
        return;
    }

    if (!m_pacmanLockPath.isEmpty() && QFile::exists(m_pacmanLockPath)) {
        m_activeScope = QStringLiteral("orphans");
        m_pacmanLockBlocked = true;
        m_status = QStringLiteral("blocked");
        m_progressStage = QStringLiteral("blocked");
        m_progressPercent = 0;
        m_cleanupStatus = QStringLiteral("blocked");
        m_cleanupSummary = QStringLiteral("Aktif pacman kilidi (%1) nedeniyle yetim paket temizliği güvenle ertelendi.")
                               .arg(m_pacmanLockPath);
        m_message = QStringLiteral("Aktif pacman kilidi (%1) tespit edildi; başka bir paket işlemi sürerken yetim paket temizliği güvenle ertelendi.")
                        .arg(m_pacmanLockPath);
        emit changed();
        emit sessionFinished();
        return;
    }

    const QString pkexecBin = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    const QFileInfo helperInfo(m_updateHelperPath);
    if (pkexecBin.isEmpty()) {
        m_status = QStringLiteral("error");
        m_progressStage = QStringLiteral("error");
        m_message = QStringLiteral("Yetim paket temizliği için pkexec bulunamadı.");
        emit changed();
        emit sessionFinished();
        return;
    }
    if (!helperInfo.exists() || !helperInfo.isExecutable()) {
        m_status = QStringLiteral("error");
        m_progressStage = QStringLiteral("error");
        m_message = QStringLiteral("Yetkili güncelleme yardımcısı (%1) bulunamadı veya çalıştırılabilir değil.")
                        .arg(m_updateHelperPath);
        emit changed();
        emit sessionFinished();
        return;
    }

    m_activeScope = QStringLiteral("orphans");
    m_updateOperationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_verifiedSnapshotId = 0;
    m_postSnapshotId = 0;
    m_pacmanLockBlocked = false;
    m_limineSyncWarning = false;
    m_orphanCleanupNoop = false;
    m_usedRecoveryGate = false;
    m_progressStage = QStringLiteral("preflight");
    m_progressPercent = 5;
    m_recoveryGuidance.clear();
    m_healthStatus.clear();
    m_healthSummary.clear();
    m_cleanupStatus.clear();
    m_cleanupSummary.clear();
    m_rebootStatus.clear();
    m_rebootRequired = false;
    m_rebootSafetyNotice.clear();

    if (!m_liveExecutionEnabled) {
        m_status = QStringLiteral("blocked");
        m_progressStage = QStringLiteral("blocked");
        m_progressPercent = 0;
        m_message = QStringLiteral("Canlı yetim paket temizliği yürütmesi şu anda kapalıdır.");
        emit changed();
        emit sessionFinished();
        return;
    }

    m_rawOutput.clear();
    m_output.clear();
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    emit outputChanged();

    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("SKIP_AUTOSNAP"));
    environment.remove(QStringLiteral("SNAP_PAC_SKIP"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    environment.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    m_process.setProcessEnvironment(environment);

    m_interactiveSession = false;
    m_process.setProgram(pkexecBin);
    // Never pass package names on the command line: the privileged helper discovers and validates live orphans itself.
    m_process.setArguments({m_updateHelperPath, QStringLiteral("remove-orphans")});
    m_progressStage = QStringLiteral("preflight");
    m_progressPercent = 10;
    m_message = QStringLiteral("Yönetici onayı (Polkit) ve yetim paket temizliği ön kontrolleri bekleniyor…");

    m_busy = true;
    m_scanner->m_installationRunning = true;
    m_status = QStringLiteral("running");
    emit changed();
    m_process.start();
}

void UpdateInstaller::sendInput(const QString &text)
{
    if (!m_busy || m_process.state() != QProcess::Running
        || text.contains('\n') || text.contains('\r'))
        return;
    if (m_process.write(text.toUtf8() + '\n') < 0) {
        m_message = "Yanıt gönderilemedi. İşlem çıktısını kontrol edin.";
        emit changed();
    }
}

void UpdateInstaller::sendKey(const QString &text)
{
    if (!m_busy || m_process.state() != QProcess::Running || text.size() != 1
        || text.contains('\n') || text.contains('\r'))
        return;
    m_process.write(text.toUtf8());
}

void UpdateInstaller::readOutput()
{
    m_rawOutput += m_decoder.decode(m_process.readAllStandardOutput());
    if (m_rawOutput.size() > 256 * 1024)
        m_rawOutput = m_rawOutput.right(256 * 1024);
    auto text = m_rawOutput;
    text.remove(QRegularExpression("\\x1b\\][^\\x07]*(?:\\x07|\\x1b\\\\)"));
    text.remove(QRegularExpression("\\x1b\\[[0-?]*[ -/]*[@-~]"));
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    text.remove(QRegularExpression("[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f\\x7f]"));
    text.replace(QRegularExpression("(?:cachy|arch)[- ]update", QRegularExpression::CaseInsensitiveOption),
                 "Güncelleme");
    m_output = text;
    emit outputChanged();

    const bool isHelperScope = (m_activeScope == QStringLiteral("official")
                                || m_activeScope == QStringLiteral("orphans"));
    if (isHelperScope) {
        parseHelperMarkers(text);
    }

    if (m_busy && !isHelperScope) {
        const QString tail = text.right(4096);
        QString stage;
        if (tail.contains(QRegularExpression(QStringLiteral("(?:reboot now|yeniden başlatmak ister misiniz|rebooting in|saniye içinde yeniden başlıyor)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Yeniden başlatma onayı bekleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:select the service|servisleri seçin|restarting services)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Servis yeniden başlatma onayı bekleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:removing old cached|önbelleğe alınan eski paketler|cached packages)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Paket önbelleği temizleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:removing flatpak unused|kullanılmayan flatpak paketleri kaldırılıyor|flatpak unused packages)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Kullanılmayan Flatpak paketleri temizleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:removing orphan packages|yetim paketler kaldırılıyor|orphan packages)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Yetim paketler kontrol ediliyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:updating flatpak packages|flatpak paketleri güncelleniyor)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Flatpak paketleri güncelleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:updating aur packages|aur paketleri güncelleniyor)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("AUR paketleri güncelleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:updating packages|paketler güncelleniyor)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Resmî depo paketleri güncelleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:proceed with update|güncellemeye devam edilsin mi|proceed with installation)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Güncelleme onayı bekleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:password for|parola:)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Yönetici parolası bekleniyor…");
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:looking for updates|güncellemeler aranıyor)"), QRegularExpression::CaseInsensitiveOption))) {
            stage = QStringLiteral("Güncellemeler kontrol ediliyor…");
        }

        if (!stage.isEmpty() && m_message != stage) {
            m_message = stage;
            emit changed();
        }
    }
}

void UpdateInstaller::parseHelperMarkers(const QString &text)
{
    bool stateChanged = false;
    auto updateStage = [&](const QString &stage, int pct, const QString &msg) {
        if (m_progressStage != stage) {
            m_progressStage = stage;
            stateChanged = true;
        }
        if (pct >= 0 && pct != m_progressPercent && (pct >= m_progressPercent || stage == QStringLiteral("blocked"))) {
            m_progressPercent = pct;
            stateChanged = true;
        }
        if (!msg.isEmpty() && m_message != msg) {
            m_message = msg;
            stateChanged = true;
        }
    };

    static const QRegularExpression markerRe(
        QStringLiteral("^\\[ZETHROPOL-UPDATE\\]\\s+STAGE=([a-z-]+)(.*)$"));
    static const QRegularExpression statusRe(QStringLiteral("\\bSTATUS=([a-z-]+)"));
    static const QRegularExpression reasonRe(QStringLiteral("\\bREASON=([a-z-]+)"));
    static const QRegularExpression snapIdRe(QStringLiteral("\\bSNAPSHOT_ID=([0-9]+)"));
    static const QRegularExpression preSnapIdRe(QStringLiteral("\\bPRE_SNAPSHOT_ID=([0-9]+)"));
    static const QRegularExpression postSnapIdRe(QStringLiteral("\\bPOST_SNAPSHOT_ID=([0-9]+)"));

    const bool isOrphanScope = (m_activeScope == QStringLiteral("orphans"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        const auto m = markerRe.match(line);
        if (m.hasMatch()) {
            const QString stage = m.captured(1);
            const QString rest = m.captured(2);
            const QString status = statusRe.match(rest).captured(1);
            const QString reason = reasonRe.match(rest).captured(1);

            const auto preMatch = preSnapIdRe.match(rest);
            if (preMatch.hasMatch()) {
                const quint32 parsed = preMatch.captured(1).toUInt();
                if (parsed > 0 && m_verifiedSnapshotId != parsed) {
                    m_verifiedSnapshotId = parsed;
                    stateChanged = true;
                }
            }
            const auto postMatch = postSnapIdRe.match(rest);
            if (postMatch.hasMatch()) {
                const quint32 parsed = postMatch.captured(1).toUInt();
                if (parsed > 0 && m_postSnapshotId != parsed) {
                    m_postSnapshotId = parsed;
                    stateChanged = true;
                }
            }

            if (stage == QStringLiteral("preflight") && status == QStringLiteral("ok")) {
                updateStage(QStringLiteral("preflight"), 15,
                            isOrphanScope
                                ? QStringLiteral("Ön kontroller tamamlandı; yetim paketler anlık olarak doğrulanıyor…")
                                : QStringLiteral("Ön kontroller tamamlandı; Snapper anlık görüntüsü hazırlanıyor…"));
            } else if (stage == QStringLiteral("blocked") && reason == QStringLiteral("pacman-lock")) {
                if (!m_pacmanLockBlocked) {
                    m_pacmanLockBlocked = true;
                    stateChanged = true;
                }
                updateStage(QStringLiteral("blocked"), 0,
                            isOrphanScope
                                ? QStringLiteral("Aktif pacman kilidi tespit edildi; yetim paket temizliği güvenle ertelendi.")
                                : QStringLiteral("Aktif pacman kilidi tespit edildi; yükseltme güvenle ertelendi."));
            } else if (stage == QStringLiteral("orphan-discovery")) {
                if (status == QStringLiteral("starting")) {
                    updateStage(QStringLiteral("orphan-discovery"), 18,
                                QStringLiteral("Anlık yetim paket listesi doğrulanıyor (pacman -Qdtq)…"));
                } else if (status == QStringLiteral("empty")) {
                    m_orphanCleanupNoop = true;
                    updateStage(QStringLiteral("orphan-discovery"), 90,
                                QStringLiteral("Kaldırılabilir yetim paket bulunmadı; değişiklik yapılmayacak."));
                } else if (status == QStringLiteral("ok")) {
                    updateStage(QStringLiteral("orphan-discovery"), 22,
                                QStringLiteral("Kaldırılabilir yetim paketler doğrulandı; Snapper anlık görüntüsü hazırlanıyor…"));
                } else if (status == QStringLiteral("failed")) {
                    updateStage(QStringLiteral("error"), m_progressPercent,
                                QStringLiteral("Yetim paket listesi doğrulanamadı."));
                }
            } else if (stage == QStringLiteral("orphan-filter") && status == QStringLiteral("protected-skipped")) {
                updateStage(m_progressStage, m_progressPercent,
                            QStringLiteral("Korumalı zethropol-* / çekirdek paketleri temizlik kapsamı dışında bırakıldı."));
            } else if (stage == QStringLiteral("pre-snapshot")) {
                if (status == QStringLiteral("starting")) {
                    updateStage(QStringLiteral("pre-snapshot"), isOrphanScope ? 25 : 20,
                                isOrphanScope
                                    ? QStringLiteral("Zorunlu Snapper temizlik öncesi anlık görüntüsü alınıyor…")
                                    : QStringLiteral("Zorunlu Snapper güncelleme öncesi anlık görüntüsü alınıyor…"));
                } else if (status == QStringLiteral("ok")) {
                    const quint32 snapId = snapIdRe.match(rest).captured(1).toUInt();
                    if (snapId > 0 && m_verifiedSnapshotId != snapId) {
                        m_verifiedSnapshotId = snapId;
                        stateChanged = true;
                    }
                    updateStage(QStringLiteral("pre-snapshot"), 35,
                                (isOrphanScope
                                     ? QStringLiteral("Temizlik öncesi Snapper anlık görüntüsü (#%1) oluşturuldu.")
                                     : QStringLiteral("Güncelleme öncesi Snapper anlık görüntüsü (#%1) oluşturuldu."))
                                    .arg(m_verifiedSnapshotId));
                } else if (status == QStringLiteral("failed")) {
                    updateStage(QStringLiteral("error"), m_progressPercent,
                                isOrphanScope
                                    ? QStringLiteral("Temizlik öncesi Snapper anlık görüntüsü alınamadı.")
                                    : QStringLiteral("Güncelleme öncesi Snapper anlık görüntüsü alınamadı."));
                }
            } else if (stage == QStringLiteral("pacman-upgrade")) {
                if (status == QStringLiteral("starting")) {
                    updateStage(QStringLiteral("upgrading"), 45,
                                QStringLiteral("Resmî depolar güncelleniyor (pacman -Syu --noconfirm)…"));
                } else if (status == QStringLiteral("ok")) {
                    updateStage(QStringLiteral("upgrading"), 75,
                                QStringLiteral("Paket yükseltmesi tamamlandı; güncelleme sonrası anlık görüntü hazırlanıyor…"));
                } else if (status == QStringLiteral("failed")) {
                    updateStage(QStringLiteral("error"), m_progressPercent,
                                QStringLiteral("Resmî paket yükseltmesi sırasında hata oluştu."));
                }
            } else if (stage == QStringLiteral("orphan-cleanup")) {
                if (status == QStringLiteral("starting")) {
                    updateStage(QStringLiteral("orphan-cleanup"), 50,
                                QStringLiteral("Yetim paketler kaldırılıyor (pacman -Rns --noconfirm)…"));
                } else if (status == QStringLiteral("ok")) {
                    updateStage(QStringLiteral("orphan-cleanup"), 75,
                                QStringLiteral("Yetim paketler kaldırıldı; işlem sonrası Snapper anlık görüntüsü hazırlanıyor…"));
                } else if (status == QStringLiteral("failed")) {
                    updateStage(QStringLiteral("error"), m_progressPercent,
                                QStringLiteral("Yetim paket kaldırma (pacman -Rns) sırasında hata oluştu."));
                }
            } else if (stage == QStringLiteral("post-snapshot")) {
                if (status == QStringLiteral("starting")) {
                    updateStage(QStringLiteral("post-snapshot"), 80,
                                isOrphanScope
                                    ? QStringLiteral("Temizlik sonrası Snapper anlık görüntüsü alınıyor…")
                                    : QStringLiteral("Güncelleme sonrası Snapper anlık görüntüsü alınıyor…"));
                } else if (status == QStringLiteral("ok")) {
                    const quint32 snapId = snapIdRe.match(rest).captured(1).toUInt();
                    if (snapId > 0 && m_postSnapshotId != snapId) {
                        m_postSnapshotId = snapId;
                        stateChanged = true;
                    }
                    updateStage(QStringLiteral("post-snapshot"), 90,
                                (isOrphanScope
                                     ? QStringLiteral("Temizlik sonrası Snapper anlık görüntüsü (#%1) oluşturuldu.")
                                     : QStringLiteral("Güncelleme sonrası Snapper anlık görüntüsü (#%1) oluşturuldu."))
                                    .arg(m_postSnapshotId));
                }
            } else if (stage == QStringLiteral("limine-sync")) {
                if (status == QStringLiteral("starting")) {
                    updateStage(QStringLiteral("limine-sync"), 92,
                                QStringLiteral("Limine önyükleyici girişleri eşitleniyor (limine-snapper-sync)…"));
                } else if (status == QStringLiteral("ok") || status == QStringLiteral("skipped")) {
                    updateStage(QStringLiteral("limine-sync"), 96, QString());
                } else if (status == QStringLiteral("warning")) {
                    m_limineSyncWarning = true;
                    updateStage(QStringLiteral("limine-sync"), 96,
                                QStringLiteral("Limine önyükleyici eşitlemesi uyarıyla tamamlandı."));
                }
            } else if (stage == QStringLiteral("complete") && status == QStringLiteral("noop")) {
                m_orphanCleanupNoop = true;
                updateStage(QStringLiteral("done"), 100,
                            QStringLiteral("Kaldırılabilir yetim paket bulunmadı; değişiklik yapılmadı."));
            } else if (stage == QStringLiteral("complete") && status == QStringLiteral("ok")) {
                updateStage(QStringLiteral("verifying"), 98,
                            isOrphanScope
                                ? QStringLiteral("Yetim paket temizliği sonrası sistem sağlığı doğrulanıyor…")
                                : QStringLiteral("Güncelleme sonrası sistem sağlığı doğrulanıyor…"));
            }
            continue;
        }

        // Monotonic progress hints during pacman execution.
        if (m_progressStage == QStringLiteral("upgrading")) {
            if (line.contains(QStringLiteral("Synchronizing package databases"), Qt::CaseInsensitive) && m_progressPercent < 50) {
                m_progressPercent = 50;
                stateChanged = true;
            } else if (line.contains(QStringLiteral("Starting full system upgrade"), Qt::CaseInsensitive) && m_progressPercent < 55) {
                m_progressPercent = 55;
                stateChanged = true;
            } else if (line.contains(QStringLiteral("Retrieving packages"), Qt::CaseInsensitive) && m_progressPercent < 60) {
                m_progressPercent = 60;
                stateChanged = true;
            } else if ((line.contains(QStringLiteral("checking keys in keyring"), Qt::CaseInsensitive)
                        || line.contains(QStringLiteral("checking package integrity"), Qt::CaseInsensitive))
                       && m_progressPercent < 65) {
                m_progressPercent = 65;
                stateChanged = true;
            } else if (line.contains(QStringLiteral("Processing package changes"), Qt::CaseInsensitive) && m_progressPercent < 70) {
                m_progressPercent = 70;
                stateChanged = true;
            } else if (line.contains(QStringLiteral("Running post-transaction hooks"), Qt::CaseInsensitive) && m_progressPercent < 74) {
                m_progressPercent = 74;
                stateChanged = true;
            }
        } else if (m_progressStage == QStringLiteral("orphan-cleanup")) {
            if (line.contains(QStringLiteral("checking dependencies"), Qt::CaseInsensitive) && m_progressPercent < 55) {
                m_progressPercent = 55;
                stateChanged = true;
            } else if (line.contains(QStringLiteral("Processing package changes"), Qt::CaseInsensitive) && m_progressPercent < 65) {
                m_progressPercent = 65;
                stateChanged = true;
            } else if (line.contains(QStringLiteral("Running post-transaction hooks"), Qt::CaseInsensitive) && m_progressPercent < 74) {
                m_progressPercent = 74;
                stateChanged = true;
            }
        }
    }

    if (stateChanged)
        emit changed();
}

void UpdateInstaller::finish(const QString &status, const QString &message)
{
    m_busy = false;
    m_interactiveSession = false;
    m_status = status;
    m_message = message;
    if (status == QStringLiteral("finished")) {
        m_progressStage = QStringLiteral("done");
        m_progressPercent = 100;
    } else if (status == QStringLiteral("blocked")) {
        m_progressStage = QStringLiteral("blocked");
        m_progressPercent = 0;
    } else if (status == QStringLiteral("cancelled")) {
        m_progressStage = QStringLiteral("cancelled");
    } else {
        m_progressStage = QStringLiteral("error");
    }
    m_scanner->m_installationRunning = false;
    if (m_usedRecoveryGate && m_activeScope != QStringLiteral("flatpak")
        && m_recoveryGateEnabled && m_recoveryComplete && !m_updateOperationId.isEmpty()) {
        const QString outcome = (status == QStringLiteral("finished"))
            ? QStringLiteral("ok")
            : (status == QStringLiteral("unknown") ? QStringLiteral("unknown") : QStringLiteral("failed"));
        m_recoveryComplete(m_updateOperationId, outcome);
    }
    const bool isOrphanScope = (m_activeScope == QStringLiteral("orphans"));
    if (status != QStringLiteral("finished") && m_verifiedSnapshotId > 0) {
        m_recoveryGuidance = (isOrphanScope
            ? QStringLiteral(
                "Yetim paket temizliği öncesi snapshot #%1 korundu. Kullanıcı verileri (/home) root geri yüklemesinden ayrı korunur, ancak sistem paketi geri alınırsa uygulama verilerinde (~/.config, ~/.local/share) sürüm uyumsuzluğu oluşabilir. Otomatik rollback veya onarım başlatılmadı; kurtarma için Recovery sekmesindeki ön kontrolü veya gerekirse haricî kurtarma/arch-chroot kullanın.")
            : QStringLiteral(
                "Güncelleme öncesi snapshot #%1 korundu. Kullanıcı verileri (/home) root geri yüklemesinden ayrı korunur, ancak sistem paketi geri alınırsa uygulama verilerinde (~/.config, ~/.local/share) sürüm uyumsuzluğu oluşabilir. Otomatik rollback veya onarım başlatılmadı; kurtarma için Recovery sekmesindeki ön kontrolü veya gerekirse haricî kurtarma/arch-chroot kullanın."))
                                 .arg(m_verifiedSnapshotId);
    } else if (status == QStringLiteral("finished") && m_verifiedSnapshotId > 0) {
        if (m_postSnapshotId > 0) {
            m_recoveryGuidance = (isOrphanScope
                ? QStringLiteral(
                    "Yetim paket temizliği öncesi snapshot #%1 ve sonrası snapshot #%2 korundu; sistem sağlığı bağımsız olarak doğrulanana kadar otomatik silinmez.")
                : QStringLiteral(
                    "Güncelleme öncesi snapshot #%1 ve sonrası snapshot #%2 korundu; sistem sağlığı bağımsız olarak doğrulanana kadar otomatik silinmez."))
                                     .arg(m_verifiedSnapshotId)
                                     .arg(m_postSnapshotId);
        } else {
            m_recoveryGuidance = (isOrphanScope
                ? QStringLiteral(
                    "Yetim paket temizliği öncesi snapshot #%1 korundu; sistem sağlığı bağımsız olarak doğrulanana kadar otomatik silinmez.")
                : QStringLiteral(
                    "Güncelleme öncesi snapshot #%1 korundu; sistem sağlığı bağımsız olarak doğrulanana kadar otomatik silinmez."))
                                     .arg(m_verifiedSnapshotId);
        }
    }
    evaluatePostUpdateState();
    emit changed();
    m_scanner->scan();
    emit sessionFinished();
}

void UpdateInstaller::evaluatePostUpdateState()
{
    PostUpdateHealthEvidence ev = m_healthProbe
        ? m_healthProbe(m_updateOperationId, m_verifiedSnapshotId, m_status)
        : PostUpdateHealthEvidence{};
    if (m_limineSyncWarning && !ev.bootVerified) {
        ev.bootDegraded = true;
        if (ev.bootDetail.isEmpty())
            ev.bootDetail = QStringLiteral("limine-snapper-sync uyarıyla tamamlandı; önyükleyici girişlerini Recovery sekmesinden doğrulayın.");
    }

    const bool rebootHintInOutput = m_output.contains(QRegularExpression(
        QStringLiteral("(?:reboot required|yeniden başlatma gerekiyor|rebooting to finalize|yeniden başlatmayı düşünün)"),
        QRegularExpression::CaseInsensitiveOption));
    const bool missingKernelImage = ev.activeKernelCheckDone && !ev.activeKernelImagePresent;

    const bool recoveryServerConfirmedClean =
        ev.recoveryAssessmentStatus == QStringLiteral("healthy")
        || (ev.recoveryAssessmentStatus == QStringLiteral("unknown")
            && ev.recoverySnapshotVerified
            && ev.bootVerified
            && ev.recoveryRecommendedLevel == QStringLiteral("none"));

    if (ev.pacmanLockPresent
        || ev.recoveryAssessmentStatus.startsWith(QStringLiteral("blocked"))
        || m_status == QStringLiteral("blocked")) {
        m_healthStatus = QStringLiteral("blocked");
        QStringList reasons;
        if (ev.pacmanLockPresent)
            reasons << QStringLiteral("/var/lib/pacman/db.lck kilidi mevcut");
        if (!ev.recoveryAssessmentSummary.isEmpty())
            reasons << ev.recoveryAssessmentSummary;
        m_healthSummary = reasons.isEmpty()
            ? QStringLiteral("Aktif güvenlik engeli veya belirsiz işlem durumu nedeniyle sistem sağlığı engelli.")
            : reasons.join(QStringLiteral(" "));
    } else if (m_status == QStringLiteral("error")
               || m_status == QStringLiteral("cancelled")
               || (ev.systemdChecked && ev.failedSystemdUnits > 0)
               || (ev.pacmanDbChecked && !ev.pacmanDbOk)
               || ev.bootDegraded
               || (missingKernelImage && !rebootHintInOutput)
               || ev.recoveryAssessmentStatus == QStringLiteral("recovery-recommended")
               || ev.recoveryAssessmentStatus == QStringLiteral("external-recovery-required")
               || ev.recoveryAssessmentStatus == QStringLiteral("snapshot-unavailable")) {
        m_healthStatus = QStringLiteral("degraded");
        QStringList reasons;
        if (m_status == QStringLiteral("error") || m_status == QStringLiteral("cancelled"))
            reasons << QStringLiteral("Güncelleme oturumu hatayla/kesintiyle sonuçlandı.");
        if (ev.systemdChecked && ev.failedSystemdUnits > 0)
            reasons << QStringLiteral("%1 başarısız systemd birimi tespit edildi.").arg(ev.failedSystemdUnits);
        if (ev.pacmanDbChecked && !ev.pacmanDbOk)
            reasons << (ev.pacmanDbDetail.isEmpty()
                            ? QStringLiteral("pacman -Dk yerel bağımlılık veritabanı hatası tespit edildi.")
                            : ev.pacmanDbDetail);
        if (ev.bootDegraded)
            reasons << (ev.bootDetail.isEmpty()
                            ? QStringLiteral("Boot, kernel/initramfs veya Limine doğrulaması başarısız.")
                            : ev.bootDetail);
        if (missingKernelImage && !rebootHintInOutput)
            reasons << QStringLiteral("Aktif çekirdek imajı bulunamadı; yalnızca yeniden başlatma gereksinimi olarak yorumlanamaz.");
        if (!ev.recoveryAssessmentSummary.isEmpty())
            reasons << ev.recoveryAssessmentSummary;
        m_healthSummary = reasons.isEmpty()
            ? QStringLiteral("Sistem sağlığında doğrulanmış sorun tespit edildi.")
            : reasons.join(QStringLiteral(" "));
    } else if (m_status == QStringLiteral("finished")
               && ev.systemdChecked && ev.failedSystemdUnits == 0
               && (!ev.pacmanDbChecked || ev.pacmanDbOk)
               && ev.activeKernelCheckDone && ev.activeKernelImagePresent
               && ev.bootVerified
               && recoveryServerConfirmedClean) {
        m_healthStatus = QStringLiteral("healthy");
        m_healthSummary = QStringLiteral(
            "Bağımsız sağlık kontrolleri (pacman kilidi yok, veritabanı tutarlı, 0 başarısız systemd birimi, doğrulanmış boot/Recovery durumu) geçti.");
    } else {
        m_healthStatus = QStringLiteral("unknown");
        QStringList gaps;
        if (m_status == QStringLiteral("unknown"))
            gaps << QStringLiteral("Güncelleme oturumu belirsiz sonlandı.");
        if (!ev.systemdChecked || ev.failedSystemdUnits < 0)
            gaps << (ev.systemdDetail.isEmpty() ? QStringLiteral("systemd birimleri doğrulanamadı.") : ev.systemdDetail);
        if (!ev.activeKernelCheckDone)
            gaps << QStringLiteral("Aktif çekirdek sürümü/dosyası doğrulanamadı.");
        if (!ev.bootVerified)
            gaps << (ev.bootDetail.isEmpty() ? QStringLiteral("Boot/Limine bütünlüğü bağımsız doğrulanamadı.") : ev.bootDetail);
        if (!recoveryServerConfirmedClean)
            gaps << QStringLiteral("Recovery sağlık değerlendirmesi kesin kanıt üretmedi.");
        m_healthSummary = gaps.isEmpty()
            ? QStringLiteral("Yeterli bağımsız sağlık kanıtı bulunamadı.")
            : gaps.join(QStringLiteral(" "));
    }

    if (m_activeScope == QStringLiteral("orphans")) {
        if (m_status == QStringLiteral("blocked")) {
            m_cleanupStatus = QStringLiteral("blocked");
            m_cleanupSummary = QStringLiteral(
                "Aktif pacman kilidi veya güvenlik engeli nedeniyle yetim paket temizliği güvenle ertelendi.");
        } else if (m_status != QStringLiteral("finished")) {
            m_cleanupStatus = QStringLiteral("failed");
            m_cleanupSummary = QStringLiteral(
                "Yetim paket temizliği hatayla veya kesintiyle sonlandı; alınan Snapper anlık görüntüleri ve günlükler korundu.");
        } else if (m_orphanCleanupNoop) {
            m_cleanupStatus = QStringLiteral("noop");
            m_cleanupSummary = QStringLiteral(
                "Kaldırılabilir yetim paket bulunmadı (veya yalnızca korumalı zethropol-*/çekirdek paketleri mevcuttu); snapshot ve paket değişikliği yapılmadı.");
        } else {
            m_cleanupStatus = QStringLiteral("completed");
            if (m_verifiedSnapshotId > 0 && m_postSnapshotId > 0) {
                m_cleanupSummary = QStringLiteral(
                    "Açık kullanıcı onayıyla yetim paket temizliği tamamlandı (Pre-Snapshot #%1 → Post-Snapshot #%2). Korumalı zethropol-* ve çekirdek paketleri hariç tutuldu.")
                                       .arg(m_verifiedSnapshotId)
                                       .arg(m_postSnapshotId);
            } else {
                m_cleanupSummary = QStringLiteral(
                    "Açık kullanıcı onayıyla yetim paket temizliği tamamlandı. Korumalı zethropol-* ve çekirdek paketleri hariç tutuldu.");
            }
        }
    } else if (m_status != QStringLiteral("finished")) {
        m_cleanupStatus = QStringLiteral("blocked");
        m_cleanupSummary = QStringLiteral(
            "Güncelleme başarılı veya doğrulanmış olmadığı için otomatik temizlik yapılmadı. "
            "Snapper snapshot'ları, günlükler, yetim paketler ve önbellek kanıtları korundu.");
    } else {
        m_cleanupStatus = QStringLiteral("unknown");
        m_cleanupSummary = QStringLiteral(
            "Control Center otomatik temizlik çalıştırmaz (snapshot, yetim paket, önbellek, boot ve günlükler silinmez); haricî temizlik sonucu ayrıca doğrulanmamıştır. "
            "Yetim paketler yalnızca tespit edilir; kaldırma ayrı açık kullanıcı onayı gerektirir.");
    }

    if (missingKernelImage || rebootHintInOutput) {
        m_rebootRequired = true;
        m_rebootStatus = QStringLiteral("recommended");
    } else if (!ev.activeKernelCheckDone || m_status == QStringLiteral("unknown")) {
        m_rebootRequired = false;
        m_rebootStatus = QStringLiteral("unknown");
    } else {
        m_rebootRequired = false;
        m_rebootStatus = QStringLiteral("not-required");
    }

    if (m_healthStatus == QStringLiteral("healthy") && m_rebootRequired) {
        m_rebootSafetyNotice = QStringLiteral(
            "Güncellemenin tamamlanması için yeniden başlatma gerekiyor. Otomatik yeniden başlatma yapılmaz; açık kullanıcı onayıyla yeniden başlatabilirsiniz.");
    } else if (m_healthStatus != QStringLiteral("healthy")) {
        m_rebootSafetyNotice = QStringLiteral(
            "Sistem sağlığı tam doğrulanmadığı için (%1) güvenli yeniden başlatma garantisi verilemez. "
            "Yeniden başlatma öncesi Recovery ön kontrolünü ve işlem çıktısını inceleyin; otomatik yeniden başlatma yapılmaz.")
                                   .arg(m_healthStatus);
    } else {
        m_rebootSafetyNotice = QStringLiteral("Bekleyen zorunlu çekirdek yeniden başlatması tespit edilmedi.");
    }
}
