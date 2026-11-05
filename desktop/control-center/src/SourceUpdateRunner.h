#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

class SourceUpdateRunner : public QObject {
    Q_OBJECT
public:
    explicit SourceUpdateRunner(QObject *parent=nullptr);
    ~SourceUpdateRunner() override;
    void start(const QVariantList &plan);
    QString localExecution() const { return m_execution; }
    bool cancelLocalBuild(const QString &execution);
signals:
    void localBuildState(const QVariantMap &state);
    void progress(const QString &message);
    void finished(const QVariantList &results);
protected:
    virtual bool desktopUser() const;
    virtual void dispatch(const QStringList &args);
    void reply(int code,QProcess::ExitStatus status,const QByteArray &output,const QByteArray &error);
private:
    friend class SourceWorkflowTest;
    void reconcileLocalBuild();
    void consumeBuildOutput(const QByteArray &output);
    void next();
    QString m_execution, m_journalOperation;
    QByteArray m_buildBuffer;
    QVariantMap m_buildResult;
    bool m_buildProtocolFailed=false;
    bool m_localCancelRequested=false;
    void finishEntry(const QString &state,const QString &reason);
    QProcess m_process;
    QTimer m_timeout;
    QVariantList m_queue,m_results;
    QVariantMap m_current;
    QString m_step;
    bool m_done=false;
};
