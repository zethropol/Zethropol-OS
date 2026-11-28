#include "CompatibilityManager.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

const QRegularExpression &profileNameRegex()
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,47}$"));
    return re;
}

QString extractErrorText(const QByteArray &stderrBytes, const QByteArray &stdoutBytes, int exitCode)
{
    const QString err = QString::fromUtf8(stderrBytes).trimmed();
    if (!err.isEmpty())
        return err.left(400);
    const QString out = QString::fromUtf8(stdoutBytes).trimmed();
    if (!out.isEmpty())
        return out.left(400);
    return QStringLiteral("Komut %1 çıkış koduyla başarısız oldu.").arg(exitCode);
}

} // namespace

CompatibilityManager::CompatibilityManager(QObject *parent)
    : QObject(parent)
{
    m_initialRefreshTimer.setSingleShot(true);
    m_initialRefreshTimer.setInterval(0);
    connect(&m_initialRefreshTimer, &QTimer::timeout, this, &CompatibilityManager::refresh);

    m_opTimeoutTimer.setSingleShot(true);
    connect(&m_opTimeoutTimer, &QTimer::timeout, this, &CompatibilityManager::handleActiveProcessTimeout);

    m_initialRefreshTimer.start();
}

CompatibilityManager::~CompatibilityManager()
{
    m_initialRefreshTimer.stop();
    m_opTimeoutTimer.stop();

    if (m_activeProcess) {
        m_activeProcess->disconnect(this);
        if (m_activeProcess->state() != QProcess::NotRunning) {
            m_activeProcess->terminate();
            if (!m_activeProcess->waitForFinished(1000)) {
                m_activeProcess->kill();
                m_activeProcess->waitForFinished(500);
            }
        }
        delete m_activeProcess;
    }

    for (const QPointer<QProcess> &proc : std::as_const(m_launchedProcesses)) {
        if (!proc)
            continue;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->terminate();
            if (!proc->waitForFinished(500)) {
                proc->kill();
                proc->waitForFinished(500);
            }
        }
        delete proc;
    }
    m_launchedProcesses.clear();
}

bool CompatibilityManager::available() const { return m_available; }
bool CompatibilityManager::busy() const { return m_busy; }
bool CompatibilityManager::launching() const { return m_launching; }
QString CompatibilityManager::statusMessage() const { return m_statusMessage; }
QString CompatibilityManager::lastError() const { return m_lastError; }

QString CompatibilityManager::wineVersion() const { return m_wineVersion; }
QString CompatibilityManager::wineMode() const { return m_wineMode; }
bool CompatibilityManager::vulkanAvailable() const { return m_vulkanAvailable; }
bool CompatibilityManager::hardwareVulkan13() const { return m_hardwareVulkan13; }
QString CompatibilityManager::vulkanGpuName() const { return m_vulkanGpuName; }
QString CompatibilityManager::vulkanApiVersion() const { return m_vulkanApiVersion; }
QString CompatibilityManager::vulkanReason() const { return m_vulkanReason; }
bool CompatibilityManager::dxvkAvailable() const { return m_dxvkAvailable; }
QString CompatibilityManager::dxvkSource() const { return m_dxvkSource; }
bool CompatibilityManager::vkd3dAvailable() const { return m_vkd3dAvailable; }
bool CompatibilityManager::bwrapAvailable() const { return m_bwrapAvailable; }
QString CompatibilityManager::bwrapPath() const { return m_bwrapPath; }
QString CompatibilityManager::gpuTierLabel() const { return m_gpuTierLabel; }
QStringList CompatibilityManager::gpuLimitations() const { return m_gpuLimitations; }
QString CompatibilityManager::anticheatNotice() const { return m_anticheatNotice; }

QVariantList CompatibilityManager::profiles() const { return m_profiles; }
int CompatibilityManager::profileCount() const { return m_profiles.size(); }
QVariantMap CompatibilityManager::lastDryRunPlan() const { return m_lastDryRunPlan; }

void CompatibilityManager::setPrefixCtlPath(const QString &path)
{
    m_initialRefreshTimer.stop();
    m_customPrefixCtl = path.trimmed();
}

void CompatibilityManager::setWineRunPath(const QString &path)
{
    m_initialRefreshTimer.stop();
    m_customWineRun = path.trimmed();
}

void CompatibilityManager::setProcessEnvironment(const QProcessEnvironment &env)
{
    m_initialRefreshTimer.stop();
    m_hasCustomEnv = true;
    m_customEnv = env;
}

void CompatibilityManager::setCommandTimeoutMs(int timeoutMs)
{
    if (timeoutMs >= 50)
        m_commandTimeoutMs = timeoutMs;
}

void CompatibilityManager::setInitTimeoutMs(int timeoutMs)
{
    if (timeoutMs >= 50)
        m_initTimeoutMs = timeoutMs;
}

bool CompatibilityManager::isValidProfileName(const QString &name)
{
    return profileNameRegex().match(name).hasMatch();
}

bool CompatibilityManager::isValidTemplate(const QString &templateName)
{
    return templateName == QStringLiteral("default-win64")
        || templateName == QStringLiteral("classic-win32");
}

bool CompatibilityManager::isValidRenderer(const QString &renderer)
{
    return renderer == QStringLiteral("auto")
        || renderer == QStringLiteral("dxvk")
        || renderer == QStringLiteral("wined3d");
}

bool CompatibilityManager::isSupportedTargetPath(const QString &filePath)
{
    if (filePath.isEmpty())
        return false;
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile())
        return false;
    const QString suffix = fi.suffix().toLower();
    return suffix == QStringLiteral("exe")
        || suffix == QStringLiteral("msi")
        || suffix == QStringLiteral("lnk");
}

QString CompatibilityManager::localPathFromUrl(const QUrl &url) const
{
    if (url.isLocalFile())
        return url.toLocalFile();
    return url.toString();
}

QString CompatibilityManager::resolvePrefixCtl() const
{
    if (!m_customPrefixCtl.isEmpty()) {
        const QFileInfo fi(m_customPrefixCtl);
        return (fi.exists() && fi.isExecutable()) ? fi.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(QStringLiteral("zethropol-prefix-ctl"));
}

QString CompatibilityManager::resolveWineRun() const
{
    if (!m_customWineRun.isEmpty()) {
        const QFileInfo fi(m_customWineRun);
        return (fi.exists() && fi.isExecutable()) ? fi.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(QStringLiteral("zethropol-wine-run"));
}

QString CompatibilityManager::normalizeTargetPath(const QString &targetPathOrUrl) const
{
    const QString trimmed = targetPathOrUrl.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (trimmed.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        const QUrl url(trimmed);
        if (url.isLocalFile())
            return url.toLocalFile();
    }
    return trimmed;
}

QProcessEnvironment CompatibilityManager::effectiveEnvironment() const
{
    if (m_hasCustomEnv)
        return m_customEnv;
    return QProcessEnvironment::systemEnvironment();
}

void CompatibilityManager::refresh()
{
    m_initialRefreshTimer.stop();
    if (m_busy) {
        return;
    }

    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_busy = false;
        m_lastError.clear();
        m_wineVersion = QStringLiteral("Bilinmiyor");
        m_wineMode = QStringLiteral("unknown");
        m_vulkanAvailable = false;
        m_hardwareVulkan13 = false;
        m_vulkanGpuName = QStringLiteral("Bilinmiyor");
        m_vulkanApiVersion = QStringLiteral("N/A");
        m_vulkanReason = QStringLiteral("zethropol-compatibility paketi kurulu değil.");
        m_dxvkAvailable = false;
        m_dxvkSource = QStringLiteral("none");
        m_vkd3dAvailable = false;
        m_bwrapAvailable = false;
        m_bwrapPath = QStringLiteral("not-installed");
        m_profiles.clear();
        m_statusMessage = QStringLiteral("zethropol-compatibility paketi kurulu değil.");
        emit stateChanged();
        emit refreshFinished(false);
        return;
    }

    m_lastError.clear();
    m_pendingAuditProfiles.clear();
    m_statusMessage = QStringLiteral("Wine, Vulkan ve profil durumu yenileniyor...");
    startCommand(ActiveOp::RefreshProbe, ctl, {QStringLiteral("probe"), QStringLiteral("--json")}, m_commandTimeoutMs);
}

void CompatibilityManager::initProfile(const QString &profile,
                                       const QString &templateName,
                                       const QString &renderer,
                                       bool sandboxEnabled,
                                       bool force)
{
    m_initialRefreshTimer.stop();
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("init"), false, m_lastError);
        return;
    }
    if (!isValidProfileName(profile)) {
        m_lastError = QStringLiteral("Geçersiz profil adı: yalnızca küçük harf, rakam, '-' ve '_' kullanılabilir.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("init"), false, m_lastError);
        return;
    }
    if (!isValidTemplate(templateName)) {
        m_lastError = QStringLiteral("Desteklenmeyen profil şablonu: %1").arg(templateName);
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("init"), false, m_lastError);
        return;
    }
    if (!isValidRenderer(renderer)) {
        m_lastError = QStringLiteral("Desteklenmeyen grafik motoru: %1").arg(renderer);
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("init"), false, m_lastError);
        return;
    }

    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-prefix-ctl bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("init"), false, m_lastError);
        return;
    }

    QStringList args = {
        QStringLiteral("init"),
        profile,
        QStringLiteral("--template"),
        templateName,
        QStringLiteral("--renderer"),
        renderer,
        QStringLiteral("--json")
    };
    if (!sandboxEnabled)
        args.append(QStringLiteral("--no-sandbox"));
    if (force)
        args.append(QStringLiteral("--force"));

    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profili oluşturuluyor (%2)...").arg(profile, templateName);
    startCommand(ActiveOp::InitProfile, ctl, args, m_initTimeoutMs, profile);
}

void CompatibilityManager::auditProfile(const QString &profile, bool fix)
{
    m_initialRefreshTimer.stop();
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("audit"), false, m_lastError);
        return;
    }
    if (!isValidProfileName(profile)) {
        m_lastError = QStringLiteral("Geçersiz profil adı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("audit"), false, m_lastError);
        return;
    }

    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-prefix-ctl bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("audit"), false, m_lastError);
        return;
    }

    QStringList args = {QStringLiteral("audit"), profile, QStringLiteral("--json")};
    if (fix)
        args.append(QStringLiteral("--fix"));

    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profili denetleniyor...").arg(profile);
    startCommand(ActiveOp::AuditProfile, ctl, args, m_commandTimeoutMs, profile);
}

void CompatibilityManager::syncShortcuts(const QString &profile)
{
    m_initialRefreshTimer.stop();
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("sync-shortcuts"), false, m_lastError);
        return;
    }
    if (!isValidProfileName(profile)) {
        m_lastError = QStringLiteral("Geçersiz profil adı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("sync-shortcuts"), false, m_lastError);
        return;
    }

    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-prefix-ctl bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("sync-shortcuts"), false, m_lastError);
        return;
    }

    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profilindeki .lnk kısayolları eşitleniyor...").arg(profile);
    startCommand(ActiveOp::SyncShortcuts, ctl,
                 {QStringLiteral("sync-shortcuts"), profile, QStringLiteral("--json")},
                 m_commandTimeoutMs, profile);
}

void CompatibilityManager::installGamingLayers(const QString &profile,
                                               bool includeVkd3d,
                                               bool noticeAccepted)
{
    m_initialRefreshTimer.stop();
    if (!noticeAccepted) {
        m_lastError = QStringLiteral("Anti-cheat / EULA uyarısı açıkça onaylanmadan DXVK/VKD3D kurulamaz.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("install-dxvk"), false, m_lastError);
        return;
    }
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("install-dxvk"), false, m_lastError);
        return;
    }
    if (!isValidProfileName(profile)) {
        m_lastError = QStringLiteral("Geçersiz profil adı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("install-dxvk"), false, m_lastError);
        return;
    }
    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-prefix-ctl bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("install-dxvk"), false, m_lastError);
        return;
    }

    QStringList args = {
        QStringLiteral("install-dxvk"),
        profile,
        QStringLiteral("--accept-notice"),
        QStringLiteral("--json")
    };
    if (includeVkd3d)
        args.append(QStringLiteral("--with-vkd3d"));

    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profiline izole DXVK%2 kuruluyor...")
                          .arg(profile, includeVkd3d ? QStringLiteral("/VKD3D") : QString());
    startCommand(ActiveOp::InstallGamingLayers, ctl, args, m_commandTimeoutMs, profile);
}

void CompatibilityManager::removeGamingLayers(const QString &profile)
{
    m_initialRefreshTimer.stop();
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove-dxvk"), false, m_lastError);
        return;
    }
    if (!isValidProfileName(profile)) {
        m_lastError = QStringLiteral("Geçersiz profil adı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove-dxvk"), false, m_lastError);
        return;
    }
    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-prefix-ctl bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove-dxvk"), false, m_lastError);
        return;
    }

    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profilinden DXVK/VKD3D kaldırılıyor...").arg(profile);
    startCommand(ActiveOp::RemoveGamingLayers, ctl,
                 {QStringLiteral("remove-dxvk"), profile, QStringLiteral("--json")},
                 m_commandTimeoutMs, profile);
}

void CompatibilityManager::removeProfile(const QString &profile, bool userConfirmed)
{
    m_initialRefreshTimer.stop();
    if (!userConfirmed) {
        m_lastError = QStringLiteral("Profil silme işlemi için açık kullanıcı onayı gereklidir.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove"), false, m_lastError);
        return;
    }
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove"), false, m_lastError);
        return;
    }
    if (!isValidProfileName(profile)) {
        m_lastError = QStringLiteral("Geçersiz profil adı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove"), false, m_lastError);
        return;
    }

    const QString ctl = resolvePrefixCtl();
    if (ctl.isEmpty()) {
        m_available = false;
        m_lastError = QStringLiteral("zethropol-prefix-ctl bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit operationFinished(QStringLiteral("remove"), false, m_lastError);
        return;
    }

    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' profili siliniyor...").arg(profile);
    startCommand(ActiveOp::RemoveProfile, ctl,
                 {QStringLiteral("remove"), profile, QStringLiteral("--yes"), QStringLiteral("--json")},
                 m_commandTimeoutMs, profile);
}

void CompatibilityManager::previewLaunch(const QString &targetPathOrUrl,
                                         const QString &profile,
                                         const QString &renderer,
                                         bool sandboxEnabled)
{
    m_initialRefreshTimer.stop();
    if (m_busy) {
        m_lastError = QStringLiteral("Başka bir uyumluluk işlemi devam ediyor.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit dryRunCompleted(false, {});
        return;
    }
    const QString target = normalizeTargetPath(targetPathOrUrl);
    if (!isSupportedTargetPath(target)) {
        m_lastError = QStringLiteral("Geçerli bir .exe, .msi veya .lnk dosyası seçin.");
        m_statusMessage = m_lastError;
        m_lastDryRunPlan.clear();
        emit stateChanged();
        emit dryRunCompleted(false, {});
        return;
    }
    if (!isValidProfileName(profile) || !isValidRenderer(renderer)) {
        m_lastError = QStringLiteral("Geçersiz profil adı veya grafik motoru.");
        m_statusMessage = m_lastError;
        m_lastDryRunPlan.clear();
        emit stateChanged();
        emit dryRunCompleted(false, {});
        return;
    }

    const QString runner = resolveWineRun();
    if (runner.isEmpty()) {
        m_lastError = QStringLiteral("zethropol-wine-run bulunamadı.");
        m_statusMessage = m_lastError;
        m_lastDryRunPlan.clear();
        emit stateChanged();
        emit dryRunCompleted(false, {});
        return;
    }

    QStringList args = {
        QStringLiteral("--profile"), profile,
        QStringLiteral("--renderer"), renderer,
        QStringLiteral("--dry-run"),
        QStringLiteral("--json")
    };
    if (!sandboxEnabled)
        args.append(QStringLiteral("--no-sandbox"));
    args.append(QStringLiteral("--"));
    args.append(target);

    m_lastError.clear();
    m_statusMessage = QStringLiteral("Çalıştırma planı önizleniyor...");
    startCommand(ActiveOp::DryRun, runner, args, m_commandTimeoutMs, profile);
}

void CompatibilityManager::launchTarget(const QString &targetPathOrUrl,
                                        const QString &profile,
                                        const QString &renderer,
                                        bool sandboxEnabled)
{
    m_initialRefreshTimer.stop();
    const QString target = normalizeTargetPath(targetPathOrUrl);
    if (!isSupportedTargetPath(target)) {
        m_lastError = QStringLiteral("Geçerli bir .exe, .msi veya .lnk dosyası seçin.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit launchFinished(target, -1, m_lastError);
        return;
    }
    if (!isValidProfileName(profile) || !isValidRenderer(renderer)) {
        m_lastError = QStringLiteral("Geçersiz profil adı veya grafik motoru.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit launchFinished(target, -1, m_lastError);
        return;
    }

    const QString runner = resolveWineRun();
    if (runner.isEmpty()) {
        m_lastError = QStringLiteral("zethropol-wine-run bulunamadı.");
        m_statusMessage = m_lastError;
        emit stateChanged();
        emit launchFinished(target, -1, m_lastError);
        return;
    }

    QStringList args = {
        QStringLiteral("--profile"), profile,
        QStringLiteral("--renderer"), renderer
    };
    if (!sandboxEnabled)
        args.append(QStringLiteral("--no-sandbox"));
    args.append(QStringLiteral("--"));
    args.append(target);

    auto *proc = new QProcess(this);
    proc->setProcessEnvironment(effectiveEnvironment());
    m_launchedProcesses.append(proc);

    m_launching = true;
    m_lastError.clear();
    m_statusMessage = QStringLiteral("'%1' başlatılıyor (%2 / %3)...")
                          .arg(QFileInfo(target).fileName(), profile, renderer);
    emit stateChanged();

    connect(proc, &QProcess::finished, this,
            [this, proc, target](int exitCode, QProcess::ExitStatus exitStatus) {
                const QString stderrText = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                m_launchedProcesses.removeAll(proc);
                proc->deleteLater();
                m_launching = !m_launchedProcesses.isEmpty();

                const bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0);
                if (ok) {
                    m_lastError.clear();
                    m_statusMessage = QStringLiteral("'%1' başarıyla sonlandı.").arg(QFileInfo(target).fileName());
                } else {
                    m_lastError = stderrText.isEmpty()
                        ? QStringLiteral("'%1' %2 çıkış koduyla sonlandı.").arg(QFileInfo(target).fileName()).arg(exitCode)
                        : stderrText.left(400);
                    m_statusMessage = m_lastError;
                }
                emit stateChanged();
                emit launchFinished(target, exitCode, m_statusMessage);
            });

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, target](QProcess::ProcessError) {
                if (proc->state() != QProcess::NotRunning)
                    return;
                m_launchedProcesses.removeAll(proc);
                proc->deleteLater();
                m_launching = !m_launchedProcesses.isEmpty();
                m_lastError = QStringLiteral("'%1' başlatılamadı.").arg(QFileInfo(target).fileName());
                m_statusMessage = m_lastError;
                emit stateChanged();
                emit launchFinished(target, -1, m_lastError);
            });

    proc->start(runner, args);
}

void CompatibilityManager::startCommand(ActiveOp op,
                                        const QString &program,
                                        const QStringList &args,
                                        int timeoutMs,
                                        const QString &contextProfile)
{
    m_activeOp = op;
    m_activeContextProfile = contextProfile;
    m_activeTimedOut = false;
    m_busy = true;

    auto *proc = new QProcess(this);
    proc->setProcessEnvironment(effectiveEnvironment());
    m_activeProcess = proc;

    connect(proc, &QProcess::finished, this, &CompatibilityManager::handleActiveProcessFinished);
    connect(proc, &QProcess::errorOccurred, this, &CompatibilityManager::handleActiveProcessError);

    m_opTimeoutTimer.start(timeoutMs);
    emit stateChanged();
    proc->start(program, args);
}

void CompatibilityManager::handleActiveProcessTimeout()
{
    if (!m_activeProcess)
        return;
    m_activeTimedOut = true;
    m_activeProcess->terminate();
    QTimer::singleShot(1000, this, [proc = m_activeProcess]() {
        if (proc && proc->state() != QProcess::NotRunning)
            proc->kill();
    });
}

void CompatibilityManager::handleActiveProcessError(QProcess::ProcessError error)
{
    if (!m_activeProcess)
        return;
    if (error != QProcess::FailedToStart)
        return;

    m_opTimeoutTimer.stop();
    const ActiveOp op = m_activeOp;
    QProcess *proc = m_activeProcess;
    m_activeProcess = nullptr;
    m_activeOp = ActiveOp::None;
    proc->disconnect(this);
    proc->deleteLater();

    const QString msg = QStringLiteral("Uyumluluk yardımcı aracı başlatılamadı.");
    if (op == ActiveOp::RefreshProbe || op == ActiveOp::RefreshList || op == ActiveOp::RefreshAudit) {
        m_available = false;
        m_busy = false;
        m_lastError = msg;
        m_statusMessage = msg;
        emit stateChanged();
        emit refreshFinished(false);
    } else if (op == ActiveOp::DryRun) {
        m_busy = false;
        m_lastError = msg;
        m_statusMessage = msg;
        m_lastDryRunPlan.clear();
        emit stateChanged();
        emit dryRunCompleted(false, {});
    } else {
        finishActiveOp(false, QStringLiteral("operation"), msg, msg);
    }
}

void CompatibilityManager::handleActiveProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_opTimeoutTimer.stop();
    QProcess *proc = m_activeProcess;
    if (!proc)
        return;

    const QByteArray stdoutBytes = proc->readAllStandardOutput();
    const QByteArray stderrBytes = proc->readAllStandardError();
    const ActiveOp op = m_activeOp;
    const QString ctxProfile = m_activeContextProfile;
    const bool timedOut = m_activeTimedOut;

    m_activeProcess = nullptr;
    m_activeOp = ActiveOp::None;
    m_activeContextProfile.clear();
    m_activeTimedOut = false;
    proc->disconnect(this);
    proc->deleteLater();

    if (timedOut) {
        const QString msg = QStringLiteral("İşlem zaman aşımına uğradı.");
        if (op == ActiveOp::RefreshProbe || op == ActiveOp::RefreshList || op == ActiveOp::RefreshAudit) {
            m_busy = false;
            m_lastError = msg;
            m_statusMessage = msg;
            emit stateChanged();
            emit refreshFinished(false);
        } else if (op == ActiveOp::DryRun) {
            m_busy = false;
            m_lastError = msg;
            m_statusMessage = msg;
            m_lastDryRunPlan.clear();
            emit stateChanged();
            emit dryRunCompleted(false, {});
        } else {
            finishActiveOp(false, QStringLiteral("timeout"), msg, msg);
        }
        return;
    }

    const bool normalZero = (exitStatus == QProcess::NormalExit && exitCode == 0);
    QString parseErr;

    switch (op) {
    case ActiveOp::RefreshProbe:
        if (!normalZero || !parseProbeJson(stdoutBytes, &parseErr)) {
            m_available = false;
            m_busy = false;
            m_lastError = parseErr.isEmpty() ? extractErrorText(stderrBytes, stdoutBytes, exitCode) : parseErr;
            m_statusMessage = m_lastError;
            emit stateChanged();
            emit refreshFinished(false);
            return;
        }
        m_available = true;
        startCommand(ActiveOp::RefreshList, resolvePrefixCtl(),
                     {QStringLiteral("list"), QStringLiteral("--json")}, m_commandTimeoutMs);
        return;

    case ActiveOp::RefreshList:
        if (!normalZero || !parseListJson(stdoutBytes, &parseErr)) {
            m_busy = false;
            m_lastError = parseErr.isEmpty() ? extractErrorText(stderrBytes, stdoutBytes, exitCode) : parseErr;
            m_statusMessage = m_lastError;
            emit stateChanged();
            emit refreshFinished(false);
            return;
        }
        startNextAuditInRefresh();
        return;

    case ActiveOp::RefreshAudit:
        // Note: `audit --json` exits 1 when an audit check fails, while still emitting valid JSON.
        if (!applyAuditJson(ctxProfile, stdoutBytes, &parseErr)) {
            // Keep profile visible with audit unknown/failed detail
            for (int i = 0; i < m_profiles.size(); ++i) {
                QVariantMap item = m_profiles[i].toMap();
                if (item.value(QStringLiteral("profile")).toString() == ctxProfile) {
                    item.insert(QStringLiteral("auditPassed"), false);
                    item.insert(QStringLiteral("auditError"), parseErr);
                    m_profiles[i] = item;
                    break;
                }
            }
        }
        startNextAuditInRefresh();
        return;

    case ActiveOp::InitProfile: {
        QJsonParseError jerr;
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
        if (!normalZero || jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            const QString err = extractErrorText(stderrBytes, stdoutBytes, exitCode);
            finishActiveOp(false, QStringLiteral("init"), err, err);
            return;
        }
        const QString msg = QStringLiteral("'%1' profili başarıyla oluşturuldu ve doğrulandı.").arg(ctxProfile);
        finishActiveOp(true, QStringLiteral("init"), msg);
        refresh();
        return;
    }

    case ActiveOp::AuditProfile: {
        if (!applyAuditJson(ctxProfile, stdoutBytes, &parseErr)) {
            const QString err = parseErr.isEmpty() ? extractErrorText(stderrBytes, stdoutBytes, exitCode) : parseErr;
            finishActiveOp(false, QStringLiteral("audit"), err, err);
            return;
        }
        const QString msg = normalZero
            ? QStringLiteral("'%1' profili izolasyon denetiminden geçti (PASS).").arg(ctxProfile)
            : QStringLiteral("'%1' profili denetiminde düzeltme gereken bulgular var.").arg(ctxProfile);
        finishActiveOp(normalZero, QStringLiteral("audit"), msg, normalZero ? QString() : msg);
        return;
    }

    case ActiveOp::SyncShortcuts: {
        QJsonParseError jerr;
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
        if (!normalZero || jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            const QString err = extractErrorText(stderrBytes, stdoutBytes, exitCode);
            finishActiveOp(false, QStringLiteral("sync-shortcuts"), err, err);
            return;
        }
        const QJsonObject obj = doc.object();
        const int conv = obj.value(QStringLiteral("converted")).toArray().size();
        const int skip = obj.value(QStringLiteral("skipped")).toArray().size();
        const QString msg = QStringLiteral("'%1' kısayolları eşitlendi: %2 dönüştürüldü, %3 atlandı.")
                                .arg(ctxProfile).arg(conv).arg(skip);
        finishActiveOp(true, QStringLiteral("sync-shortcuts"), msg);
        return;
    }

    case ActiveOp::InstallGamingLayers: {
        QJsonParseError jerr;
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
        if (!normalZero || jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            const QString err = extractErrorText(stderrBytes, stdoutBytes, exitCode);
            finishActiveOp(false, QStringLiteral("install-dxvk"), err, err);
            return;
        }
        const QJsonObject obj = doc.object();
        const int dxvkCount = obj.value(QStringLiteral("dxvk_files")).toArray().size();
        const int vkd3dCount = obj.value(QStringLiteral("vkd3d_files")).toArray().size();
        const QString msg = QStringLiteral("'%1' profiline izole DXVK/VKD3D kuruldu (DXVK: %2, VKD3D: %3 dosya).")
                                .arg(ctxProfile).arg(dxvkCount).arg(vkd3dCount);
        finishActiveOp(true, QStringLiteral("install-dxvk"), msg);
        return;
    }

    case ActiveOp::RemoveGamingLayers: {
        QJsonParseError jerr;
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
        if (!normalZero || jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            const QString err = extractErrorText(stderrBytes, stdoutBytes, exitCode);
            finishActiveOp(false, QStringLiteral("remove-dxvk"), err, err);
            return;
        }
        const int removedCount = doc.object().value(QStringLiteral("removed_files")).toArray().size();
        const QString msg = QStringLiteral("'%1' profilinden %2 yönetilen DXVK/VKD3D dosyası kaldırıldı.")
                                .arg(ctxProfile).arg(removedCount);
        finishActiveOp(true, QStringLiteral("remove-dxvk"), msg);
        return;
    }

    case ActiveOp::RemoveProfile: {
        if (!normalZero) {
            const QString err = extractErrorText(stderrBytes, stdoutBytes, exitCode);
            finishActiveOp(false, QStringLiteral("remove"), err, err);
            return;
        }
        const QString msg = QStringLiteral("'%1' profili kaldırıldı.").arg(ctxProfile);
        finishActiveOp(true, QStringLiteral("remove"), msg);
        refresh();
        return;
    }

    case ActiveOp::DryRun: {
        if (!normalZero || !parseDryRunJson(stdoutBytes, &parseErr)) {
            m_busy = false;
            m_lastError = parseErr.isEmpty() ? extractErrorText(stderrBytes, stdoutBytes, exitCode) : parseErr;
            m_statusMessage = m_lastError;
            m_lastDryRunPlan.clear();
            emit stateChanged();
            emit dryRunCompleted(false, {});
            return;
        }
        m_busy = false;
        m_lastError.clear();
        m_statusMessage = QStringLiteral("Önizleme hazır: motor=%1, sandbox=%2")
                              .arg(m_lastDryRunPlan.value(QStringLiteral("effectiveRenderer")).toString(),
                                   m_lastDryRunPlan.value(QStringLiteral("sandboxEnabled")).toBool()
                                       ? QStringLiteral("aktif")
                                       : QStringLiteral("kapalı"));
        emit stateChanged();
        emit dryRunCompleted(true, m_lastDryRunPlan);
        return;
    }

    case ActiveOp::None:
        m_busy = false;
        emit stateChanged();
        return;
    }
}

void CompatibilityManager::finishActiveOp(bool ok,
                                          const QString &opName,
                                          const QString &message,
                                          const QString &errorDetail)
{
    m_busy = false;
    m_lastError = ok ? QString() : errorDetail;
    m_statusMessage = message;
    emit stateChanged();
    emit operationFinished(opName, ok, message);
}

void CompatibilityManager::startNextAuditInRefresh()
{
    if (m_pendingAuditProfiles.isEmpty()) {
        m_busy = false;
        m_lastError.clear();
        m_statusMessage = QStringLiteral("Hazır (%1 izole profil)").arg(m_profiles.size());
        emit stateChanged();
        emit refreshFinished(true);
        return;
    }

    const QString nextProfile = m_pendingAuditProfiles.takeFirst();
    startCommand(ActiveOp::RefreshAudit, resolvePrefixCtl(),
                 {QStringLiteral("audit"), nextProfile, QStringLiteral("--json")},
                 m_commandTimeoutMs, nextProfile);
}

bool CompatibilityManager::parseProbeJson(const QByteArray &stdoutBytes, QString *errorOut)
{
    QJsonParseError jerr;
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
    if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("probe JSON yanıtı geçersiz.");
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject wine = root.value(QStringLiteral("wine")).toObject();
    const QJsonObject vulkan = root.value(QStringLiteral("vulkan")).toObject();
    const QJsonObject layers = root.value(QStringLiteral("translation_layers")).toObject();

    m_wineVersion = wine.value(QStringLiteral("version")).toString(QStringLiteral("Bilinmiyor"));
    m_wineMode = wine.value(QStringLiteral("mode")).toString(QStringLiteral("unknown"));

    m_vulkanAvailable = vulkan.value(QStringLiteral("available")).toBool(false);
    m_hardwareVulkan13 = vulkan.value(QStringLiteral("hardware_vulkan_13")).toBool(false);
    m_vulkanReason = vulkan.value(QStringLiteral("reason")).toString();

    m_vulkanGpuName = QStringLiteral("Bilinmiyor");
    m_vulkanApiVersion = QStringLiteral("N/A");
    const QJsonArray devices = vulkan.value(QStringLiteral("devices")).toArray();
    for (const QJsonValue &val : devices) {
        const QJsonObject dev = val.toObject();
        const bool isHw = dev.value(QStringLiteral("is_hardware")).toBool(false);
        if (isHw || m_vulkanGpuName == QStringLiteral("Bilinmiyor")) {
            m_vulkanGpuName = dev.value(QStringLiteral("name")).toString(QStringLiteral("Bilinmiyor"));
            m_vulkanApiVersion = dev.value(QStringLiteral("api_version")).toString(QStringLiteral("N/A"));
            if (isHw)
                break;
        }
    }

    m_dxvkAvailable = layers.value(QStringLiteral("dxvk_available")).toBool(false);
    m_dxvkSource = layers.value(QStringLiteral("dxvk_source")).toString(QStringLiteral("none"));
    m_vkd3dAvailable = layers.value(QStringLiteral("vkd3d_available")).toBool(false);

    const QString bwrap = root.value(QStringLiteral("bwrap")).toString(QStringLiteral("not-installed"));
    m_bwrapPath = bwrap;
    m_bwrapAvailable = (!bwrap.isEmpty() && bwrap != QStringLiteral("not-installed"));

    const QJsonObject gpuCaps = root.value(QStringLiteral("gpu_capabilities")).toObject();
    if (!gpuCaps.isEmpty()) {
        m_gpuTierLabel = gpuCaps.value(QStringLiteral("tier_label")).toString(QStringLiteral("Bilinmiyor"));
        m_gpuLimitations.clear();
        for (const QJsonValue &lim : gpuCaps.value(QStringLiteral("limitations")).toArray()) {
            const QString s = lim.toString().trimmed();
            if (!s.isEmpty())
                m_gpuLimitations.append(s);
        }
    } else {
        m_gpuTierLabel = m_hardwareVulkan13
            ? QStringLiteral("Donanım Vulkan 1.3+ Uygun")
            : QStringLiteral("Eski/Yazılım -> WineD3D");
        m_gpuLimitations.clear();
    }

    const QJsonObject gamingStack = root.value(QStringLiteral("gaming_stack")).toObject();
    const QJsonObject ac = gamingStack.value(QStringLiteral("anticheat_eula")).toObject();
    const QString notice = ac.value(QStringLiteral("notice")).toString().trimmed();
    if (!notice.isEmpty())
        m_anticheatNotice = notice;
    return true;
}

bool CompatibilityManager::parseListJson(const QByteArray &stdoutBytes, QString *errorOut)
{
    QJsonParseError jerr;
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
    if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("Profil listesi JSON yanıtı geçersiz.");
        return false;
    }

    const QJsonArray arr = doc.object().value(QStringLiteral("profiles")).toArray();
    QVariantList list;
    m_pendingAuditProfiles.clear();

    for (const QJsonValue &val : arr) {
        const QJsonObject obj = val.toObject();
        const QString profile = obj.value(QStringLiteral("profile")).toString();
        if (profile.isEmpty())
            continue;

        QVariantMap item;
        item.insert(QStringLiteral("profile"), profile);
        item.insert(QStringLiteral("template"), obj.value(QStringLiteral("template")).toString(QStringLiteral("unknown")));
        item.insert(QStringLiteral("requestedArch"), obj.value(QStringLiteral("requested_arch")).toString(QStringLiteral("unknown")));
        item.insert(QStringLiteral("effectiveArch"), obj.value(QStringLiteral("effective_arch")).toString(QStringLiteral("unknown")));
        item.insert(QStringLiteral("windowsVersion"), obj.value(QStringLiteral("windows_version")).toString(QStringLiteral("unknown")));
        item.insert(QStringLiteral("renderer"), obj.value(QStringLiteral("renderer")).toString(QStringLiteral("auto")));
        item.insert(QStringLiteral("locale"), obj.value(QStringLiteral("locale")).toString(QStringLiteral("tr_TR.UTF-8")));
        item.insert(QStringLiteral("sandbox"), obj.value(QStringLiteral("sandbox")).toBool(true));
        item.insert(QStringLiteral("wineVersion"), obj.value(QStringLiteral("wine_version")).toString(QStringLiteral("Bilinmiyor")));
        item.insert(QStringLiteral("metadataValid"), obj.value(QStringLiteral("metadata_valid")).toBool(true));
        item.insert(QStringLiteral("auditPassed"), false);
        item.insert(QStringLiteral("zDriveIsolated"), false);
        item.insert(QStringLiteral("homeIsolated"), false);
        item.insert(QStringLiteral("turkishLocaleOk"), false);
        item.insert(QStringLiteral("fontsOk"), false);
        item.insert(QStringLiteral("auditChecks"), QVariantList());

        list.append(item);
        if (isValidProfileName(profile))
            m_pendingAuditProfiles.append(profile);
    }

    m_profiles = list;
    return true;
}

bool CompatibilityManager::applyAuditJson(const QString &profileName,
                                          const QByteArray &stdoutBytes,
                                          QString *errorOut)
{
    QJsonParseError jerr;
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
    if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("Denetim JSON yanıtı geçersiz.");
        return false;
    }

    const QJsonObject obj = doc.object();
    const bool passed = obj.value(QStringLiteral("passed")).toBool(false);
    const QJsonArray checksArr = obj.value(QStringLiteral("checks")).toArray();

    bool zIso = false;
    bool homeIso = false;
    bool trOk = false;
    bool fontOk = false;
    QVariantList checksList;

    for (const QJsonValue &val : checksArr) {
        const QJsonObject c = val.toObject();
        const QString id = c.value(QStringLiteral("id")).toString();
        const bool ok = c.value(QStringLiteral("passed")).toBool(false);
        const QString detail = c.value(QStringLiteral("detail")).toString();

        if (id == QStringLiteral("z_drive_isolated"))
            zIso = ok;
        else if (id == QStringLiteral("home_folders_isolated"))
            homeIso = ok;
        else if (id == QStringLiteral("turkish_locale_registry"))
            trOk = ok;
        else if (id == QStringLiteral("font_substitutes_registry"))
            fontOk = ok;

        QVariantMap cm;
        cm.insert(QStringLiteral("id"), id);
        cm.insert(QStringLiteral("passed"), ok);
        cm.insert(QStringLiteral("detail"), detail);
        checksList.append(cm);
    }

    for (int i = 0; i < m_profiles.size(); ++i) {
        QVariantMap item = m_profiles[i].toMap();
        if (item.value(QStringLiteral("profile")).toString() == profileName) {
            item.insert(QStringLiteral("auditPassed"), passed);
            item.insert(QStringLiteral("zDriveIsolated"), zIso);
            item.insert(QStringLiteral("homeIsolated"), homeIso);
            item.insert(QStringLiteral("turkishLocaleOk"), trOk);
            item.insert(QStringLiteral("fontsOk"), fontOk);
            item.insert(QStringLiteral("auditChecks"), checksList);
            m_profiles[i] = item;
            break;
        }
    }
    return true;
}

bool CompatibilityManager::parseDryRunJson(const QByteArray &stdoutBytes, QString *errorOut)
{
    QJsonParseError jerr;
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes, &jerr);
    if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("Dry-run JSON yanıtı geçersiz.");
        return false;
    }

    const QJsonObject obj = doc.object();
    const QJsonObject rend = obj.value(QStringLiteral("renderer")).toObject();

    QVariantMap plan;
    plan.insert(QStringLiteral("profile"), obj.value(QStringLiteral("profile")).toString());
    plan.insert(QStringLiteral("targetHostPath"), obj.value(QStringLiteral("target_host_path")).toString());
    plan.insert(QStringLiteral("targetWindowsPath"), obj.value(QStringLiteral("target_windows_path")).toString());
    plan.insert(QStringLiteral("targetKind"), obj.value(QStringLiteral("target_kind")).toString());
    plan.insert(QStringLiteral("effectiveArch"), obj.value(QStringLiteral("effective_arch")).toString());
    plan.insert(QStringLiteral("effectiveRenderer"), rend.value(QStringLiteral("effective")).toString());
    plan.insert(QStringLiteral("fallbackApplied"), rend.value(QStringLiteral("fallback_applied")).toBool(false));
    plan.insert(QStringLiteral("rendererReason"), rend.value(QStringLiteral("reason")).toString());
    plan.insert(QStringLiteral("sandboxEnabled"), obj.value(QStringLiteral("sandbox_enabled")).toBool(false));
    plan.insert(QStringLiteral("sandboxTool"), obj.value(QStringLiteral("sandbox_tool")).toString());
    plan.insert(QStringLiteral("sandboxWarning"), obj.value(QStringLiteral("sandbox_warning")).toString());

    m_lastDryRunPlan = plan;
    return true;
}
