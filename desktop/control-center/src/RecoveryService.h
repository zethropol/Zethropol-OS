#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>

class RecoveryService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool permissionRequired READ permissionRequired NOTIFY stateChanged)
    Q_PROPERTY(QString filesystem READ filesystem NOTIFY stateChanged)
    Q_PROPERTY(QString snapshotTool READ snapshotTool NOTIFY stateChanged)
    Q_PROPERTY(int snapshotCount READ snapshotCount NOTIFY stateChanged)
    Q_PROPERTY(int currentSnapshotId READ currentSnapshotId NOTIFY stateChanged)
    Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recoveryEvents READ recoveryEvents NOTIFY stateChanged)
    Q_PROPERTY(QVariantList recoveryPoints READ recoveryPoints NOTIFY stateChanged)
    Q_PROPERTY(bool actionBusy READ actionBusy NOTIFY stateChanged)
    Q_PROPERTY(QString actionStatus READ actionStatus NOTIFY stateChanged)

public:
    explicit RecoveryService(QObject *parent = nullptr);

    bool checking() const;
    bool available() const;
    bool permissionRequired() const;
    QString filesystem() const;
    QString snapshotTool() const;
    int snapshotCount() const;
    int currentSnapshotId() const;
    QVariantList snapshots() const;
    QVariantList recoveryEvents() const;
    QVariantList recoveryPoints() const;
    bool actionBusy() const;
    QString actionStatus() const;

public slots:
    void check();
    void refresh();
    void createRecoveryPoint(const QString &description);
    void deleteRecoveryPoint(int snapshotId);

signals:
    void stateChanged();

private slots:
    void readOutput();
    void readError();

private:
    void startProcess(const QString &program, const QStringList &arguments);

    QProcess process;
    QProcess actionProcess;
    bool m_checking = false;
    bool m_available = false;
    bool m_permissionRequired = false;
    QString m_filesystem = QStringLiteral("Unknown");
    QString m_snapshotTool = QStringLiteral("Unknown");
    int m_snapshotCount = 0;
    int m_currentSnapshotId = 0;
    QVariantList m_snapshots;
    QVariantList m_recoveryEvents;
    QVariantList m_recoveryPoints;
    bool m_actionBusy = false;
    QString m_actionStatus;
    QString m_actionOperation;
};
