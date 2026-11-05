#pragma once
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>

// Reads recipe text only. Never sources PKGBUILD, invokes makepkg, sudo or
// an AUR install helper. Missing privileged build/install capability is explicit.
class SourceReview : public QObject {
    Q_OBJECT
public:
    explicit SourceReview(QObject *parent=nullptr);
    ~SourceReview() override;
    void start(const QVariantList &aur,const QVariantList &flatpak);
    static QVariantMap inspectAur(const QVariantMap &candidate,const QString &base,const QString &commit,
        const QByteArray &pkgbuild,const QByteArray &srcinfo);
    static bool validName(const QString &name);
    static bool validFlatpak(const QVariantMap &entry);
signals:
    void progress(const QString &message);
    void finished(const QVariantList &plan);
private:
    void next();
    void command(const QString &step,const QString &program,const QStringList &args);
    void completed(int code,QProcess::ExitStatus status);
    void finishEntry(const QString &error={});
    QProcess m_process;
    QTimer m_timeout;
    QTemporaryDir m_temp;
    QVariantList m_queue,m_results;
    QVariantMap m_current;
    QString m_step,m_base,m_commit,m_repo;
    QByteArray m_pkgbuild,m_srcinfo;
    QStringList m_missing,m_conflicts;
    bool m_done=false;
};
