#include "ProcessService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>
#include <sys/types.h>

namespace {

const QSet<QString> s_criticalProcessNames = {
    QStringLiteral("systemd"),
    QStringLiteral("systemd-journald"),
    QStringLiteral("systemd-logind"),
    QStringLiteral("systemd-udevd"),
    QStringLiteral("systemd-resolved"),
    QStringLiteral("dbus-broker"),
    QStringLiteral("dbus-daemon"),
    QStringLiteral("polkitd"),
    QStringLiteral("auditd"),
    QStringLiteral("kthreadd")
};

QString usernameFromUid(uid_t uid)
{
    static QHash<uid_t, QString> userCache;
    auto it = userCache.find(uid);
    if (it != userCache.end())
        return it.value();

    struct passwd *pw = getpwuid(uid);
    QString name = pw ? QString::fromLocal8Bit(pw->pw_name) : QString::number(uid);
    userCache.insert(uid, name);
    return name;
}

QString formatStateName(const QString &code)
{
    if (code == QStringLiteral("R")) return QStringLiteral("Çalışıyor");
    if (code == QStringLiteral("S")) return QStringLiteral("Uyuyor");
    if (code == QStringLiteral("D")) return QStringLiteral("Disk Bekliyor");
    if (code == QStringLiteral("Z")) return QStringLiteral("Zombi");
    if (code == QStringLiteral("T") || code == QStringLiteral("t")) return QStringLiteral("Durduruldu");
    if (code == QStringLiteral("I")) return QStringLiteral("Boşta (Idle)");
    return code;
}

} // namespace

ProcessService::ProcessService(QObject *parent)
    : QObject(parent)
{
    m_systemTotalMemory = readSystemTotalMemory();
    m_prevSystemTotalCpuTicks = readSystemTotalCpuTicks();

    connect(&m_refreshTimer, &QTimer::timeout, this, &ProcessService::refresh);
    m_refreshTimer.setInterval(m_refreshInterval);
    if (m_autoRefresh)
        m_refreshTimer.start();

    refresh();
}

QVariantList ProcessService::processes() const
{
    return m_processes;
}

QVariantList ProcessService::treeProcesses() const
{
    return m_treeProcesses;
}

int ProcessService::totalProcesses() const
{
    return m_totalProcesses;
}

int ProcessService::userProcesses() const
{
    return m_userProcesses;
}

int ProcessService::backgroundProcesses() const
{
    return m_backgroundProcesses;
}

double ProcessService::totalCpuPercent() const
{
    return m_totalCpuPercent;
}

qint64 ProcessService::totalMemoryBytes() const
{
    return m_systemTotalMemory;
}

qint64 ProcessService::usedMemoryBytes() const
{
    return m_usedMemoryBytes;
}

QString ProcessService::usedMemoryFormatted() const
{
    return formatMemory(m_usedMemoryBytes);
}

bool ProcessService::autoRefresh() const
{
    return m_autoRefresh;
}

void ProcessService::setAutoRefresh(bool enabled)
{
    if (m_autoRefresh == enabled)
        return;

    m_autoRefresh = enabled;
    if (m_autoRefresh)
        m_refreshTimer.start();
    else
        m_refreshTimer.stop();

    emit autoRefreshChanged();
}

int ProcessService::refreshInterval() const
{
    return m_refreshInterval;
}

void ProcessService::setRefreshInterval(int msec)
{
    if (msec < 500)
        msec = 500;

    if (m_refreshInterval == msec)
        return;

    m_refreshInterval = msec;
    m_refreshTimer.setInterval(m_refreshInterval);
    emit refreshIntervalChanged();
}

QString ProcessService::searchQuery() const
{
    return m_searchQuery;
}

void ProcessService::setSearchQuery(const QString &query)
{
    if (m_searchQuery == query)
        return;

    m_searchQuery = query;
    emit searchQueryChanged();
    applyFilterAndSort();
    emit stateChanged();
}

QString ProcessService::filterType() const
{
    return m_filterType;
}

void ProcessService::setFilterType(const QString &type)
{
    if (m_filterType == type)
        return;

    m_filterType = type;
    emit filterTypeChanged();
    applyFilterAndSort();
    emit stateChanged();
}

QString ProcessService::sortField() const
{
    return m_sortField;
}

bool ProcessService::sortAscending() const
{
    return m_sortAscending;
}

void ProcessService::sortBy(const QString &field)
{
    if (m_sortField == field) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortField = field;
        // Default descending for CPU and memory, ascending for name and PID
        m_sortAscending = (field == QStringLiteral("name") || field == QStringLiteral("pid"));
    }

    emit sortChanged();
    applyFilterAndSort();
    emit stateChanged();
}

QString ProcessService::statusMessage() const
{
    return m_statusMessage;
}

bool ProcessService::actionBusy() const
{
    return m_actionBusy;
}

void ProcessService::refresh()
{
    scanProcesses();
    applyFilterAndSort();
    emit stateChanged();
}

qint64 ProcessService::readSystemTotalCpuTicks()
{
    QFile file(QStringLiteral("/proc/stat"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    QByteArray line = file.readLine();
    file.close();

    // cpu user nice system idle iowait irq softirq steal guest guest_nice
    const QList<QByteArray> parts = line.split(' ');
    qint64 total = 0;
    int count = 0;
    for (const QByteArray &part : parts) {
        if (part.isEmpty() || part == "cpu")
            continue;
        total += part.toLongLong();
        if (++count >= 8)
            break;
    }
    return total;
}

qint64 ProcessService::readSystemTotalMemory()
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith(QStringLiteral("MemTotal:"))) {
            const QStringList parts = line.simplified().split(QChar(32), Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                return parts.at(1).toLongLong() * 1024;
            }
        }
    }
    return 0;
}

QString ProcessService::formatMemory(qint64 bytes)
{
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
    return QStringLiteral("%1 GB").arg(QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

quint64 ProcessService::readProcessStartTime(int pid) const
{
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return 0;

    const QByteArray data = file.readAll();
    file.close();

    const int lastParen = data.lastIndexOf(')');
    if (lastParen < 0)
        return 0;

    const QByteArray afterParen = data.mid(lastParen + 1).trimmed();
    const QList<QByteArray> fields = afterParen.split(' ');
    // field 22 starttime -> index 22 - 3 + 2 = 21 in afterParen fields (field 3 is index 0)
    // field 3: state (0)
    // field 4: ppid (1)
    // ...
    // field 22: starttime (19)
    if (fields.size() > 19)
        return fields.at(19).toULongLong();

    return 0;
}

uid_t ProcessService::readProcessUid(int pid) const
{
    if (pid <= 0)
        return 65534;

    QFileInfo info(QStringLiteral("/proc/%1").arg(pid));
    const uint uid = info.ownerId();
    return uid == uint(-1) ? 65534 : static_cast<uid_t>(uid);
}

QString ProcessService::readProcessName(int pid) const
{
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    const QByteArray data = file.readAll();
    file.close();

    const int firstParen = data.indexOf('(');
    const int lastParen = data.lastIndexOf(')');
    if (firstParen >= 0 && lastParen > firstParen) {
        return QString::fromUtf8(data.mid(firstParen + 1, lastParen - firstParen - 1));
    }
    return QString();
}

bool ProcessService::isCritical(int pid) const
{
    if (pid <= 2)
        return true;

    if (pid == static_cast<int>(::getpid()))
        return true;

    const QString name = readProcessName(pid).toLower();
    if (s_criticalProcessNames.contains(name))
        return true;

    // Check if it's a kernel thread (ppid == 2)
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray data = file.readAll();
        file.close();
        const int lastParen = data.lastIndexOf(')');
        if (lastParen >= 0) {
            const QByteArray afterParen = data.mid(lastParen + 1).trimmed();
            const QList<QByteArray> fields = afterParen.split(' ');
            if (fields.size() > 1) {
                int ppid = fields.at(1).toInt();
                if (ppid == 2)
                    return true;
            }
        }
    }

    return false;
}

bool ProcessService::canTerminate(int pid) const
{
    if (isCritical(pid))
        return false;

    const uid_t currentUid = ::getuid();
    if (currentUid == 0)
        return true;

    const uid_t procUid = readProcessUid(pid);
    return (procUid == currentUid);
}

quint64 ProcessService::processStartTime(int pid) const
{
    return readProcessStartTime(pid);
}

bool ProcessService::verifyPidIdentity(int pid, quint64 expectedStartTime, QString *errorMessage) const
{
    if (pid <= 0) {
        if (errorMessage) *errorMessage = QStringLiteral("Geçersiz PID.");
        return false;
    }

    if (!QFile::exists(QStringLiteral("/proc/%1").arg(pid))) {
        if (errorMessage) *errorMessage = QStringLiteral("İşlem artık mevcut değil (PID: %1).").arg(pid);
        return false;
    }

    if (expectedStartTime == 0) {
        if (errorMessage) *errorMessage = QStringLiteral("Süreç kimliği doğrulanamadı: başlangıç zamanı eksik.");
        return false;
    }

    {
        quint64 currentStartTime = readProcessStartTime(pid);
        if (currentStartTime == 0 || currentStartTime != expectedStartTime) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Güvenlik Uyarısı: Süreç başlangıç zamanı uyuşmuyor; PID yeniden kullanılmış olabilir (PID: %1).").arg(pid);
            }
            return false;
        }
    }

    return true;
}

void ProcessService::scanProcesses()
{
    QDir procDir(QStringLiteral("/proc"));
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    const qint64 currentSystemCpuTicks = readSystemTotalCpuTicks();
    const qint64 deltaSystemCpuTicks = (m_prevSystemTotalCpuTicks > 0 && currentSystemCpuTicks > m_prevSystemTotalCpuTicks)
        ? (currentSystemCpuTicks - m_prevSystemTotalCpuTicks) : 0;

    const uid_t currentUid = ::getuid();
    if (m_systemTotalMemory <= 0)
        m_systemTotalMemory = readSystemTotalMemory();

    QList<RawProcessInfo> rawList;
    rawList.reserve(entries.size());

    qint64 totalUsedRss = 0;
    int userCount = 0;
    int bgCount = 0;
    QHash<int, qint64> currentProcCpuTicks;

    for (const QString &entry : entries) {
        bool ok = false;
        int pid = entry.toInt(&ok);
        if (!ok || pid <= 0)
            continue;

        // Read /proc/[pid]/stat
        QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!statFile.open(QIODevice::ReadOnly))
            continue;

        const QByteArray statData = statFile.readAll();
        statFile.close();

        const int firstParen = statData.indexOf('(');
        const int lastParen = statData.lastIndexOf(')');
        if (firstParen < 0 || lastParen <= firstParen)
            continue;

        RawProcessInfo info;
        info.pid = pid;
        info.name = QString::fromUtf8(statData.mid(firstParen + 1, lastParen - firstParen - 1));

        const QByteArray afterParen = statData.mid(lastParen + 1).trimmed();
        const QList<QByteArray> fields = afterParen.split(' ');
        if (fields.size() < 22)
            continue;

        info.state = QString::fromLatin1(fields.at(0));
        info.ppid = fields.at(1).toInt();
        info.utime = fields.at(11).toLongLong();
        info.stime = fields.at(12).toLongLong();
        info.numThreads = fields.at(17).toInt();
        info.startTime = fields.at(19).toULongLong();

        // Read /proc/[pid]/cmdline
        QFile cmdlineFile(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (cmdlineFile.open(QIODevice::ReadOnly)) {
            QByteArray cmdData = cmdlineFile.readAll();
            cmdlineFile.close();
            if (!cmdData.isEmpty()) {
                cmdData.replace('\0', ' ');
                info.cmdline = QString::fromUtf8(cmdData).trimmed();
            }
        }
        if (info.cmdline.isEmpty()) {
            info.cmdline = QStringLiteral("[%1]").arg(info.name);
        }

        // Read /proc/[pid]/status
        QFile statusFile(QStringLiteral("/proc/%1/status").arg(pid));
        if (statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&statusFile);
            while (!in.atEnd()) {
                const QString line = in.readLine();
                if (line.startsWith(QStringLiteral("Uid:"))) {
                    const QStringList parts = line.simplified().split(QChar(32), Qt::SkipEmptyParts);
                    if (parts.size() >= 2)
                        info.uid = static_cast<uid_t>(parts.at(1).toUInt());
                } else if (line.startsWith(QStringLiteral("VmRSS:"))) {
                    const QStringList parts = line.simplified().split(QChar(32), Qt::SkipEmptyParts);
                    if (parts.size() >= 2)
                        info.vmRssKb = parts.at(1).toLongLong();
                }
            }
            statusFile.close();
        }

        info.uid = readProcessUid(pid);
        info.user = usernameFromUid(info.uid);

        // CPU calculation
        qint64 totalProcTicks = info.utime + info.stime;
        currentProcCpuTicks.insert(pid, totalProcTicks);

        if (deltaSystemCpuTicks > 0 && m_prevProcCpuTicks.contains(pid)) {
            qint64 deltaProcTicks = totalProcTicks - m_prevProcCpuTicks.value(pid);
            if (deltaProcTicks > 0) {
                info.cpuPercent = (100.0 * static_cast<double>(deltaProcTicks)) / static_cast<double>(deltaSystemCpuTicks);
                if (info.cpuPercent > 100.0)
                    info.cpuPercent = 100.0;
            }
        }

        // Memory calculation
        qint64 memBytes = info.vmRssKb * 1024;
        totalUsedRss += memBytes;
        if (m_systemTotalMemory > 0)
            info.memoryPercent = (100.0 * static_cast<double>(memBytes)) / static_cast<double>(m_systemTotalMemory);
        info.memoryFormatted = formatMemory(memBytes);

        // Protection and role
        info.isCritical = (info.pid <= 2 || info.ppid == 2 || info.pid == static_cast<int>(::getpid())
                           || s_criticalProcessNames.contains(info.name.toLower()));

        if (info.isCritical) {
            info.canTerminate = false;
            info.protectionReason = QStringLiteral("Kritik Sistem Süreci (Korumalı)");
        } else if (currentUid != 0 && info.uid != currentUid) {
            info.canTerminate = false;
            info.protectionReason = QStringLiteral("Yetkisiz (Farklı kullanıcı)");
        } else {
            info.canTerminate = true;
            info.protectionReason.clear();
        }

        // Background vs GUI application detection
        // Background: kernel thread (ppid 2 or empty original cmdline), system daemon, or non-GUI
        bool hasGuiHint = info.cmdline.contains(QStringLiteral("plasma")) ||
                          info.cmdline.contains(QStringLiteral("kwin")) ||
                          info.cmdline.contains(QStringLiteral("firefox")) ||
                          info.cmdline.contains(QStringLiteral("chrome")) ||
                          info.cmdline.contains(QStringLiteral("dolphin")) ||
                          info.cmdline.contains(QStringLiteral("konsole")) ||
                          info.cmdline.contains(QStringLiteral("control_center"));

        info.isBackground = (info.ppid == 2 || info.uid == 0 || !hasGuiHint);

        if (info.uid == currentUid)
            userCount++;
        if (info.isBackground)
            bgCount++;

        rawList.append(info);
    }

    m_prevSystemTotalCpuTicks = currentSystemCpuTicks;
    m_prevProcCpuTicks = currentProcCpuTicks;

    buildProcessHierarchy(rawList);

    m_scannedProcesses = rawList;
    m_totalProcesses = rawList.size();
    m_userProcesses = userCount;
    m_backgroundProcesses = bgCount;
    m_usedMemoryBytes = totalUsedRss;

    double totalCpu = 0.0;
    for (const RawProcessInfo &info : rawList) {
        totalCpu += info.cpuPercent;
    }
    m_totalCpuPercent = std::min(100.0, totalCpu);
}

void ProcessService::buildProcessHierarchy(QList<RawProcessInfo> &rawList)
{
    QHash<int, int> pidToIndex;
    for (int i = 0; i < rawList.size(); ++i) {
        pidToIndex.insert(rawList.at(i).pid, i);
    }

    for (const RawProcessInfo &info : rawList) {
        if (info.ppid > 0 && pidToIndex.contains(info.ppid)) {
            int parentIdx = pidToIndex.value(info.ppid);
            rawList[parentIdx].childPids.append(info.pid);
        }
    }
}

void ProcessService::applyFilterAndSort()
{
    const QString query = m_searchQuery.trimmed().toLower();
    const QString filter = m_filterType;

    QList<RawProcessInfo> filtered;
    filtered.reserve(m_scannedProcesses.size());

    for (const RawProcessInfo &info : m_scannedProcesses) {
        if (filter == QStringLiteral("apps") && info.isBackground)
            continue;
        if (filter == QStringLiteral("background") && !info.isBackground)
            continue;

        if (!query.isEmpty()) {
            bool matches = info.name.toLower().contains(query) ||
                           QString::number(info.pid).contains(query) ||
                           info.cmdline.toLower().contains(query) ||
                           info.user.toLower().contains(query);
            if (!matches)
                continue;
        }

        filtered.append(info);
    }

    // Sort
    const QString sort = m_sortField;
    const bool asc = m_sortAscending;

    std::sort(filtered.begin(), filtered.end(), [sort, asc](const RawProcessInfo &a, const RawProcessInfo &b) {
        if (sort == QStringLiteral("name")) {
            int cmp = QString::compare(a.name, b.name, Qt::CaseInsensitive);
            return asc ? cmp < 0 : cmp > 0;
        } else if (sort == QStringLiteral("pid")) {
            return asc ? a.pid < b.pid : a.pid > b.pid;
        } else if (sort == QStringLiteral("memory")) {
            return asc ? a.vmRssKb < b.vmRssKb : a.vmRssKb > b.vmRssKb;
        } else { // default "cpu"
            if (a.cpuPercent == b.cpuPercent)
                return a.pid < b.pid;
            return asc ? a.cpuPercent < b.cpuPercent : a.cpuPercent > b.cpuPercent;
        }
    });

    QVariantList flatList;
    flatList.reserve(filtered.size());

    for (const RawProcessInfo &info : filtered) {
        QVariantMap map;
        map.insert(QStringLiteral("pid"), info.pid);
        map.insert(QStringLiteral("ppid"), info.ppid);
        map.insert(QStringLiteral("name"), info.name);
        map.insert(QStringLiteral("state"), formatStateName(info.state));
        map.insert(QStringLiteral("rawState"), info.state);
        map.insert(QStringLiteral("cmdline"), info.cmdline);
        map.insert(QStringLiteral("startTime"), static_cast<qulonglong>(info.startTime));
        map.insert(QStringLiteral("cpuPercent"), QString::number(info.cpuPercent, 'f', 1));
        map.insert(QStringLiteral("cpuRaw"), info.cpuPercent);
        map.insert(QStringLiteral("memoryFormatted"), info.memoryFormatted);
        map.insert(QStringLiteral("memoryPercent"), QString::number(info.memoryPercent, 'f', 1));
        map.insert(QStringLiteral("memoryRaw"), info.vmRssKb);
        map.insert(QStringLiteral("uid"), static_cast<int>(info.uid));
        map.insert(QStringLiteral("user"), info.user);
        map.insert(QStringLiteral("numThreads"), info.numThreads);
        map.insert(QStringLiteral("isBackground"), info.isBackground);
        map.insert(QStringLiteral("isCritical"), info.isCritical);
        map.insert(QStringLiteral("canTerminate"), info.canTerminate);
        map.insert(QStringLiteral("protectionReason"), info.protectionReason);

        QVariantList childList;
        for (int cpid : info.childPids)
            childList.append(cpid);
        map.insert(QStringLiteral("childPids"), childList);
        map.insert(QStringLiteral("childCount"), info.childPids.size());

        flatList.append(map);
    }
    m_processes = flatList;

    // Build hierarchical tree list
    QHash<int, QVariantMap> mapByPid;
    for (const QVariant &item : flatList) {
        QVariantMap m = item.toMap();
        mapByPid.insert(m.value(QStringLiteral("pid")).toInt(), m);
    }

    QVariantList treeList;
    std::function<void(int, int)> addTreeNode = [&](int pid, int depth) {
        if (!mapByPid.contains(pid))
            return;
        QVariantMap node = mapByPid.value(pid);
        node.insert(QStringLiteral("depth"), depth);
        treeList.append(node);

        const QVariantList children = node.value(QStringLiteral("childPids")).toList();
        for (const QVariant &c : children) {
            int childPid = c.toInt();
            if (mapByPid.contains(childPid)) {
                addTreeNode(childPid, depth + 1);
            }
        }
    };

    for (const QVariant &item : flatList) {
        QVariantMap m = item.toMap();
        int ppid = m.value(QStringLiteral("ppid")).toInt();
        // Root nodes: ppid <= 1 or parent not in map
        if (ppid <= 1 || !mapByPid.contains(ppid)) {
            addTreeNode(m.value(QStringLiteral("pid")).toInt(), 0);
        }
    }
    m_treeProcesses = treeList;
}

QVariantMap ProcessService::getProcessDetails(int pid) const
{
    for (const RawProcessInfo &info : m_scannedProcesses) {
        if (info.pid == pid) {
            QVariantMap map;
            map.insert(QStringLiteral("pid"), info.pid);
            map.insert(QStringLiteral("ppid"), info.ppid);
            map.insert(QStringLiteral("parentName"), readProcessName(info.ppid));
            map.insert(QStringLiteral("name"), info.name);
            map.insert(QStringLiteral("state"), formatStateName(info.state));
            map.insert(QStringLiteral("cmdline"), info.cmdline);
            map.insert(QStringLiteral("startTime"), static_cast<qulonglong>(info.startTime));
            map.insert(QStringLiteral("cpuPercent"), QString::number(info.cpuPercent, 'f', 1));
            map.insert(QStringLiteral("memoryFormatted"), info.memoryFormatted);
            map.insert(QStringLiteral("memoryPercent"), QString::number(info.memoryPercent, 'f', 1));
            map.insert(QStringLiteral("uid"), static_cast<int>(info.uid));
            map.insert(QStringLiteral("user"), info.user);
            map.insert(QStringLiteral("numThreads"), info.numThreads);
            map.insert(QStringLiteral("isBackground"), info.isBackground);
            map.insert(QStringLiteral("isCritical"), info.isCritical);
            map.insert(QStringLiteral("canTerminate"), info.canTerminate);
            map.insert(QStringLiteral("protectionReason"), info.protectionReason);

            QVariantList children;
            for (int cpid : info.childPids) {
                QVariantMap childMap;
                childMap.insert(QStringLiteral("pid"), cpid);
                childMap.insert(QStringLiteral("name"), readProcessName(cpid));
                children.append(childMap);
            }
            map.insert(QStringLiteral("children"), children);
            return map;
        }
    }
    return QVariantMap();
}

bool ProcessService::terminateProcess(int pid, quint64 expectedStartTime)
{
    m_statusMessage.clear();

    // 1. Backend-level verification: critical process protection
    if (isCritical(pid)) {
        m_statusMessage = QStringLiteral("Kritik sistem süreçleri sonlandırılamaz (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    // 2. Backend-level verification: authorization check
    if (!canTerminate(pid)) {
        m_statusMessage = QStringLiteral("Yetkisiz işlem: Bu süreç başka bir kullanıcıya ait (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    // 3. Backend-level verification: PID reuse start time check
    QString idError;
    if (!verifyPidIdentity(pid, expectedStartTime, &idError)) {
        m_statusMessage = idError;
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    const quint64 actualStartTime = (expectedStartTime > 0) ? expectedStartTime : readProcessStartTime(pid);
    const QString procName = readProcessName(pid);

    m_actionBusy = true;
    emit actionBusyChanged();

    // 4. Send SIGTERM
    if (::kill(pid, SIGTERM) != 0) {
        m_actionBusy = false;
        emit actionBusyChanged();
        m_statusMessage = QStringLiteral("SIGTERM gönderilemedi (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    m_statusMessage = QStringLiteral("SIGTERM sinyali gönderildi (PID: %1)...").arg(pid);
    emit statusMessageChanged();

    // 5. Asynchronously verify termination after 1.5s grace period
    QTimer::singleShot(1500, this, [this, pid, procName, actualStartTime]() {
        m_actionBusy = false;
        emit actionBusyChanged();

        // Check if process is still alive and identical
        if (::kill(pid, 0) == 0) {
            quint64 checkStartTime = readProcessStartTime(pid);
            if (checkStartTime != 0 && checkStartTime == actualStartTime) {
                // Process is still running and did not respond to SIGTERM
                m_statusMessage = QStringLiteral("İşlem (PID: %1, %2) SIGTERM sinyaline yanıt vermedi.").arg(pid).arg(procName);
                emit statusMessageChanged();
                emit processRequiresForceKill(pid, procName, actualStartTime);
                return;
            }
        }

        // Process terminated or changed
        m_statusMessage = QStringLiteral("İşlem başarıyla sonlandırıldı (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(true, m_statusMessage);
        refresh();
    });

    return true;
}

bool ProcessService::forceKillProcess(int pid, quint64 expectedStartTime, bool userConfirmed)
{
    m_statusMessage.clear();

    // 1. Enforce explicit user confirmation requirement on backend
    if (!userConfirmed) {
        m_statusMessage = QStringLiteral("SIGKILL işlemi kullanıcı tarafından açıkça onaylanmalıdır.");
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    // 2. Backend-level verification: critical process protection
    if (isCritical(pid)) {
        m_statusMessage = QStringLiteral("Kritik sistem süreçleri zorla sonlandırılamaz (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    // 3. Backend-level verification: authorization check
    if (!canTerminate(pid)) {
        m_statusMessage = QStringLiteral("Yetkisiz işlem: Bu süreç başka bir kullanıcıya ait (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    // 4. Backend-level verification: PID reuse start time check
    QString idError;
    if (!verifyPidIdentity(pid, expectedStartTime, &idError)) {
        m_statusMessage = idError;
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    // 5. Send SIGKILL
    if (::kill(pid, SIGKILL) != 0) {
        m_statusMessage = QStringLiteral("SIGKILL gönderilemedi (PID: %1).").arg(pid);
        emit statusMessageChanged();
        emit actionFinished(false, m_statusMessage);
        return false;
    }

    m_statusMessage = QStringLiteral("İşlem zorla sonlandırıldı (SIGKILL, PID: %1).").arg(pid);
    emit statusMessageChanged();
    emit actionFinished(true, m_statusMessage);
    refresh();
    return true;
}
