#pragma once

#include <QObject>
#include <QProcess>
#include <QStringDecoder>

class UpdateScanner;
class UpdateInstallerTest;

class UpdateInstaller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)

public:
    explicit UpdateInstaller(UpdateScanner *scanner, QObject *parent = nullptr);
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString message() const { return m_message; }
    QString output() const { return m_output; }
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
};
