#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QVariantList>

class UpdateScannerTest;
class UpdateInstaller;

class UpdateScanner : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList reports READ reports NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString completedAt READ completedAt NOTIFY changed)
    Q_PROPERTY(bool hasUpdates READ hasUpdates NOTIFY changed)

public:
    explicit UpdateScanner(QObject *parent = nullptr);
    ~UpdateScanner() override;

    bool busy() const { return m_busy; }
    QVariantList reports() const { return m_reports; }
    QString summary() const;
    QString completedAt() const { return m_completedAt; }
    bool hasUpdates() const;
    Q_INVOKABLE void scan();

signals:
    void changed();
    void scanFinished();

private:
    friend class UpdateScannerTest;
    friend class UpdateInstaller;
    void start(int index, const QString &program, const QStringList &arguments, int timeout);
    void finish(int index, QProcess *process, int exitCode, QProcess::ExitStatus exitStatus);
    void update(int index, const QString &state, const QString &detail,
                const QVariantList &packages = {});
    void finishIfReady();

    QVariantList m_reports;
    QList<QProcess *> m_processes;
    QTemporaryDir m_database;
    QString m_configurationPath;
    QString m_completedAt;
    bool m_busy = false;
    bool m_starting = false;
    bool m_installationRunning = false;
};
