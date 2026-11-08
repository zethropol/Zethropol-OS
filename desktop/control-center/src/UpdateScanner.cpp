#include "UpdateScanner.h"

#include <QDateTime>
#include <QFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

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

    m_starting = false;
    finishIfReady();
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
    if (process->property("timedOut").toBool()) {
        update(index, "error", "Tarama zaman aşımına uğradı. Yeniden tarayabilirsiniz.");
    } else if (exitStatus != QProcess::NormalExit
               || (exitCode != 0 && !(index == 0 && exitCode == 2 && output.trimmed().isEmpty()))) {
        update(index, "error", "Tarama tamamlanamadı: "
               + (error.isEmpty() ? QString("Araç çıkış kodu: %1").arg(exitCode) : error.left(1500)));
    } else {
        QVariantList packages;
        bool unparsed = false;
        const QRegularExpression pattern("^(\\S+)\\s+(\\S+)\\s+->\\s+(\\S+)(?:\\s+.*)?$");
        for (const auto &line : output.split('\n', Qt::SkipEmptyParts)) {
            if (line.trimmed().isEmpty())
                continue;
            if (index == 2) {
                const auto fields = line.split('\t');
                if (fields.size() != 3 || fields[0].trimmed().isEmpty()) {
                    unparsed = true;
                    continue;
                }
                packages.append(QVariantMap{{"name", fields[1].isEmpty() ? fields[0] : fields[1]},
                    {"identifier", fields[0]}, {"currentVersion", ""}, {"newVersion", fields[2].trimmed()}});
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
            detail += " Uyarı: " + error.left(1500);
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
