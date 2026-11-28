#include "UpdateScanner.h"

#include <QDateTime>
#include <QFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

namespace {

const QSet<QString> &protectedOrphanExactPackages()
{
    static const QSet<QString> kExact = {
        QStringLiteral("zethropol"),
        QStringLiteral("base"),
        QStringLiteral("base-devel"),
        QStringLiteral("glibc"),
        QStringLiteral("gcc-libs"),
        QStringLiteral("libgcc"),
        QStringLiteral("dbus"),
        QStringLiteral("dbus-broker"),
        QStringLiteral("dbus-broker-units"),
        QStringLiteral("polkit"),
        QStringLiteral("polkit-kde-agent"),
        QStringLiteral("pacman"),
        QStringLiteral("pacman-contrib"),
        QStringLiteral("arch-update"),
        QStringLiteral("cachy-update"),
        QStringLiteral("snapper"),
        QStringLiteral("snap-pac"),
        QStringLiteral("limine-snapper-sync"),
        QStringLiteral("btrfs-progs"),
        QStringLiteral("mkinitcpio"),
        QStringLiteral("dracut"),
        QStringLiteral("sudo"),
        QStringLiteral("sudo-rs"),
        QStringLiteral("networkmanager"),
        QStringLiteral("ufw"),
        QStringLiteral("firewalld"),
        QStringLiteral("pipewire"),
        QStringLiteral("wireplumber"),
        QStringLiteral("mesa"),
        QStringLiteral("konsole"),
        QStringLiteral("dolphin"),
        QStringLiteral("systemsettings"),
        QStringLiteral("sddm"),
        QStringLiteral("power-profiles-daemon"),
        QStringLiteral("smartmontools"),
        QStringLiteral("fwupd"),
        QStringLiteral("util-linux"),
        QStringLiteral("efibootmgr"),
        QStringLiteral("cryptsetup"),
        QStringLiteral("lvm2"),
        QStringLiteral("amd-ucode"),
        QStringLiteral("intel-ucode"),
        QStringLiteral("shim"),
        QStringLiteral("sbctl"),
    };
    return kExact;
}

const QStringList &protectedOrphanPrefixes()
{
    static const QStringList kPrefixes = {
        QStringLiteral("zethropol-"),
        QStringLiteral("linux"),
        QStringLiteral("systemd"),
        QStringLiteral("plasma-"),
        QStringLiteral("kwin"),
        QStringLiteral("qt6-"),
        QStringLiteral("limine"),
        QStringLiteral("grub"),
    };
    return kPrefixes;
}

} // namespace

bool UpdateScanner::isValidPackageName(const QString &package)
{
    if (package.isEmpty() || package.size() > 128)
        return false;
    if (package.startsWith(QLatin1Char('-')) || package.startsWith(QLatin1Char('.')))
        return false;
    static const QRegularExpression pkgRe(QStringLiteral("^[a-z0-9@._+-]+$"));
    return pkgRe.match(package).hasMatch();
}

bool UpdateScanner::isProtectedOrphanPackage(const QString &package)
{
    const QString lower = package.trimmed().toLower();
    if (lower.isEmpty())
        return true;
    if (protectedOrphanExactPackages().contains(lower))
        return true;
    for (const QString &prefix : protectedOrphanPrefixes()) {
        if (lower.startsWith(prefix))
            return true;
    }
    return false;
}

UpdateScanner::UpdateScanner(QObject *parent)
    : QObject(parent),
      m_configurationPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                          + "/arch-update/arch-update.conf")
{
    for (const auto &title : {"Resmî depolar", "AUR", "Flatpak"})
        m_reports.append(QVariantMap{{"title", QString::fromUtf8(title)},
                                     {"state", "pending"}, {"detail", "Tarama bekleniyor."},
                                     {"packages", QVariantList{}}});
    QTimer::singleShot(0, this, &UpdateScanner::scan);
}

UpdateScanner::~UpdateScanner()
{
    for (auto *process : m_processes) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

QString UpdateScanner::summary() const
{
    if (m_busy)
        return "Güncelleme kaynakları taranıyor…";
    if (m_completedAt.isEmpty())
        return "İlk tarama hazırlanıyor…";
    int count = 0;
    bool incomplete = false;
    for (const auto &value : m_reports) {
        const auto report = value.toMap();
        count += report.value("packages").toList().size();
        incomplete |= report.value("state") == "error" || report.value("state") == "warning";
    }
    if (incomplete)
        return QString("Tarama uyarılarla tamamlandı. Bildirilen güncelleme: %1.").arg(count);
    return count ? QString("Tarama tamamlandı. %1 güncelleme bulundu.").arg(count)
                 : "Tarama tamamlandı. Etkin kaynaklarda güncelleme bulunamadı.";
}

bool UpdateScanner::hasUpdates() const
{
    for (const auto &report : m_reports)
        if (!report.toMap().value("packages").toList().isEmpty())
            return true;
    return false;
}

void UpdateScanner::scan()
{
    if (m_busy || m_installationRunning)
        return;
    m_busy = true;
    m_starting = true;
    m_completedAt.clear();
    m_orphanPackages.clear();
    m_removableOrphanPackages.clear();
    m_protectedOrphanPackages.clear();
    for (int i = 0; i < m_reports.size(); ++i)
        update(i, "pending", "Tarama bekleniyor.");

    QFile config(m_configurationPath);
    QString text;
    if (config.open(QIODevice::ReadOnly))
        text = QString::fromUtf8(config.readAll());
    const auto enabled = [&text](const QString &option) {
        return QRegularExpression("^\\s*" + option + "\\s*$",
                                  QRegularExpression::MultilineOption).match(text).hasMatch();
    };
    int timeout = 30;
    const auto configuredTimeout = QRegularExpression(
        "^\\s*UpdateCheckTimeout\\s*=\\s*([0-9]+)\\s*$",
        QRegularExpression::MultilineOption).match(text);
    if (configuredTimeout.hasMatch()) {
        bool valid = false;
        const auto seconds = configuredTimeout.captured(1).toInt(&valid);
        if (valid && seconds <= 3600)
            timeout = seconds;
    }

    if (!m_database.isValid())
        update(0, "error", "Geçici paket veritabanı oluşturulamadı.");
    else
        start(0, QStandardPaths::findExecutable("checkupdates"), {"--nocolor"}, timeout);

    if (enabled("NoAUR")) {
        update(1, "disabled", "Güncelleme yapılandırmasında AUR taraması kapalı.");
    } else {
        QString helper;
        const auto configuredHelper = QRegularExpression(
            "^\\s*AURHelper\\s*=\\s*(paru|yay|pikaur)\\s*$",
            QRegularExpression::MultilineOption).match(text);
        if (configuredHelper.hasMatch()) {
            helper = QStandardPaths::findExecutable(configuredHelper.captured(1));
        } else {
            for (const auto &candidate : {"paru", "yay", "pikaur"}) {
                helper = QStandardPaths::findExecutable(candidate);
                if (!helper.isEmpty())
                    break;
            }
        }
        start(1, helper, {"--color", "never", "-Qua"}, timeout);
    }

    if (enabled("NoFlatpak"))
        update(2, "disabled", "Güncelleme yapılandırmasında Flatpak taraması kapalı.");
    else
        // Query remote updates without invoking the interactive AppStream updater.
        start(2, QStandardPaths::findExecutable("flatpak"),
              {"remote-ls", "--updates", "--columns=application,name,version"}, timeout);

    // Read-only orphan package detection (never removes anything automatically).
    startOrphanScan(timeout);

    m_starting = false;
    finishIfReady();
}

void UpdateScanner::startOrphanScan(int timeout)
{
    const QString pacmanBin = QStandardPaths::findExecutable(QStringLiteral("pacman"));
    if (pacmanBin.isEmpty())
        return;

    auto *process = new QProcess(this);
    m_processes.append(process);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(environment);
    process->setStandardInputFile(QProcess::nullDevice());

    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->setProperty("handled", true);
        m_processes.removeOne(process);
        process->deleteLater();
        finishIfReady();
    });
    connect(process, &QProcess::finished, this, [this, process, timer](int code, QProcess::ExitStatus status) {
        timer->stop();
        if (process->property("handled").toBool())
            return;
        process->setProperty("handled", true);
        m_processes.removeOne(process);
        if (!process->property("timedOut").toBool()
            && status == QProcess::NormalExit
            && code == 0) {
            const QString out = QString::fromUtf8(process->readAllStandardOutput());
            QStringList orphans;
            QStringList removable;
            QStringList protectedOrphans;
            QSet<QString> seen;
            for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const QString trimmed = line.trimmed();
                if (trimmed.isEmpty() || seen.contains(trimmed))
                    continue;
                seen.insert(trimmed);
                orphans.append(trimmed);
                if (isProtectedOrphanPackage(trimmed)) {
                    protectedOrphans.append(trimmed);
                } else if (isValidPackageName(trimmed)) {
                    removable.append(trimmed);
                }
            }
            m_orphanPackages = orphans;
            m_removableOrphanPackages = removable;
            m_protectedOrphanPackages = protectedOrphans;
            emit changed();
        }
        process->deleteLater();
        finishIfReady();
    });

    process->start(pacmanBin, {QStringLiteral("-Qtdq")});
    if (timeout > 0)
        timer->start(qMin(timeout, 15) * 1000);
}

void UpdateScanner::start(int index, const QString &program,
                               const QStringList &arguments, int timeout)
{
    if (program.isEmpty()) {
        update(index, "error", index == 1 ? "AUR yardımcısı bulunamadı."
                                         : "Tarama için gerekli araç bulunamadı.");
        return;
    }
    auto *process = new QProcess(this);
    m_processes.append(process);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C");
    if (index == 0)
        environment.insert("CHECKUPDATES_DB", m_database.path() + "/sync");
    process->setProcessEnvironment(environment);
    process->setStandardInputFile(QProcess::nullDevice());
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    connect(process, &QProcess::errorOccurred, this, [this, index, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->setProperty("handled", true);
        m_processes.removeOne(process);
        update(index, "error", "Tarama başlatılamadı: " + process->errorString());
        process->deleteLater();
        finishIfReady();
    });
    connect(process, &QProcess::finished, this, [this, index, process, timer](int code, QProcess::ExitStatus status) {
        timer->stop();
        finish(index, process, code, status);
    });
    update(index, "scanning", "Taranıyor…");
    process->start(program, arguments);
    if (timeout > 0)
        timer->start(timeout * 1000);
}

void UpdateScanner::finish(int index, QProcess *process, int exitCode,
                               QProcess::ExitStatus exitStatus)
{
    if (process->property("handled").toBool())
        return;
    process->setProperty("handled", true);
    m_processes.removeOne(process);
    const auto output = QString::fromUtf8(process->readAllStandardOutput());
    const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed();
    const bool noUpdates = (index == 0 && exitCode == 2 && output.trimmed().isEmpty())
                        || (index == 1 && exitCode == 1 && output.trimmed().isEmpty() && error.isEmpty());
    if (process->property("timedOut").toBool()) {
        update(index, "error", "Tarama zaman aşımına uğradı. Yeniden tarayabilirsiniz.");
    } else if (exitStatus != QProcess::NormalExit
               || (exitCode != 0 && !noUpdates)) {
        const QString reason = !error.isEmpty() ? error.left(16384)
            : (!output.trimmed().isEmpty() ? output.trimmed().left(16384)
                                           : QString("Araç çıkış kodu: %1").arg(exitCode));
        update(index, "error", "Tarama tamamlanamadı: " + reason);
    } else {
        QVariantList packages;
        bool unparsed = false;
        const QRegularExpression pattern("^(\\S+)\\s+(\\S+)\\s+->\\s+(\\S+)(?:\\s+.*)?$");
        for (const auto &line : output.split('\n', Qt::SkipEmptyParts)) {
            if (line.trimmed().isEmpty())
                continue;
            if (index == 2) {
                const auto fields = line.split('\t');
                if (fields.size() < 2 || fields.size() > 3 || fields[0].trimmed().isEmpty()) {
                    unparsed = true;
                    continue;
                }
                const QString id = fields[0].trimmed();
                const QString name = fields[1].trimmed();
                const QString version = fields.size() == 3 ? fields[2].trimmed() : QString();
                packages.append(QVariantMap{{"name", name.isEmpty() ? id : name},
                    {"identifier", id}, {"currentVersion", ""}, {"newVersion", version}});
            } else {
                // Match the update tool's exclusion of ignored AUR updates.
                if (index == 1 && line.trimmed().endsWith("[ignored]"))
                    continue;
                const auto match = pattern.match(line.trimmed());
                if (!match.hasMatch()) {
                    unparsed = true;
                    continue;
                }
                packages.append(QVariantMap{{"name", match.captured(1)}, {"identifier", ""},
                    {"currentVersion", match.captured(2)}, {"newVersion", match.captured(3)}});
            }
        }
        QString detail = packages.isEmpty() ? "Güncelleme bulunamadı."
                                           : QString("%1 güncelleme bulundu.").arg(packages.size());
        if (unparsed)
            detail += " Bazı sonuçlar okunamadı; liste eksik olabilir.";
        if (!error.isEmpty())
            detail += " Uyarı: " + error.left(16384);
        update(index, unparsed || !error.isEmpty() ? "warning" : "complete", detail, packages);
    }
    process->deleteLater();
    finishIfReady();
}

void UpdateScanner::update(int index, const QString &state, const QString &detail,
                                const QVariantList &packages)
{
    auto report = m_reports[index].toMap();
    report["state"] = state;
    report["detail"] = detail;
    report["packages"] = packages;
    m_reports[index] = report;
    emit changed();
}

void UpdateScanner::finishIfReady()
{
    if (m_starting || !m_busy || !m_processes.isEmpty())
        return;
    m_busy = false;
    m_completedAt = QDateTime::currentDateTime().toString("dd.MM.yyyy HH:mm:ss");
    emit changed();
    emit scanFinished();
}
