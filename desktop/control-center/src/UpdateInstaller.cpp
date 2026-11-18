#include "UpdateInstaller.h"
#include "UpdateScanner.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>

namespace {

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
        30000);
    if (createReply.type() == QDBusMessage::ErrorMessage || createReply.arguments().size() < 4) {
        return {false, QStringLiteral("not-available"),
                QStringLiteral("Recovery servisi erişilemez veya geçersiz yanıt döndürdü; güncelleme engellendi."), 0};
    }
    const bool createOk = createReply.arguments().at(0).toBool();
    const QString createCode = createReply.arguments().at(1).toString();
    const QString createMsg = createReply.arguments().at(2).toString();
    const QString recoveryOpId = createReply.arguments().at(3).toString();
    if (!createOk || recoveryOpId.isEmpty()) {
        return {false, createCode, createMsg.isEmpty() ? QStringLiteral("Güncelleme öncesi snapshot başlatılamadı.") : createMsg, 0};
    }

    quint32 snapshotId = 0;
    for (int i = 0; i < 120; ++i) {
        const QDBusMessage stateReply = callRecovery(
            QStringLiteral("GetOperationState"), {recoveryOpId}, 5000);
        if (stateReply.type() == QDBusMessage::ErrorMessage || stateReply.arguments().isEmpty()) {
            return {false, QStringLiteral("not-available"),
                    QStringLiteral("Recovery işlem durumu sorgulanamadı; güncelleme engellendi."), 0};
        }
        const QDBusReply<QVariantMap> parsedState(stateReply);
        if (!parsedState.isValid()) {
            return {false, QStringLiteral("not-available"),
                    QStringLiteral("Recovery işlem durumu çözümlenemedi; güncelleme engellendi."), 0};
        }
        const QVariantMap opMap = parsedState.value();
        const QString status = opMap.value(QStringLiteral("status")).toString();
        const QString code = opMap.value(QStringLiteral("code")).toString();
        const QString message = opMap.value(QStringLiteral("message")).toString();
        if (status == QStringLiteral("ok")) {
            snapshotId = opMap.value(QStringLiteral("snapshot_id")).toString().toUInt();
            break;
        }
        if (status != QStringLiteral("running")) {
            return {false, code, message.isEmpty() ? QStringLiteral("Güncelleme öncesi snapshot doğrulanamadı.") : message, 0};
        }
        QThread::msleep(250);
    }
    if (snapshotId == 0) {
        return {false, QStringLiteral("verification-failed"),
                QStringLiteral("Güncelleme öncesi snapshot kimliği doğrulanamadı."), 0};
    }

    const QDBusMessage verifyReply = callRecovery(
        QStringLiteral("VerifyPreUpdateSnapshot"),
        {updateOpId, QVariant::fromValue(snapshotId), criticalBootUpdate},
        10000);
    if (verifyReply.type() == QDBusMessage::ErrorMessage || verifyReply.arguments().size() < 3) {
        return {false, QStringLiteral("verification-failed"),
                QStringLiteral("Snapshot son doğrulama kapısı başarısız; güncelleme engellendi."), snapshotId};
    }
    if (!verifyReply.arguments().at(0).toBool()) {
        return {false, verifyReply.arguments().at(1).toString(),
                verifyReply.arguments().at(2).toString(), snapshotId};
    }

    const QDBusMessage beginReply = callRecovery(
        QStringLiteral("BeginPreUpdateExecution"),
        {updateOpId, QVariant::fromValue(snapshotId)},
        5000);
    if (beginReply.type() == QDBusMessage::ErrorMessage || beginReply.arguments().size() < 3
        || !beginReply.arguments().at(0).toBool()) {
        const QString errMsg = beginReply.arguments().size() >= 3
            ? beginReply.arguments().at(2).toString()
            : QStringLiteral("Rezervasyon aktif güncelleme durumuna geçirilemedi.");
        return {false, QStringLiteral("invalid-state"), errMsg, snapshotId};
    }

    return {true, QStringLiteral("ok"), QStringLiteral("Doğrulandı"), snapshotId};
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

} // namespace

bool UpdateInstaller::officialRepoReady() const
{
    return isOfficialRepoScanComplete(m_scanner);
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
        QStringLiteral("^(?:linux(?:-[a-z0-9._-]+)?|mkinitcpio.*|dracut.*|limine.*|grub.*|efibootmgr|systemd.*|btrfs-progs|snapper|snap-pac|limine-snapper-sync|pacman|glibc)$"),
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
      m_recoveryComplete(runDefaultRecoveryComplete)
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
        if (state != QProcess::NormalExit) {
            finish("unknown", "Kurulum işlemi beklenmedik biçimde sonlandı. Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.");
        } else if (code == 0) {
            static const QRegularExpression verifiedErrorRe(
                QStringLiteral("(?:an error has occurred during the update process"
                               "|güncelleme sırasında bir hata oluştu"
                               "|failed to commit transaction"
                               "|failed to prepare transaction"
                               "|could not satisfy dependencies"
                               "|unable to lock database"
                               "|invalid or corrupted package"
                               "|failed to synchronize all databases"
                               "|==>\\s*ERROR:"
                               "|error:\\s*Failed to (?:install|update)"
                               "|işlem gerçekleştirilemedi"
                               "|bağımlılıklar karşılanamadı"
                               "|veritabanı kilitlenemedi)"),
                QRegularExpression::CaseInsensitiveOption);
            if (verifiedErrorRe.match(m_output).hasMatch()) {
                finish("error", "Kurulum çıktısında işlem hatası tespit edildi. Bazı paketler değişmiş veya güncellenmemiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.");
            } else if (m_output.contains(QRegularExpression(QStringLiteral("(?:reboot required|yeniden başlatma gerekiyor|rebooting to finalize|yeniden başlatmayı düşünün)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Güncellemeler uygulandı. Çekirdek güncellemesinin tamamlanması için sistemi yeniden başlatmanız gerekir.");
            } else if (m_output.contains(QRegularExpression(QStringLiteral("(?:the update has been applied|güncelleme uygulandı)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Güncellemeler başarıyla kuruldu. Paket durumu yenilenen raporu inceleyerek kontrol edilebilir.");
            } else {
                finish("finished", "Kurulum oturumu sona erdi. Paket durumunu yenilenen rapordan ve işlem çıktısından kontrol edin.");
            }
        } else if (code == 4 || code == 17) {
            finish("cancelled", "Güncelleme işlemi kullanıcı tarafından iptal edildi veya başka bir oturum nedeniyle durduruldu. Rapor yeniden taramayla güncellenir.");
        } else if (code == 2) {
            finish("error", "Yönetici yetkisi alınamadı (sudo yetkilendirmesi başarısız oldu veya yetki yükseltme aracı bulunamadı).");
        } else if (code == 5) {
            finish("error", "Resmî paket yükseltmesi sırasında hata oluştu. Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.");
        } else if (code == 6) {
            finish("error", "Yeniden başlatma işlemi başlatılamadı. Sistemi elle yeniden başlatabilirsiniz.");
        } else {
            finish("error", QString("Kurulum hata veya kesintiyle sonlandı (çıkış kodu %1). Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.").arg(code));
        }
    });
}

void UpdateInstaller::start()
{
    if (m_busy || m_scanner->busy() || !m_scanner->hasUpdates())
        return;
    if (m_sessionTool.isEmpty())
        m_sessionTool = QStandardPaths::findExecutable("script");
    if (m_updater.isEmpty()) {
        m_updater = QStandardPaths::findExecutable("arch-update");
        if (m_updater.isEmpty())
            m_updater = QStandardPaths::findExecutable("cachy-update");
    }
    if (m_sessionTool.isEmpty() || m_updater.isEmpty()) {
        m_status = "error";
        m_message = "Kurulum için gerekli sistem araçları bulunamadı.";
        emit changed();
        return;
    }

    QString officialRepoReason;
    if (!isOfficialRepoScanComplete(m_scanner, &officialRepoReason)) {
        m_status = QStringLiteral("error");
        m_message = officialRepoReason;
        emit changed();
        emit sessionFinished();
        return;
    }

    m_updateOperationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_verifiedSnapshotId = 0;
    m_recoveryGuidance.clear();

    if (m_recoveryGateEnabled) {
        const bool criticalBoot = isCriticalBootUpdate(m_scanner);
        const PreUpdateGateResult gate = m_recoveryGate
            ? m_recoveryGate(m_updateOperationId, criticalBoot)
            : PreUpdateGateResult{false, QStringLiteral("not-available"),
                                  QStringLiteral("Recovery ön-kapısı yapılandırılmadı."), 0};
        if (!gate.ok || gate.snapshotId == 0) {
            m_status = "error";
            m_message = gate.message.isEmpty()
                ? QStringLiteral("Güncelleme öncesi Recovery doğrulaması başarısız; işlem engellendi.")
                : gate.message;
            emit changed();
            emit sessionFinished();
            return;
        }
        m_verifiedSnapshotId = gate.snapshotId;

        if (!m_liveExecutionEnabled) {
            if (m_recoveryComplete)
                m_recoveryComplete(m_updateOperationId, QStringLiteral("aborted"));
            m_status = "blocked";
            m_message = QStringLiteral("Güncelleme öncesi snapshot #%1 doğrulandı, ancak 03.2.08 aşamasında canlı güncelleme yürütmesi kapalıdır.")
                            .arg(m_verifiedSnapshotId);
            emit changed();
            emit sessionFinished();
            return;
        }
    }

    m_rawOutput.clear();
    m_output.clear();
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    emit outputChanged();
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("SKIP_AUTOSNAP"));
    environment.insert("TERM", "dumb");
    environment.insert("NO_COLOR", "1");
    m_process.setProcessEnvironment(environment);
    m_process.setProgram(m_sessionTool);
    // Preserve the real controlling TTY required by sudo and package prompts.
    // Use argv directly, no shell interpolation. Never record input or output
    // to disk, and disable line echo so entered passwords are not displayed.
    m_process.setArguments({"--quiet", "--return", "--flush", "--echo", "never",
                            "/dev/null", "--", m_updater});
    m_busy = true;
    m_scanner->m_installationRunning = true;
    m_status = "running";
    m_message = "Kurulum Control Center üzerinde sürüyor. İşlem çıktısındaki soruları aşağıdaki yanıt alanından cevaplayın.";
    emit changed();
    m_process.start();
}

void UpdateInstaller::sendInput(const QString &text)
{
    if (!m_busy || m_process.state() != QProcess::Running
        || text.contains('\n') || text.contains('\r'))
        return;
    // Input is sent only to the live process; it is not appended to the report.
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
    // Pagers consume a single key. Do not leave an extra Enter that could
    // accidentally answer the following package confirmation.
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

    if (m_busy) {
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
        } else if (tail.contains(QRegularExpression(QStringLiteral("(?:proceed with update|güncellemeye devam edilsin mi)"), QRegularExpression::CaseInsensitiveOption))) {
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

void UpdateInstaller::finish(const QString &status, const QString &message)
{
    m_busy = false;
    m_status = status;
    m_message = message;
    m_scanner->m_installationRunning = false;
    if (m_recoveryGateEnabled && m_recoveryComplete && !m_updateOperationId.isEmpty()) {
        const QString outcome = (status == QStringLiteral("finished"))
            ? QStringLiteral("ok")
            : (status == QStringLiteral("unknown") ? QStringLiteral("unknown") : QStringLiteral("failed"));
        m_recoveryComplete(m_updateOperationId, outcome);
    }
    if (status != QStringLiteral("finished") && m_verifiedSnapshotId > 0) {
        m_recoveryGuidance = QStringLiteral(
            "Güncelleme öncesi snapshot #%1 korundu. Kullanıcı verileri (/home) root geri yüklemesinden ayrı korunur, ancak sistem paketi geri alınırsa uygulama verilerinde (~/.config, ~/.local/share) sürüm uyumsuzluğu oluşabilir. Otomatik rollback veya onarım başlatılmadı; kurtarma için Recovery ön kontrolü veya gerekirse haricî kurtarma/arch-chroot kullanılmalıdır.")
                                 .arg(m_verifiedSnapshotId);
    } else if (status == QStringLiteral("finished") && m_verifiedSnapshotId > 0) {
        m_recoveryGuidance = QStringLiteral(
            "Güncelleme öncesi snapshot #%1 korundu; sistem sağlığı bağımsız olarak doğrulanana kadar otomatik silinmez.")
                                 .arg(m_verifiedSnapshotId);
    }
    emit changed();
    m_scanner->scan();
    emit sessionFinished();
}
