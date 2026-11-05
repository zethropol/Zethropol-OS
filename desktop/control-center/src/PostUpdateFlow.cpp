#include "PostUpdateFlow.h"
#include "core/update_state_decoder.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QRegularExpression>

PostUpdateFlow::PostUpdateFlow(QObject *parent, const QString &file) : QObject(parent) {
    if (file.isEmpty()) m_settings = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
        QStringLiteral("Zethropol"), QStringLiteral("ControlCenterUpdates"));
    else m_settings = std::make_unique<QSettings>(file, QSettings::IniFormat);
    m_poll.setInterval(5000);
    connect(&m_poll, &QTimer::timeout, this, &PostUpdateFlow::refresh);
}
QString PostUpdateFlow::key(const QString &name) const { return "operations/" + m_id + "/" + name; }
bool PostUpdateFlow::persist(const QString &name, bool value) {
    m_settings->setValue(key(name), value);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        m_settingsError = true;
        m_poll.stop();
        m_phase = "error";
        fail(QStringLiteral("Yerel pencere onayı kaydedilemedi. Otomatik yeniden başlatma yapılmadı."));
        return false;
    }
    return true;
}
bool PostUpdateFlow::terminalCommit(const QVariantMap &s) const {
    return s.value("post_update_protocol").toString() == "1"
        && s.value("terminal").toString() == "true" && s.value("worker_busy").toString() == "false"
        && QStringList{"ok", "ok_with_warnings", "failed"}.contains(s.value("status").toString())
        && s.value("package_commit").toString() == "committed";
}
bool PostUpdateFlow::canReviewReboot() const {
    return !m_settingsError && !m_updateBusy && !m_id.isEmpty() && terminalCommit(m_state)
        && m_state.value("reboot_requirement").toString() == "required" && !m_rebooting;
}
void PostUpdateFlow::clearPresentation() {
    m_state.clear(); m_id.clear(); m_title.clear(); m_details.clear();
    m_phase = "idle";
}
void PostUpdateFlow::setUpdateBusy(bool value) {
    if (m_updateBusy == value) return;
    m_updateBusy = value;
    if (value) {
        ++m_generation;
        m_loading = false;
        m_rebooting = false;
        clearPresentation();
        m_status = "Güncelleme devam ediyor; işlem sonrası pencereler bekletiliyor.";
        emit changed();
    } else refresh();
}
void PostUpdateFlow::start() { m_poll.start(); refresh(); }
void PostUpdateFlow::refresh() {
    if (busy()) return;
    m_loading = true;
    ++m_generation;
    fetchState(QString(), false);
}
void PostUpdateFlow::fetchState(const QString &id, bool forReboot) {
    auto call = QDBusMessage::createMethodCall("org.zethropol.Update", "/org/zethropol/Update",
        "org.zethropol.Update.v1", "GetPostUpdateState");
    call << id;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 15000), this);
    const auto generation = m_generation;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, forReboot, generation](QDBusPendingCallWatcher *w) {
        const auto reply = w->reply(); w->deleteLater();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            deliverReadError(reply.errorName(), reply.errorMessage(), forReboot, generation); return;
        }
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1 || reply.signature() != QStringLiteral("a{ss}")) {
            deliverReadError("invalid_post_update_reply", "Expected a{ss}; received " + reply.signature(), forReboot, generation); return;
        }
        const auto map = ZethropolUpdateWire::decodeMap(reply.arguments().first());
        deliverState(map, forReboot, generation);
    });
}
void PostUpdateFlow::deliverReadError(const QString &name, const QString &message, bool forReboot, quint64 generation) {
    if (generation && generation != m_generation) return;
    m_loading = false;
    clearPresentation();
    m_status = QStringLiteral("Güncelleme sonrası durum okunamadı: %1 — %2").arg(name, message.left(8192));
    if (name == "org.freedesktop.DBus.Error.UnknownMethod") {
        m_poll.stop();
        m_state.clear();
        m_phase = "idle";
        m_status = QStringLiteral("Bu akış için üretim Update servisine GetPostUpdateState protokolü uygulanmalı. Yeniden başlatma gereksinimi tahmin edilmedi.");
    }
    if (name == "org.freedesktop.DBus.Error.AccessDenied") {
        m_poll.stop();
        m_state.clear();
        m_phase = "idle";
        m_status += QStringLiteral("\nOturum yetkisi ve GetPostUpdateState D-Bus izni kontrol edilmeli. Otomatik tekrar yapılmadı.");
    }
    if (forReboot) { m_rebooting = false; fail(m_status); }
    emit changed();
}
void PostUpdateFlow::deliverState(const QVariantMap &state, bool forReboot, quint64 generation) {
    if (generation && generation != m_generation) return;
    m_loading = false;
    if (forReboot && !m_rebooting) return; // A service reply cannot create consent.
    if (forReboot && (m_phase != "reboot" || m_settingsError
        || !m_settings->value(key("cleanupAcknowledged"), false).toBool())) {
        m_rebooting = false;
        fail("Yeniden başlatma onayı artık geçerli değil; Power çağrısı yapılmadı.");
        return;
    }
    const QString requestedId = m_id;
    if (state.value("post_update_protocol").toString() != "1") {
        clearPresentation();
        m_status = QStringLiteral("Güncelleme sonrası protokol eksik/geçersiz; sonuç veya yeniden başlatma gereksinimi varsayılmadı.");
        if (forReboot) { m_rebooting = false; fail(m_status); }
        emit changed(); return;
    }
    if (state.value("status").toString() == "not-found") {
        if (forReboot) { m_rebooting = false; fail("İşlem artık bulunamıyor; yeniden başlatma çağrılmadı."); }
        else { clearPresentation(); m_status = "Devam edilecek tamamlanmış işlem yok."; }
        emit changed(); return;
    }
    const QString id = state.value("operation_id").toString();
    if (!QRegularExpression("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$").match(id).hasMatch()) {
        deliverReadError("invalid_operation_id", "Geçersiz işlem kimliği; sonuç doğrulanamadı.", forReboot);
        return;
    }
    if (forReboot && (m_updateBusy || id != requestedId || !terminalCommit(state) || state.value("reboot_requirement").toString() != "required")) {
        m_rebooting = false;
        m_state = state;
        m_phase = "idle";
        fail("İşlem durumu değişti veya yeniden başlatma gereksinimi doğrulanmadı. Yeniden başlatma çağrılmadı.");
        return;
    }
    m_id = id; m_state = state;
    if (forReboot) { callPowerReboot(); return; }
    present();
}
void PostUpdateFlow::present() {
    if (m_settingsError) { m_phase = "error"; emit changed(); return; }
    const QString commit = m_state.value("package_commit").toString();
    if (!m_updateBusy && m_state.value("terminal") == "true"
        && m_state.value("worker_busy") == "false"
        && QStringList{"ok", "ok_with_warnings", "failed"}.contains(m_state.value("status").toString())
        && (commit == "unknown" || commit == "failed-may-be-partial")) {
        m_phase = "uncertain";
        m_title = "Güncelleme sonucu doğrulanmadı";
        m_details = QStringLiteral("İşlem: %1\nServis sonucu: %2\nPaket commit kanıtı: %3\n\n%4\n\nHook ayrıntıları: %5\nBoot ayrıntıları: %6\n\nKısmi değişiklikler olabilir. Güncelleme başarılı sayılmadı. Otomatik devam, temizlik veya yeniden başlatma yapılmaz.")
            .arg(m_id, m_state.value("status").toString(), commit,
                m_state.value("message", "Servis mesajı bildirilmedi.").toString(),
                m_state.value("hook_details").toString(), m_state.value("boot_details").toString());
        m_status = m_title + " — " + m_id;
        emit changed(); return;
    }
    if (m_updateBusy || !terminalCommit(m_state)) {
        m_title.clear(); m_details.clear();
        m_phase = "idle";
        m_status = "Paket işlemi devam ediyor veya commit doğrulanmadı; sonuç/yeniden başlatma penceresi açılmadı.";
        emit changed(); return;
    }
    const QString cleanup = m_state.value("cleanup_status").toString();
    bool countsValid = true;
    for (const char *name : {"cleanup_removed_files", "cleanup_removed_bytes", "cleanup_reclaimed_bytes"}) {
        const QString number = m_state.value(name).toString();
        bool valid = false; number.toULongLong(&valid);
        countsValid = countsValid && valid && QRegularExpression("^(0|[1-9][0-9]*)$").match(number).hasMatch();
    }
    const bool completed = cleanup == "completed" && countsValid
        && QStringList{"ok", "ok_with_warnings"}.contains(m_state.value("status").toString());
    m_title = completed ? QStringLiteral("Önbellek temizliği tamamlandı") : QStringLiteral("Önbellek temizliği sonucu");
    m_details = QStringLiteral("İşlem: %1\nPaket işlemi: %2\nTemizlik durumu: %3\nTemizlenen dosya: %4\nKaldırılan bayt: %5\nGözlenen boş alan artışı: %6 bayt\n\n%7\n\nBoş alan artışı gözlemdir; CoW fiziksel alanın geri kazanılmasını geciktirebilir.")
        .arg(m_id, m_state.value("status").toString(), cleanup.isEmpty() ? QStringLiteral("bildirilmedi") : cleanup,
            m_state.value("cleanup_removed_files", "bildirilmedi").toString(),
            m_state.value("cleanup_removed_bytes", "bildirilmedi").toString(),
            m_state.value("cleanup_reclaimed_bytes", "bildirilmedi").toString(),
            m_state.value("cleanup_details").toString());
    if (!completed) m_details += QStringLiteral("\nBaşarılı temizlik varsayılmadı; gerçek durum yukarıda gösteriliyor.");
    m_status = QStringLiteral("Son işlem: %1. Yeniden başlatma: %2").arg(m_id, m_state.value("reboot_requirement", "unknown").toString());
    if (!m_settings->value(key("cleanupAcknowledged"), false).toBool()) m_phase = "cleanup";
    else if (canReviewReboot()) m_phase = m_settings->value(key("rebootDeferred"), false).toBool() ? "deferred" : "reboot";
    else m_phase = "idle";
    emit changed();
}
void PostUpdateFlow::acknowledgeCleanup() {
    if (m_phase != "cleanup") return;
    if (!persist("cleanupAcknowledged", true)) return;
    present();
}
void PostUpdateFlow::deferReboot() {
    if (m_phase != "reboot") return;
    if (!persist("rebootDeferred", true)) return;
    m_phase = "deferred";
    emit changed();
}
void PostUpdateFlow::reviewReboot() {
    if (canReviewReboot() && m_settings->value(key("cleanupAcknowledged"), false).toBool()) {
        if (!persist("rebootDeferred", false)) return;
        m_phase = "reboot"; emit changed();
    }
}
void PostUpdateFlow::rebootNow() {
    if (m_phase != "reboot" || busy() || !canReviewReboot()
        || !m_settings->value(key("cleanupAcknowledged"), false).toBool()) return;
    m_rebooting = true;
    m_loading = true;
    ++m_generation;
    emit changed();
    fetchState(m_id, true); // Fresh authoritative check before any power action.
}
void PostUpdateFlow::callPowerReboot() {
    // Same existing procedure as PowerBridge: org.zethropol.Power authorizes
    // with PolicyKit, then uses logind.Reboot. No shell/sudo or auth bypass.
    auto call = QDBusMessage::createMethodCall("org.zethropol.Power", "/org/zethropol/Power", "org.zethropol.Power.v1", "Reboot");
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 90000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const auto reply = w->reply(); w->deleteLater();
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) {
            deliverRebootReply(false, reply.errorName(), reply.errorMessage()); return;
        }
        const auto map = ZethropolUpdateWire::decodeMap(reply.arguments().first());
        if (map.value("success").metaType().id() != QMetaType::Bool) {
            deliverRebootReply(false, "invalid_power_reply", "Yeniden başlatma sonucu doğrulanamadı. Otomatik tekrar yapılmadı."); return;
        }
        deliverRebootReply(map.value("success").toBool(), map.value("code").toString(), map.value("message").toString());
    });
}
void PostUpdateFlow::deliverRebootReply(bool success, const QString &code, const QString &message) {
    m_rebooting = false;
    if (!success) { fail(QStringLiteral("Yeniden başlatma hatası: %1\n%2\nOtomatik tekrar yapılmadı.").arg(code, message.left(8192))); return; }
    if (!persist("rebootDeferred", true)) return;
    m_phase = "deferred";
    m_status = "Yeniden başlatma isteği yetkili Power servisine iletildi; sistemin kapandığı henüz doğrulanmadı.";
    emit changed();
}
void PostUpdateFlow::fail(const QString &message) {
    m_status = message;
    emit operationError(QStringLiteral("Güncelleme sonrası işlem"), message);
    emit changed();
}
