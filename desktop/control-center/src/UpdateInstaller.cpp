#include "UpdateInstaller.h"
#include "UpdateScanner.h"

#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

UpdateInstaller::UpdateInstaller(UpdateScanner *scanner, QObject *parent)
    : QObject(parent), m_scanner(scanner),
      m_sessionTool(QStandardPaths::findExecutable("script")),
      m_updater(QStandardPaths::findExecutable("arch-update"))
{
    if (m_updater.isEmpty())
        m_updater = QStandardPaths::findExecutable("cachy-update");

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
            if (m_output.contains(QRegularExpression(QStringLiteral("(?:reboot required|yeniden başlatma gerekiyor|rebooting to finalize|yeniden başlatmayı düşünün)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Güncellemeler uygulandı. Çekirdek güncellemesinin tamamlanması için sistemi yeniden başlatmanız gerekir.");
            } else if (m_output.contains(QRegularExpression(QStringLiteral("(?:an error has occurred during the update process|güncelleme sırasında bir hata oluştu)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Kurulum tamamlandı, ancak bazı kaynaklarda hata oluştu. İşlem çıktısını ve yenilenen raporu inceleyin.");
            } else if (m_output.contains(QRegularExpression(QStringLiteral("(?:the update has been applied|güncelleme uygulandı)"), QRegularExpression::CaseInsensitiveOption))) {
                finish("finished", "Güncellemeler başarıyla kuruldu. Paket durumu yenilenen rapordan kontrol edilebilir.");
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

    m_rawOutput.clear();
    m_output.clear();
    m_decoder = QStringDecoder(QStringDecoder::Utf8);
    emit outputChanged();
    auto environment = QProcessEnvironment::systemEnvironment();
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
    emit changed();
    m_scanner->scan();
    emit sessionFinished();
}
