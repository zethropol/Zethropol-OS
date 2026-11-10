#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>
#include <QHash>
#include <QString>
#include <QSet>
#include <sys/types.h>

class ProcessService : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QVariantList processes READ processes NOTIFY stateChanged)
    Q_PROPERTY(QVariantList treeProcesses READ treeProcesses NOTIFY stateChanged)
    Q_PROPERTY(int totalProcesses READ totalProcesses NOTIFY stateChanged)
    Q_PROPERTY(int userProcesses READ userProcesses NOTIFY stateChanged)
    Q_PROPERTY(int backgroundProcesses READ backgroundProcesses NOTIFY stateChanged)
    Q_PROPERTY(double totalCpuPercent READ totalCpuPercent NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalMemoryBytes READ totalMemoryBytes NOTIFY stateChanged)
    Q_PROPERTY(qint64 usedMemoryBytes READ usedMemoryBytes NOTIFY stateChanged)
    Q_PROPERTY(QString usedMemoryFormatted READ usedMemoryFormatted NOTIFY stateChanged)
    Q_PROPERTY(bool autoRefresh READ autoRefresh WRITE setAutoRefresh NOTIFY autoRefreshChanged)
    Q_PROPERTY(int refreshInterval READ refreshInterval WRITE setRefreshInterval NOTIFY refreshIntervalChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(QString filterType READ filterType WRITE setFilterType NOTIFY filterTypeChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool actionBusy READ actionBusy NOTIFY actionBusyChanged)

public:
    explicit ProcessService(QObject *parent = nullptr);
    ~ProcessService() override = default;

    QVariantList processes() const;
    QVariantList treeProcesses() const;
    int totalProcesses() const;
    int userProcesses() const;
    int backgroundProcesses() const;
    double totalCpuPercent() const;
    qint64 totalMemoryBytes() const;
    qint64 usedMemoryBytes() const;
    QString usedMemoryFormatted() const;

    bool autoRefresh() const;
    void setAutoRefresh(bool enabled);

    int refreshInterval() const;
    void setRefreshInterval(int msec);

    QString searchQuery() const;
    void setSearchQuery(const QString &query);

    QString filterType() const;
    void setFilterType(const QString &type);

    QString sortField() const;
    bool sortAscending() const;

    QString statusMessage() const;
    bool actionBusy() const;

    Q_INVOKABLE void sortBy(const QString &field);
    Q_INVOKABLE bool terminateProcess(int pid, quint64 expectedStartTime = 0);
    Q_INVOKABLE bool forceKillProcess(int pid, quint64 expectedStartTime, bool userConfirmed);
    Q_INVOKABLE bool isCritical(int pid) const;
    Q_INVOKABLE bool canTerminate(int pid) const;
    Q_INVOKABLE quint64 processStartTime(int pid) const;
    Q_INVOKABLE QVariantMap getProcessDetails(int pid) const;

public slots:
    void refresh();

signals:
    void stateChanged();
    void autoRefreshChanged();
    void refreshIntervalChanged();
    void searchQueryChanged();
    void filterTypeChanged();
    void sortChanged();
    void statusMessageChanged();
    void actionBusyChanged();
    void processRequiresForceKill(int pid, const QString &name, quint64 startTime);
    void actionFinished(bool success, const QString &message);

private:
    struct RawProcessInfo {
        int pid = 0;
        int ppid = 0;
        QString name;
        QString state;
        QString cmdline;
        quint64 startTime = 0;
        qint64 utime = 0;
        qint64 stime = 0;
        qint64 vmRssKb = 0;
        uid_t uid = 0;
        QString user;
        int numThreads = 1;
        bool isBackground = true;
        bool isCritical = false;
        bool canTerminate = false;
        QString protectionReason;
        double cpuPercent = 0.0;
        double memoryPercent = 0.0;
        QString memoryFormatted;
        QList<int> childPids;
    };

    void scanProcesses();
    void calculateMetrics(QList<RawProcessInfo> &rawList);
    void buildProcessHierarchy(QList<RawProcessInfo> &rawList);
    void applyFilterAndSort();

    quint64 readProcessStartTime(int pid) const;
    uid_t readProcessUid(int pid) const;
    QString readProcessName(int pid) const;
    bool verifyPidIdentity(int pid, quint64 expectedStartTime, QString *errorMessage = nullptr) const;

    static qint64 readSystemTotalCpuTicks();
    static qint64 readSystemTotalMemory();
    static QString formatMemory(qint64 bytes);

    QTimer m_refreshTimer;
    bool m_autoRefresh = true;
    int m_refreshInterval = 2000;
    QString m_searchQuery;
    QString m_filterType = QStringLiteral("all"); // "all", "apps", "background"
    QString m_sortField = QStringLiteral("cpu"); // "cpu", "memory", "pid", "name"
    bool m_sortAscending = false;
    QString m_statusMessage;
    bool m_actionBusy = false;

    qint64 m_prevSystemTotalCpuTicks = 0;
    QHash<int, qint64> m_prevProcCpuTicks; // pid -> (utime + stime)
    qint64 m_systemTotalMemory = 0;

    QList<RawProcessInfo> m_scannedProcesses;
    QVariantList m_processes;
    QVariantList m_treeProcesses;

    int m_totalProcesses = 0;
    int m_userProcesses = 0;
    int m_backgroundProcesses = 0;
    double m_totalCpuPercent = 0.0;
    qint64 m_usedMemoryBytes = 0;
};
