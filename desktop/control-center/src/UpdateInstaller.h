#pragma once

#include <QObject>
#include <QProcess>
#include <QStringDecoder>
#include <functional>

class UpdateScanner;
class UpdateInstallerTest;

struct PreUpdateGateResult {
    bool ok = false;
    QString code;
    QString message;
    quint32 snapshotId = 0;
};

class UpdateInstaller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString recoveryGuidance READ recoveryGuidance NOTIFY changed)
    Q_PROPERTY(bool liveExecutionEnabled READ liveExecutionEnabled CONSTANT)
    Q_PROPERTY(bool officialRepoReady READ officialRepoReady NOTIFY changed)

public:
    explicit UpdateInstaller(UpdateScanner *scanner, QObject *parent = nullptr);
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString message() const { return m_message; }
    QString output() const { return m_output; }
    QString recoveryGuidance() const { return m_recoveryGuidance; }
    bool liveExecutionEnabled() const { return m_liveExecutionEnabled; }
    bool officialRepoReady() const;
    static bool isOfficialRepoScanComplete(const UpdateScanner *scanner, QString *reason = nullptr);
    static bool isCriticalBootUpdate(const UpdateScanner *scanner);
    Q_INVOKABLE void start();
    Q_INVOKABLE void sendInput(const QString &text);
    Q_INVOKABLE void sendKey(const QString &text);

signals:
    void changed();
    void outputChanged();
    void sessionFinished();

private:
    friend class UpdateInstallerTest;
    void readOutput();
    void finish(const QString &status, const QString &message);

    UpdateScanner *m_scanner;
    QProcess m_process;
    QString m_sessionTool;
    QString m_updater;
    QString m_status = "idle";
    QString m_message;
    QString m_rawOutput;
    QString m_output;
    QStringDecoder m_decoder{QStringDecoder::Utf8};
    bool m_busy = false;
    bool m_recoveryGateEnabled = true;
    bool m_liveExecutionEnabled = false;
    QString m_updateOperationId;
    quint32 m_verifiedSnapshotId = 0;
    QString m_recoveryGuidance;
    std::function<PreUpdateGateResult(const QString &updateOpId, bool criticalBootUpdate)> m_recoveryGate;
    std::function<void(const QString &updateOpId, const QString &outcome)> m_recoveryComplete;
};
