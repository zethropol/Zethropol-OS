#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>

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

public slots:
    void check();
    void install();

signals:
    void stateChanged();

private slots:
    void readOutput();
    void finishCleanup();

private:
    QProcess process;
    QProcess cleanupProcess;
    bool m_checking = false;
    bool m_installing = false;
    bool m_cleaningUninstalled = false;
    bool m_available = false;
    bool m_failed = false;
    int m_count = 0;
    QString m_status;
    QString m_output;
    QVariantList m_updates;
};
