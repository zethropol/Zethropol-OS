#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>
#include <QStringList>

class UpdateService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool installing READ installing NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString output READ output NOTIFY stateChanged)
    Q_PROPERTY(QVariantList updates READ updates NOTIFY stateChanged)
    Q_PROPERTY(QStringList orphanPackages READ orphanPackages NOTIFY stateChanged)
    Q_PROPERTY(int orphanCount READ orphanCount NOTIFY stateChanged)
    Q_PROPERTY(bool removingOrphans READ removingOrphans NOTIFY stateChanged)

public:
    explicit UpdateService(QObject *parent = nullptr);

    bool checking() const;
    bool installing() const;
    bool available() const;
    bool failed() const;
    int count() const;
    QString status() const;
    QString output() const;
    QVariantList updates() const;
    QStringList orphanPackages() const;
    int orphanCount() const;
    bool removingOrphans() const;

public slots:
    void check();
    void install();
    void removeOrphans();

signals:
    void stateChanged();

private slots:
    void readOutput();
    void finishCleanup();
    void finishOrphanScan();

private:
    QProcess process;
    QProcess cleanupProcess;
    QProcess orphanProcess;
    bool m_checking = false;
    bool m_installing = false;
    bool m_removingOrphans = false;
    bool m_available = false;
    bool m_failed = false;
    int m_count = 0;
    QString m_status;
    QString m_output;
    QVariantList m_updates;
    QStringList m_orphanPackages;
};
