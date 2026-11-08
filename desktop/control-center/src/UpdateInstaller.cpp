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
            // A zero exit can still follow a failed optional source. The fresh
            // scan and actual output, rather than this exit code, show the result.
            finish("finished", "Kurulum oturumu sona erdi. Paket durumunu yenilenen rapordan ve işlem çıktısından kontrol edin.");
        } else if (code == 4 || code == 17) {
            finish("cancelled", "Güncelleme işlemi iptal edildi veya başka bir oturum nedeniyle durduruldu. Rapor yeniden taramayla güncellenir.");
        } else {
            finish("error", QString("Kurulum hata veya kesintiyle sonlandı (çıkış kodu %1). Bazı paketler değişmiş olabilir; işlem çıktısını ve yenilenen raporu inceleyin.").arg(code));
        }
    });
}

void UpdateInstaller::start()
{
    if (m_busy || m_scanner->busy() || !m_scanner->hasUpdates())
        return;
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
