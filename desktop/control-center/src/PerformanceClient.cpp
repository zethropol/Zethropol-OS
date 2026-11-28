#include "PerformanceClient.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

constexpr auto kPerfService = "org.zethropol.Performance";
constexpr auto kPerfPath = "/org/zethropol/Performance";
constexpr auto kPerfInterface = "org.zethropol.Performance.v1";

QVariant perfDbusToVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return perfDbusToVariant(value.value<QDBusVariant>().variant());

    if (value.userType() == QMetaType::QVariantMap) {
        const QVariantMap source = value.toMap();
        QVariantMap out;
        for (auto it = source.cbegin(); it != source.cend(); ++it)
            out.insert(it.key(), perfDbusToVariant(it.value()));
        return out;
    }

    if (value.userType() == QMetaType::QVariantList) {
        const QVariantList source = value.toList();
        QVariantList out;
        out.reserve(source.size());
        for (const QVariant &item : source)
            out.append(perfDbusToVariant(item));
        return out;
    }

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto argument = value.value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::ArrayType) {
        if (argument.currentSignature() == QStringLiteral("as")) {
            QStringList strings;
            argument.beginArray();
            while (!argument.atEnd()) {
                QString s;
                argument >> s;
                strings.append(s);
            }
            argument.endArray();
            return strings;
        }
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd()) {
            QDBusVariant item;
            argument >> item;
            list.append(perfDbusToVariant(item.variant()));
        }
        argument.endArray();
        return list;
    }

    if (argument.currentType() != QDBusArgument::MapType)
        return {};

    QVariantMap map;
    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        QDBusVariant wrapped;
        argument >> key >> wrapped;
        map.insert(key, perfDbusToVariant(wrapped.variant()));
        argument.endMapEntry();
    }
    argument.endMap();
    return map;
}

QStringList toStringList(const QVariant &val)
{
    if (val.typeId() == QMetaType::QStringList)
        return val.toStringList();
    QStringList out;
    const QVariantList list = val.toList();
    for (const QVariant &item : list) {
        const QString s = item.toString().trimmed();
        if (!s.isEmpty())
            out.append(s);
    }
    return out;
}

QVariantMap defaultReadinessMap()
{
    return QVariantMap{
        {QStringLiteral("gpuName"), QStringLiteral("Bilinmiyor")},
        {QStringLiteral("matchedProfile"), QStringLiteral("unavailable")},
        {QStringLiteral("tier"), QStringLiteral("vulkan_unavailable")},
        {QStringLiteral("tierLabel"), QStringLiteral("Hazırlık bilgisi bekleniyor")},
        {QStringLiteral("driverName"), QStringLiteral("none")},
        {QStringLiteral("driverInfo"), QString()},
        {QStringLiteral("vulkanApiVersion"), QStringLiteral("N/A")},
        {QStringLiteral("vulkanAvailable"), false},
        {QStringLiteral("hardwareVulkan13"), false},
        {QStringLiteral("dxvkSupported"), false},
        {QStringLiteral("vkd3dSupported"), false},
        {QStringLiteral("dxvkAvailable"), false},
        {QStringLiteral("dxvkSource"), QStringLiteral("none")},
        {QStringLiteral("vkd3dAvailable"), false},
        {QStringLiteral("vkd3dSource"), QStringLiteral("none")},
        {QStringLiteral("steamAvailable"), false},
        {QStringLiteral("protonAvailable"), false},
        {QStringLiteral("protonSource"), QStringLiteral("none")},
        {QStringLiteral("gamescopeAvailable"), false},
        {QStringLiteral("mangohudAvailable"), false},
        {QStringLiteral("gamemodeAvailable"), false},
        {QStringLiteral("wineAvailable"), false},
        {QStringLiteral("limitations"), QStringList{}},
        {QStringLiteral("recommendations"), QStringList{}},
        {QStringLiteral("missingComponents"), QStringList{}},
        {QStringLiteral("kernelAnticheatBypassForbidden"), true},
        {QStringLiteral("anticheatNotice"),
         QStringLiteral("Uyarı: Çekirdek seviyesi anti-cheat atlatma veya oyun EULA/kullanım koşulları ihlali kesinlikle yasaktır.")}
    };
}

} // namespace

PerformanceClient::PerformanceClient(QObject *parent)
    : QObject(parent)
    , m_profiles(defaultProfiles())
    , m_readiness(defaultReadinessMap())
{
    auto bus = QDBusConnection::systemBus();
    if (bus.isConnected()) {
        m_signalConnected = bus.connect(
            QString::fromLatin1(kPerfService),
            QString::fromLatin1(kPerfPath),
            QString::fromLatin1(kPerfInterface),
            QStringLiteral("ProfileChanged"),
            this,
            SLOT(handleProfileChangedSignal(QVariantMap)));
    }

    m_refreshTimer.setInterval(5000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &PerformanceClient::requestProfileState);
    m_refreshTimer.start();

    QTimer::singleShot(0, this, &PerformanceClient::refresh);
}

bool PerformanceClient::isValidProfile(const QString &profile)
{
    const QString norm = profile.trimmed().toLower();
    return norm == QStringLiteral("general")
        || norm == QStringLiteral("gaming")
        || norm == QStringLiteral("developer")
        || norm == QStringLiteral("creator");
}

QVariantList PerformanceClient::defaultProfiles()
{
    return QVariantList{
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("general")},
            {QStringLiteral("title"), QStringLiteral("General")},
            {QStringLiteral("description"), QStringLiteral("Varsayılan dengeli profil; günlük kullanım için verimli CPU/GPU saatleri.")},
            {QStringLiteral("targetGovernor"), QStringLiteral("schedutil")},
            {QStringLiteral("targetGpuDpm"), QStringLiteral("auto")},
            {QStringLiteral("thermalGuardApplies"), false},
            {QStringLiteral("allowedNow"), true},
            {QStringLiteral("blockedReason"), QString()}
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("gaming")},
            {QStringLiteral("title"), QStringLiteral("Gaming")},
            {QStringLiteral("description"), QStringLiteral("Düşük gecikme ve kararlı kare süresi için yüksek CPU/GPU saat optimizasyonu (termal korumalı).")},
            {QStringLiteral("targetGovernor"), QStringLiteral("performance")},
            {QStringLiteral("targetGpuDpm"), QStringLiteral("high")},
            {QStringLiteral("thermalGuardApplies"), true},
            {QStringLiteral("allowedNow"), true},
            {QStringLiteral("blockedReason"), QString()}
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("developer")},
            {QStringLiteral("title"), QStringLiteral("Developer")},
            {QStringLiteral("description"), QStringLiteral("Derleme ve paralel iş yükleri için CPU öncelikli yüksek akış; GPU serin/otomatik modda.")},
            {QStringLiteral("targetGovernor"), QStringLiteral("performance")},
            {QStringLiteral("targetGpuDpm"), QStringLiteral("auto")},
            {QStringLiteral("thermalGuardApplies"), false},
            {QStringLiteral("allowedNow"), true},
            {QStringLiteral("blockedReason"), QString()}
        },
        QVariantMap{
            {QStringLiteral("id"), QStringLiteral("creator")},
            {QStringLiteral("title"), QStringLiteral("Creator")},
            {QStringLiteral("description"), QStringLiteral("Medya düzenleme ve uzun süreli render iş yükleri için kararlı termal süreklilik.")},
            {QStringLiteral("targetGovernor"), QStringLiteral("schedutil")},
            {QStringLiteral("targetGpuDpm"), QStringLiteral("auto")},
            {QStringLiteral("thermalGuardApplies"), false},
            {QStringLiteral("allowedNow"), true},
            {QStringLiteral("blockedReason"), QString()}
        }
    };
}

void PerformanceClient::setAutoRefreshEnabled(bool enabled)
{
    if (enabled)
        m_refreshTimer.start();
    else
        m_refreshTimer.stop();
}

void PerformanceClient::refresh()
{
    requestProfileState();
    requestGamingReadiness();
}

void PerformanceClient::requestProfileState()
{
    if (m_loadingProfile)
        return;

    const auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_available = false;
        m_lastError = QStringLiteral("Performans D-Bus bağlantısı kullanılamıyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        return;
    }

    m_loadingProfile = true;
    const auto msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPerfService),
        QString::fromLatin1(kPerfPath),
        QString::fromLatin1(kPerfInterface),
        QStringLiteral("GetProfileState"));
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 4000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &PerformanceClient::handleProfileStateReply);
}

void PerformanceClient::requestGamingReadiness()
{
    if (m_loadingReadiness)
        return;

    const auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return;

    m_loadingReadiness = true;
    const auto msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPerfService),
        QString::fromLatin1(kPerfPath),
        QString::fromLatin1(kPerfInterface),
        QStringLiteral("GetGamingReadiness"));
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 6000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &PerformanceClient::handleGamingReadinessReply);
}

void PerformanceClient::handleProfileStateReply()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;

    m_loadingProfile = false;
    QDBusPendingReply<QVariantMap> reply = *watcher;
    watcher->deleteLater();

    if (reply.isError()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-performance profil servisi erişilebilir değil.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        return;
    }

    const QVariantMap envelope = reply.value();
    if (!envelope.value(QStringLiteral("success")).toBool()) {
        m_available = false;
        m_lastError = envelope.value(QStringLiteral("message")).toString();
        if (m_lastError.isEmpty())
            m_lastError = QStringLiteral("Performans profili okunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        return;
    }

    const QVariantMap data = perfDbusToVariant(envelope.value(QStringLiteral("data"))).toMap();
    if (data.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("Performans servisi boş profil verisi döndürdü.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        return;
    }

    m_available = true;
    m_lastError.clear();
    applyProfileStateMap(data);
}

void PerformanceClient::handleGamingReadinessReply()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;

    m_loadingReadiness = false;
    QDBusPendingReply<QVariantMap> reply = *watcher;
    watcher->deleteLater();

    if (reply.isError())
        return;

    const QVariantMap envelope = reply.value();
    if (!envelope.value(QStringLiteral("success")).toBool())
        return;

    const QVariantMap data = perfDbusToVariant(envelope.value(QStringLiteral("data"))).toMap();
    if (!data.isEmpty())
        applyReadinessMap(data);
}

void PerformanceClient::setProfile(const QString &profile, bool userConfirmed)
{
    const QString norm = profile.trimmed().toLower();
    if (!isValidProfile(norm)) {
        m_lastError = QStringLiteral("Geçersiz performans profili: %1").arg(profile);
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("set-profile"), false, m_lastError);
        return;
    }

    if (!userConfirmed) {
        m_lastError = QStringLiteral("Performans profili değişikliği için açık kullanıcı onayı gereklidir; sessiz profil dayatması engellendi.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("set-profile"), false, m_lastError);
        return;
    }

    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir profil değişikliği devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("set-profile"), false, m_lastError);
        return;
    }

    const auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_available = false;
        m_lastError = QStringLiteral("Performans D-Bus bağlantısı kullanılamıyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("set-profile"), false, m_lastError);
        return;
    }

    m_busy = true;
    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profili için Polkit yetkilendirmesi ve termal sınır kontrolü yapılıyor...").arg(norm);
    emit stateChanged();

    auto msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kPerfService),
        QString::fromLatin1(kPerfPath),
        QString::fromLatin1(kPerfInterface),
        QStringLiteral("SetProfile"));
    msg << norm << true;

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg, 120000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, &PerformanceClient::handleSetProfileReply);
}

void PerformanceClient::handleSetProfileReply()
{
    auto *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;

    m_busy = false;
    QDBusPendingReply<QVariantMap> reply = *watcher;
    watcher->deleteLater();

    if (reply.isError()) {
        m_lastError = reply.error().message().isEmpty()
            ? QStringLiteral("Performans profili değiştirilemedi.")
            : reply.error().message();
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("set-profile"), false, m_lastError);
        return;
    }

    const QVariantMap envelope = reply.value();
    const bool ok = envelope.value(QStringLiteral("success")).toBool();
    const QString message = envelope.value(QStringLiteral("message")).toString();
    const QVariantMap data = perfDbusToVariant(envelope.value(QStringLiteral("data"))).toMap();

    if (!data.isEmpty()) {
        m_available = true;
        applyProfileStateMap(data);
    }

    if (ok) {
        m_lastError.clear();
        m_statusMessage = message.isEmpty()
            ? QStringLiteral("Performans profili güncellendi.")
            : message;
    } else {
        m_lastError = message.isEmpty()
            ? QStringLiteral("Performans profili değişikliği reddedildi.")
            : message;
        m_statusMessage = m_lastError;
    }

    emit stateChanged();
    emit operationFinished(QStringLiteral("set-profile"), ok, m_statusMessage);
}

void PerformanceClient::handleProfileChangedSignal(const QVariantMap &state)
{
    const QVariantMap data = perfDbusToVariant(state).toMap();
    if (data.isEmpty())
        return;
    m_available = true;
    applyProfileStateMap(data);
}

void PerformanceClient::applyProfileStateMap(const QVariantMap &data)
{
    const QString active = data.value(QStringLiteral("active_profile")).toString().trimmed().toLower();
    if (isValidProfile(active))
        m_activeProfile = active;

    const QString gov = data.value(QStringLiteral("cpu_governor")).toString().trimmed();
    if (!gov.isEmpty())
        m_cpuGovernor = gov;

    m_availableGovernors = toStringList(data.value(QStringLiteral("available_governors")));

    const QString dpm = data.value(QStringLiteral("gpu_dpm_level")).toString().trimmed();
    if (!dpm.isEmpty())
        m_gpuDpmLevel = dpm;

    m_thermalGuardActive = data.value(QStringLiteral("thermal_guard_active")).toBool();
    const QString thermalReason = data.value(QStringLiteral("thermal_guard_reason")).toString().trimmed();
    if (!thermalReason.isEmpty())
        m_thermalGuardReason = thermalReason;

    if (data.contains(QStringLiteral("thermal_cpu_limit_c")))
        m_thermalCpuLimitC = data.value(QStringLiteral("thermal_cpu_limit_c")).toDouble();
    if (data.contains(QStringLiteral("thermal_gpu_limit_c")))
        m_thermalGpuLimitC = data.value(QStringLiteral("thermal_gpu_limit_c")).toDouble();

    m_gamingSessionActive = data.value(QStringLiteral("gaming_session_active")).toBool();
    m_detectedGamingProcesses = toStringList(data.value(QStringLiteral("detected_gaming_processes")));
    const QString rec = data.value(QStringLiteral("recommended_profile")).toString().trimmed().toLower();
    if (isValidProfile(rec))
        m_recommendedProfile = rec;
    const QString advisory = data.value(QStringLiteral("gaming_session_advisory")).toString().trimmed();
    if (!advisory.isEmpty())
        m_gamingSessionAdvisory = advisory;

    // Silent profile forcing is unconditionally disabled
    m_silentAutoSwitchBlocked = true;

    const QString profilesJson = data.value(QStringLiteral("profiles_json")).toString();
    if (!profilesJson.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(profilesJson.toUtf8());
        if (doc.isArray()) {
            QVariantList parsed;
            const QJsonArray arr = doc.array();
            for (const QJsonValue &val : arr) {
                const QJsonObject obj = val.toObject();
                parsed.append(QVariantMap{
                    {QStringLiteral("id"), obj.value(QStringLiteral("id")).toString()},
                    {QStringLiteral("title"), obj.value(QStringLiteral("title")).toString()},
                    {QStringLiteral("description"), obj.value(QStringLiteral("description")).toString()},
                    {QStringLiteral("targetGovernor"), obj.value(QStringLiteral("target_governor")).toString()},
                    {QStringLiteral("targetGpuDpm"), obj.value(QStringLiteral("target_gpu_dpm")).toString()},
                    {QStringLiteral("thermalGuardApplies"), obj.value(QStringLiteral("thermal_guard_applies")).toBool()},
                    {QStringLiteral("allowedNow"), obj.value(QStringLiteral("allowed_now")).toBool(true)},
                    {QStringLiteral("blockedReason"), obj.value(QStringLiteral("blocked_reason")).toString()}
                });
            }
            if (!parsed.isEmpty())
                m_profiles = parsed;
        }
    }

    if (m_lastError.isEmpty()) {
        m_statusMessage = QStringLiteral("Aktif profil: %1 · CPU governor: %2 · GPU DPM: %3")
                              .arg(m_activeProfile.toUpper(), m_cpuGovernor, m_gpuDpmLevel);
    }

    emit stateChanged();
}

void PerformanceClient::applyReadinessMap(const QVariantMap &data)
{
    QVariantMap next = defaultReadinessMap();
    next.insert(QStringLiteral("gpuName"), data.value(QStringLiteral("gpu_name"), next.value(QStringLiteral("gpuName"))).toString());
    next.insert(QStringLiteral("matchedProfile"), data.value(QStringLiteral("matched_profile"), next.value(QStringLiteral("matchedProfile"))).toString());
    next.insert(QStringLiteral("tier"), data.value(QStringLiteral("tier"), next.value(QStringLiteral("tier"))).toString());
    next.insert(QStringLiteral("tierLabel"), data.value(QStringLiteral("tier_label"), next.value(QStringLiteral("tierLabel"))).toString());
    next.insert(QStringLiteral("driverName"), data.value(QStringLiteral("driver_name"), next.value(QStringLiteral("driverName"))).toString());
    next.insert(QStringLiteral("driverInfo"), data.value(QStringLiteral("driver_info")).toString());
    next.insert(QStringLiteral("vulkanApiVersion"), data.value(QStringLiteral("vulkan_api_version"), QStringLiteral("N/A")).toString());
    next.insert(QStringLiteral("vulkanAvailable"), data.value(QStringLiteral("vulkan_available")).toBool());
    next.insert(QStringLiteral("hardwareVulkan13"), data.value(QStringLiteral("hardware_vulkan_13")).toBool());
    next.insert(QStringLiteral("dxvkSupported"), data.value(QStringLiteral("dxvk_supported")).toBool());
    next.insert(QStringLiteral("vkd3dSupported"), data.value(QStringLiteral("vkd3d_supported")).toBool());
    next.insert(QStringLiteral("dxvkAvailable"), data.value(QStringLiteral("dxvk_available")).toBool());
    next.insert(QStringLiteral("dxvkSource"), data.value(QStringLiteral("dxvk_source"), QStringLiteral("none")).toString());
    next.insert(QStringLiteral("vkd3dAvailable"), data.value(QStringLiteral("vkd3d_available")).toBool());
    next.insert(QStringLiteral("vkd3dSource"), data.value(QStringLiteral("vkd3d_source"), QStringLiteral("none")).toString());
    next.insert(QStringLiteral("steamAvailable"), data.value(QStringLiteral("steam_available")).toBool());
    next.insert(QStringLiteral("protonAvailable"), data.value(QStringLiteral("proton_available")).toBool());
    next.insert(QStringLiteral("protonSource"), data.value(QStringLiteral("proton_source"), QStringLiteral("none")).toString());
    next.insert(QStringLiteral("gamescopeAvailable"), data.value(QStringLiteral("gamescope_available")).toBool());
    next.insert(QStringLiteral("mangohudAvailable"), data.value(QStringLiteral("mangohud_available")).toBool());
    next.insert(QStringLiteral("gamemodeAvailable"), data.value(QStringLiteral("gamemode_available")).toBool());
    next.insert(QStringLiteral("wineAvailable"), data.value(QStringLiteral("wine_available")).toBool());
    next.insert(QStringLiteral("limitations"), toStringList(data.value(QStringLiteral("limitations"))));
    next.insert(QStringLiteral("recommendations"), toStringList(data.value(QStringLiteral("recommendations"))));
    next.insert(QStringLiteral("missingComponents"), toStringList(data.value(QStringLiteral("missing_components"))));
    next.insert(QStringLiteral("kernelAnticheatBypassForbidden"), true);
    const QString notice = data.value(QStringLiteral("anticheat_eula_notice")).toString().trimmed();
    if (!notice.isEmpty())
        next.insert(QStringLiteral("anticheatNotice"), notice);

    m_readiness = next;
    emit stateChanged();
}
