#include "RecoveryService.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

QVariant dbusValueToVariant(const QVariant &value)
{
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return dbusValueToVariant(value.value<QDBusVariant>().variant());

    if (value.userType() == QMetaType::QVariantMap) {
        const QVariantMap source = value.toMap();
        QVariantMap result;
        for (auto it = source.cbegin(); it != source.cend(); ++it)
            result.insert(it.key(), dbusValueToVariant(it.value()));
        return result;
    }

    if (value.userType() == QMetaType::QVariantList) {
        const QVariantList source = value.toList();
        QVariantList result;
        result.reserve(source.size());
        for (const QVariant &item : source)
            result.append(dbusValueToVariant(item));
        return result;
    }

    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;

    const auto argument = value.value<QDBusArgument>();
    if (argument.currentType() == QDBusArgument::StructureType) {
        bool success = false;
        QString code;
        QString message;
        QString operationId;
        QVariantMap dataMap;
        argument.beginStructure();
        argument >> success >> code >> message >> operationId;
        if (!argument.atEnd() && argument.currentType() == QDBusArgument::MapType) {
            argument.beginMap();
            while (!argument.atEnd()) {
                argument.beginMapEntry();
                QString key;
                QDBusVariant wrapped;
                argument >> key >> wrapped;
                dataMap.insert(key, dbusValueToVariant(wrapped.variant()));
                argument.endMapEntry();
            }
            argument.endMap();
        }
        argument.endStructure();
        return QVariantMap{
            {QStringLiteral("success"), success},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message},
            {QStringLiteral("operation_id"), operationId},
            {QStringLiteral("data"), dataMap}
        };
    }
    if (argument.currentType() == QDBusArgument::ArrayType) {
        QVariantList list;
        argument.beginArray();
        while (!argument.atEnd()) {
            if (argument.currentType() == QDBusArgument::StructureType) {
                uint id = 0;
                QString name;
                QString createdAt;
                QString description;
                bool isCurrent = false;
                argument.beginStructure();
                argument >> id >> name >> createdAt >> description >> isCurrent;
                argument.endStructure();
                list.append(QVariantMap{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("created_at"), createdAt},
                    {QStringLiteral("description"), description},
                    {QStringLiteral("is_current"), isCurrent}
                });
            } else if (argument.currentType() == QDBusArgument::MapType) {
                const bool isStringMap = argument.currentSignature() == QStringLiteral("a{ss}");
                QVariantMap innerMap;
                argument.beginMap();
                while (!argument.atEnd()) {
                    argument.beginMapEntry();
                    QString k;
                    if (isStringMap) {
                        QString s;
                        argument >> k >> s;
                        innerMap.insert(k, s);
                    } else {
                        QDBusVariant w;
                        argument >> k >> w;
                        innerMap.insert(k, dbusValueToVariant(w.variant()));
                    }
                    argument.endMapEntry();
                }
                argument.endMap();
                list.append(innerMap);
            } else {
                QDBusVariant item;
                argument >> item;
                list.append(dbusValueToVariant(item.variant()));
            }
        }
        argument.endArray();
        return list;
    }
    if (argument.currentType() != QDBusArgument::MapType)
        return {};

    const bool isStringMap = argument.currentSignature() == QStringLiteral("a{ss}");
    QVariantMap map;
    argument.beginMap();
    while (!argument.atEnd()) {
        argument.beginMapEntry();
        QString key;
        if (isStringMap) {
            QString strVal;
            argument >> key >> strVal;
            map.insert(key, strVal);
        } else {
            QDBusVariant wrappedValue;
            argument >> key >> wrappedValue;
            map.insert(key, dbusValueToVariant(wrappedValue.variant()));
        }
        argument.endMapEntry();
    }
    argument.endMap();
    return map;
}

QString normalizeFsName(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("Btrfs");
    return trimmed.isEmpty() ? QStringLiteral("Unknown") : trimmed;
}

QString extractSubvolOption(const QString &options, const QString &rootField)
{
    const QStringList parts = options.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &rawPart : parts) {
        const QString part = rawPart.trimmed();
        if (part.startsWith(QStringLiteral("subvol="))) {
            const QString subvol = part.mid(7).trimmed();
            if (!subvol.isEmpty())
                return subvol;
        }
    }
    if (!rootField.isEmpty() && rootField != QStringLiteral("/"))
        return rootField;
    return QStringLiteral("/");
}

bool parseKeyValueFile(const QString &content, QMap<QString, QString> &out, QString &errorReason)
{
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            errorReason = QStringLiteral("Malformed line %1 in Snapper configuration.").arg(i + 1);
            return false;
        }
        const QString key = line.left(eq).trimmed();
        static const QRegularExpression validKey(QStringLiteral("^[A-Za-z0-9_]+$"));
        if (!validKey.match(key).hasMatch()) {
            errorReason = QStringLiteral("Invalid configuration key '%1'.").arg(key);
            return false;
        }
        QString val = line.mid(eq + 1).trimmed();
        if ((val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"')) && val.size() >= 2)
            || (val.startsWith(QLatin1Char('\'')) && val.endsWith(QLatin1Char('\'')) && val.size() >= 2)) {
            val = val.mid(1, val.size() - 2);
        } else if (val.startsWith(QLatin1Char('"')) || val.startsWith(QLatin1Char('\''))) {
            errorReason = QStringLiteral("Unterminated quote on line %1.").arg(i + 1);
            return false;
        }
        out.insert(key, val);
    }
    return true;
}

bool isValidDetectionStatus(const QString &status)
{
    return status == QStringLiteral("installed_and_configured")
        || status == QStringLiteral("installed_not_configured")
        || status == QStringLiteral("not_installed")
        || status == QStringLiteral("invalid_configuration")
        || status == QStringLiteral("permission_denied")
        || status == QStringLiteral("backend_unavailable");
}

} // namespace

RecoveryService::RecoveryService(QObject *parent)
    : QObject(parent)
{
    processTimeoutTimer.setSingleShot(true);
    processTimeoutTimer.setInterval(4000);
    connect(&processTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (process.state() != QProcess::NotRunning) {
            process.setProperty("timedOut", true);
            process.kill();
        }
    });

    operationPollTimer.setInterval(50);
    connect(&operationPollTimer, &QTimer::timeout, this, &RecoveryService::pollOperationState);

    connect(&process, &QProcess::finished, this, &RecoveryService::readOutput);
    connect(&process, &QProcess::errorOccurred, this, &RecoveryService::readError);

    connect(&actionProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromLocal8Bit(actionProcess.readAllStandardOutput()).trimmed();
        const QString error = QString::fromLocal8Bit(actionProcess.readAllStandardError()).trimmed();

        m_actionBusy = false;

        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            if (m_actionOperation == QStringLiteral("delete"))
                m_actionStatus = QStringLiteral("Recovery point deleted successfully.");
            else
                m_actionStatus = QStringLiteral("Recovery action completed successfully.");
        } else if (!error.isEmpty()) {
            m_actionStatus = error;
        } else if (!output.isEmpty()) {
            m_actionStatus = output;
        } else {
            if (m_actionOperation == QStringLiteral("delete"))
                m_actionStatus = QStringLiteral("Recovery point deletion failed.");
            else
                m_actionStatus = QStringLiteral("Recovery action failed.");
        }

        emit stateChanged();

        if (exitStatus == QProcess::NormalExit && exitCode == 0)
            refresh();
    });

    connect(&actionProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            m_actionStatus = QStringLiteral("Could not start recovery action.");
        else
            m_actionStatus = QStringLiteral("Recovery action process failed.");

        m_actionBusy = false;
        emit stateChanged();
    });

    QDBusConnection::systemBus().connect(
        QStringLiteral("org.zethropol.Recovery"),
        QStringLiteral("/org/zethropol/Recovery"),
        QStringLiteral("org.zethropol.Recovery.v1"),
        QStringLiteral("StateChanged"),
        this,
        SLOT(handleDbusStateChanged(QVariantMap)));

    check();
}

bool RecoveryService::checking() const { return m_checking; }
bool RecoveryService::available() const { return m_available; }
bool RecoveryService::permissionRequired() const { return m_permissionRequired; }
QString RecoveryService::detectionStatus() const { return m_detectionStatus; }
QString RecoveryService::statusTitle() const { return m_statusTitle; }
QString RecoveryService::statusMessage() const { return m_statusMessage; }
QString RecoveryService::filesystem() const { return m_filesystem; }
QString RecoveryService::rootSubvolume() const { return m_rootSubvolume; }
QString RecoveryService::snapshotTool() const { return m_snapshotTool; }
QString RecoveryService::configName() const { return m_configName; }
QStringList RecoveryService::availableConfigs() const { return m_availableConfigs; }
bool RecoveryService::snapperInstalled() const { return m_snapperInstalled; }
bool RecoveryService::configured() const { return m_configured; }
bool RecoveryService::snapshotsDirExists() const { return m_snapshotsDirExists; }
bool RecoveryService::snapshotsDirAccessible() const { return m_snapshotsDirAccessible; }
int RecoveryService::snapshotCount() const { return m_snapshotCount; }
int RecoveryService::currentSnapshotId() const { return m_currentSnapshotId; }
QVariantList RecoveryService::snapshots() const { return m_snapshots; }
QVariantList RecoveryService::recoveryEvents() const { return m_recoveryEvents; }
QVariantList RecoveryService::recoveryPoints() const { return m_recoveryPoints; }
bool RecoveryService::actionBusy() const { return m_actionBusy; }
QString RecoveryService::actionStatus() const { return m_actionStatus; }
QString RecoveryService::lastOperationId() const { return m_lastOperationId; }
QString RecoveryService::lastOperationCode() const { return m_lastOperationCode; }
int RecoveryService::lastCreatedSnapshotId() const { return m_lastCreatedSnapshotId; }
bool RecoveryService::confirmationPending() const { return m_confirmationPending; }
QString RecoveryService::pendingDescription() const { return m_pendingDescription; }
bool RecoveryService::preflightBusy() const { return m_preflightBusy; }
int RecoveryService::selectedRollbackPointId() const { return m_selectedRollbackPointId; }
QVariantMap RecoveryService::preflightResult() const { return m_preflightResult; }

void RecoveryService::setLocalFallbackEnabled(bool enabled)
{
    m_localFallbackEnabled = enabled;
}

void RecoveryService::setOperationTimeoutMs(int timeoutMs)
{
    m_operationTimeoutMs = qMax(200, timeoutMs);
}

void RecoveryService::setDetectionPaths(const QString &snapperBinary,
                                        const QString &sysconfFile,
                                        const QString &configsDir,
                                        const QString &mountinfoFile,
                                        const QString &fstabFile,
                                        const QString &snapshotsDir)
{
    m_snapperBinary = snapperBinary;
    m_sysconfFile = sysconfFile;
    m_configsDir = configsDir;
    m_mountinfoFile = mountinfoFile;
    m_fstabFile = fstabFile;
    m_snapshotsDir = snapshotsDir;
}

QPair<QString, QString> RecoveryService::detectRootFilesystem(const QString &mountinfoPath,
                                                              const QString &fstabPath)
{
    QString rootFs;
    QString rootSubvol = QStringLiteral("/");
    QString bindBtrfsSubvol;
    bool hasBindBtrfs = false;

    QFile mountFile(mountinfoPath);
    if (mountFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(mountFile.readAll());
        const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const int sep = line.indexOf(QStringLiteral(" - "));
            if (sep <= 0)
                continue;
            const QStringList left = line.left(sep).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            const QStringList right = line.mid(sep + 3).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (left.size() < 5 || right.isEmpty())
                continue;
            const QString rootField = left.at(3);
            const QString mountPoint = left.at(4);
            const QString fsType = right.at(0);
            const QString superOpts = right.size() > 2 ? right.at(2) : QString();

            if (mountPoint == QStringLiteral("/")) {
                rootFs = fsType;
                rootSubvol = extractSubvolOption(superOpts, rootField);
            } else if ((mountPoint == QStringLiteral("/etc")
                        || mountPoint == QStringLiteral("/usr")
                        || mountPoint == QStringLiteral("/var"))
                       && fsType.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) == 0) {
                hasBindBtrfs = true;
                bindBtrfsSubvol = extractSubvolOption(superOpts, rootField);
            }
        }
    }

    if (!rootFs.isEmpty()
        && rootFs != QStringLiteral("tmpfs")
        && rootFs != QStringLiteral("overlay")
        && rootFs != QStringLiteral("rootfs")) {
        return qMakePair(normalizeFsName(rootFs), rootSubvol);
    }

    QFile fstabFile(fstabPath);
    if (fstabFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(fstabFile.readAll());
        const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (fields.size() >= 4 && fields.at(1) == QStringLiteral("/")) {
                return qMakePair(normalizeFsName(fields.at(2)),
                                 extractSubvolOption(fields.at(3), QStringLiteral("/")));
            }
        }
    }

    if (hasBindBtrfs)
        return qMakePair(QStringLiteral("Btrfs"), bindBtrfsSubvol);

    if (!rootFs.isEmpty())
        return qMakePair(normalizeFsName(rootFs), rootSubvol);

    return qMakePair(QStringLiteral("Unknown"), QStringLiteral("/"));
}

void RecoveryService::setDisconnectedState(const QString &status,
                                           const QString &title,
                                           const QString &message,
                                           bool permissionReq)
{
    m_checking = false;
    m_available = false;
    m_permissionRequired = permissionReq;
    m_detectionStatus = status;
    m_statusTitle = title;
    m_statusMessage = message;
    m_snapshotCount = 0;
    m_currentSnapshotId = 0;
    m_snapshots.clear();
    m_recoveryEvents.clear();
    m_recoveryPoints.clear();
    emit stateChanged();
}

void RecoveryService::check()
{
    if (m_checking || process.state() != QProcess::NotRunning)
        return;

    const auto fsInfo = detectRootFilesystem(m_mountinfoFile, m_fstabFile);
    m_filesystem = fsInfo.first;
    m_rootSubvolume = fsInfo.second;

    m_checking = true;
    m_permissionRequired = false;
    emit stateChanged();

    const QDBusConnection connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        if (m_localFallbackEnabled) {
            startLocalDetection();
            return;
        }
        setDisconnectedState(
            QStringLiteral("backend_unavailable"),
            QStringLiteral("Recovery backend unavailable"),
            QStringLiteral("System D-Bus connection is unavailable."));
        return;
    }

    const QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Recovery"),
        QStringLiteral("/org/zethropol/Recovery"),
        QStringLiteral("org.zethropol.Recovery.v1"),
        QStringLiteral("GetState"));

    auto *watcher = new QDBusPendingCallWatcher(connection.asyncCall(msg, 4000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &RecoveryService::handleDbusReply);
}

void RecoveryService::refresh()
{
    check();
}

void RecoveryService::handleDbusStateChanged(const QVariantMap &state)
{
    const QVariantMap unwrapped = dbusValueToVariant(state).toMap();
    if (unwrapped.contains(QStringLiteral("data"))) {
        applyStateMap(dbusValueToVariant(unwrapped.value(QStringLiteral("data"))).toMap());
    } else {
        applyStateMap(unwrapped);
    }
}

void RecoveryService::handleDbusReply(QDBusPendingCallWatcher *watcher)
{
    if (!watcher)
        return;
    watcher->deleteLater();

    QDBusPendingReply<QVariantMap> reply = *watcher;
    if (reply.isError()) {
        if (m_localFallbackEnabled) {
            startLocalDetection();
            return;
        }
        const QDBusError err = reply.error();
        QString detail = err.message().trimmed();
        if (err.type() == QDBusError::ServiceUnknown
            || err.name() == QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner")
            || detail.contains(QStringLiteral("org.zethropol.Recovery"))
            || detail.contains(QStringLiteral("ServiceUnknown"))
            || detail.isEmpty()) {
            detail = QStringLiteral("Recovery D-Bus service (org.zethropol.Recovery) is not running or unreachable.");
        }
        setDisconnectedState(
            QStringLiteral("backend_unavailable"),
            QStringLiteral("Recovery backend unavailable"),
            detail);
        return;
    }

    const QVariantMap envelope = dbusValueToVariant(reply.value()).toMap();
    const QVariant dataVar = dbusValueToVariant(envelope.value(QStringLiteral("data")));
    if (!dataVar.isValid() || dataVar.userType() != QMetaType::QVariantMap) {
        setDisconnectedState(
            QStringLiteral("invalid_configuration"),
            QStringLiteral("Invalid recovery configuration"),
            QStringLiteral("Recovery D-Bus service returned an invalid state payload."));
        return;
    }

    applyStateMap(dataVar.toMap());
}

void RecoveryService::applyStateMap(const QVariantMap &data)
{
    const QString rawStatus = data.value(QStringLiteral("detectionStatus")).toString();
    if (!isValidDetectionStatus(rawStatus)) {
        setDisconnectedState(
            QStringLiteral("invalid_configuration"),
            QStringLiteral("Invalid recovery configuration"),
            QStringLiteral("Recovery D-Bus response contains an unknown detection status."));
        return;
    }

    m_checking = false;
    m_detectionStatus = rawStatus;
    m_available = data.value(QStringLiteral("available")).toBool()
        && rawStatus == QStringLiteral("installed_and_configured");
    m_permissionRequired = data.value(QStringLiteral("permissionRequired")).toBool()
        || rawStatus == QStringLiteral("permission_denied");
    m_snapperInstalled = data.value(QStringLiteral("snapperInstalled")).toBool();
    m_configured = data.value(QStringLiteral("configured")).toBool();

    const QString fs = data.value(QStringLiteral("filesystem")).toString().trimmed();
    if (!fs.isEmpty())
        m_filesystem = fs;
    const QString subvol = data.value(QStringLiteral("rootSubvolume")).toString().trimmed();
    if (!subvol.isEmpty())
        m_rootSubvolume = subvol;
    const QString tool = data.value(QStringLiteral("snapshotTool")).toString().trimmed();
    m_snapshotTool = tool.isEmpty() ? QStringLiteral("Snapper") : tool;
    m_configName = data.value(QStringLiteral("configName")).toString();

    QStringList configs;
    const QVariantList rawConfigs = data.value(QStringLiteral("availableConfigs")).toList();
    for (const QVariant &c : rawConfigs)
        configs.append(c.toString());
    m_availableConfigs = configs;

    m_snapshotsDirExists = data.value(QStringLiteral("snapshotsDirExists")).toBool();
    m_snapshotsDirAccessible = data.value(QStringLiteral("snapshotsDirAccessible")).toBool();

    QString title = data.value(QStringLiteral("statusTitle")).toString().trimmed();
    QString message = data.value(QStringLiteral("statusMessage")).toString().trimmed();

    if (m_available) {
        QVariantList rawSnapshots = dbusValueToVariant(data.value(QStringLiteral("snapshots"))).toList();
        QVariantList rawEvents = dbusValueToVariant(data.value(QStringLiteral("recoveryEvents"))).toList();
        QVariantList rawPoints = dbusValueToVariant(data.value(QStringLiteral("recoveryPoints"))).toList();

        QSet<int> seenSnapshotIds;
        QVariantList dedupSnapshots;
        for (const QVariant &v : rawSnapshots) {
            const QVariantMap item = v.toMap();
            if (!item.contains(QStringLiteral("id")))
                continue;
            bool ok = false;
            const int id = item.value(QStringLiteral("id")).toInt(&ok);
            if (!ok || id < 0 || seenSnapshotIds.contains(id))
                continue;
            seenSnapshotIds.insert(id);
            dedupSnapshots.append(item);
        }

        QSet<QString> seenEvents;
        QVariantList dedupEvents;
        for (const QVariant &v : rawEvents) {
            const QVariantMap ev = v.toMap();
            bool preOk = false;
            bool postOk = false;
            const int preId = ev.value(QStringLiteral("preId")).toInt(&preOk);
            const int postId = ev.value(QStringLiteral("postId")).toInt(&postOk);
            if (!preOk || !postOk || preId < 0 || postId < 0)
                continue;
            const QString key = QStringLiteral("%1:%2").arg(preId).arg(postId);
            if (seenEvents.contains(key))
                continue;
            seenEvents.insert(key);
            dedupEvents.append(ev);
        }

        QSet<int> seenPointIds;
        QVariantList dedupPoints;
        for (const QVariant &v : rawPoints) {
            const QVariantMap pt = v.toMap();
            if (!pt.contains(QStringLiteral("id")))
                continue;
            bool ok = false;
            const int id = pt.value(QStringLiteral("id")).toInt(&ok);
            if (!ok || id < 0 || seenPointIds.contains(id))
                continue;
            seenPointIds.insert(id);
            dedupPoints.append(pt);
        }

        m_snapshots = dedupSnapshots;
        m_recoveryEvents = dedupEvents;
        m_recoveryPoints = dedupPoints;
        m_snapshotCount = data.contains(QStringLiteral("snapshotCount"))
            ? data.value(QStringLiteral("snapshotCount")).toInt()
            : m_snapshots.size();
        m_currentSnapshotId = data.value(QStringLiteral("currentSnapshotId")).toInt();
        if (title.isEmpty())
            title = QStringLiteral("Recovery is available");
        if (message.isEmpty()) {
            message = QStringLiteral("%1 · %2 · %3 snapshots")
                          .arg(m_filesystem, m_snapshotTool)
                          .arg(m_snapshotCount);
        }
    } else {
        m_snapshots.clear();
        m_recoveryEvents.clear();
        m_recoveryPoints.clear();
        m_snapshotCount = 0;
        m_currentSnapshotId = 0;

        if (title.isEmpty()) {
            if (rawStatus == QStringLiteral("not_installed"))
                title = QStringLiteral("Snapper is not installed");
            else if (rawStatus == QStringLiteral("installed_not_configured"))
                title = QStringLiteral("Snapper is not configured");
            else if (rawStatus == QStringLiteral("invalid_configuration"))
                title = QStringLiteral("Invalid recovery configuration");
            else if (rawStatus == QStringLiteral("permission_denied"))
                title = QStringLiteral("Authorization required");
            else
                title = QStringLiteral("Recovery backend unavailable");
        }
        if (message.isEmpty()) {
            if (rawStatus == QStringLiteral("not_installed"))
                message = QStringLiteral("Snapper executable was not found on this system.");
            else if (rawStatus == QStringLiteral("installed_not_configured"))
                message = QStringLiteral("Snapper is installed, but no root configuration is defined.");
            else if (rawStatus == QStringLiteral("invalid_configuration"))
                message = QStringLiteral("Snapper configuration or Btrfs setup is invalid.");
            else if (rawStatus == QStringLiteral("permission_denied"))
                message = QStringLiteral("Administrator permission is required to read Snapper snapshots");
            else
                message = QStringLiteral("Recovery backend is currently unreachable.");
        }
    }

    m_statusTitle = title;
    m_statusMessage = message;
    emit stateChanged();
}

void RecoveryService::startLocalDetection()
{
    const auto fsInfo = detectRootFilesystem(m_mountinfoFile, m_fstabFile);
    m_filesystem = fsInfo.first;
    m_rootSubvolume = fsInfo.second;

    QFileInfo snapDirInfo(m_snapshotsDir);
    m_snapshotsDirExists = snapDirInfo.exists() && snapDirInfo.isDir();
    m_snapshotsDirAccessible = m_snapshotsDirExists && snapDirInfo.isReadable() && snapDirInfo.isExecutable();

    QString resolvedBin = m_snapperBinary;
    if (!resolvedBin.contains(QLatin1Char('/')))
        resolvedBin = QStandardPaths::findExecutable(resolvedBin);

    QFileInfo binInfo(resolvedBin);
    m_snapperInstalled = !resolvedBin.isEmpty() && binInfo.exists() && binInfo.isFile() && binInfo.isExecutable();
    m_snapshotTool = m_snapperInstalled ? QStringLiteral("Snapper") : QStringLiteral("Not installed");

    if (m_filesystem.compare(QStringLiteral("Btrfs"), Qt::CaseInsensitive) != 0) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("invalid_configuration"),
            QStringLiteral("Invalid recovery configuration"),
            QStringLiteral("Root filesystem is %1, but Snapper recovery requires a Btrfs root filesystem.")
                .arg(m_filesystem));
        return;
    }

    if (!m_snapperInstalled) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("not_installed"),
            QStringLiteral("Snapper is not installed"),
            QStringLiteral("Snapper executable was not found on this system."));
        return;
    }

    QStringList sysconfListed;
    bool hasSysconf = false;
    QFile sysconf(m_sysconfFile);
    if (sysconf.exists()) {
        if (!sysconf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (sysconf.error() == QFileDevice::PermissionsError) {
                setDisconnectedState(
                    QStringLiteral("permission_denied"),
                    QStringLiteral("Authorization required"),
                    QStringLiteral("Administrator permission is required to read Snapper configuration."),
                    true);
                return;
            }
            setDisconnectedState(
                QStringLiteral("invalid_configuration"),
                QStringLiteral("Invalid recovery configuration"),
                QStringLiteral("Failed to read Snapper system configuration file."));
            return;
        }
        QMap<QString, QString> kv;
        QString parseErr;
        if (!parseKeyValueFile(QString::fromUtf8(sysconf.readAll()), kv, parseErr)) {
            setDisconnectedState(
                QStringLiteral("invalid_configuration"),
                QStringLiteral("Invalid recovery configuration"),
                parseErr);
            return;
        }
        if (kv.contains(QStringLiteral("SNAPPER_CONFIGS"))) {
            hasSysconf = true;
            sysconfListed = kv.value(QStringLiteral("SNAPPER_CONFIGS"))
                                .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        }
    }

    QFileInfo cfgDirInfo(m_configsDir);
    if (!cfgDirInfo.exists()) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("installed_not_configured"),
            QStringLiteral("Snapper is not configured"),
            QStringLiteral("Snapper is installed, but /etc/snapper/configs does not exist."));
        return;
    }
    if (!cfgDirInfo.isReadable() || !cfgDirInfo.isExecutable()) {
        m_configured = true;
        setDisconnectedState(
            QStringLiteral("permission_denied"),
            QStringLiteral("Authorization required"),
            QStringLiteral("Administrator permission is required to access /etc/snapper/configs."),
            true);
        return;
    }

    QDir cfgDir(m_configsDir);
    const QStringList entries = cfgDir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    m_availableConfigs = entries;

    if (hasSysconf && sysconfListed.isEmpty() && entries.isEmpty()) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("installed_not_configured"),
            QStringLiteral("Snapper is not configured"),
            QStringLiteral("Snapper is installed, but no configuration is defined."));
        return;
    }
    if (hasSysconf && !sysconfListed.isEmpty() && entries.isEmpty()) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("invalid_configuration"),
            QStringLiteral("Invalid recovery configuration"),
            QStringLiteral("SNAPPER_CONFIGS references a configuration that is missing from /etc/snapper/configs."));
        return;
    }
    if (entries.isEmpty()) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("installed_not_configured"),
            QStringLiteral("Snapper is not configured"),
            QStringLiteral("Snapper is installed, but no configuration was found in /etc/snapper/configs."));
        return;
    }

    QStringList ordered = entries;
    if (ordered.contains(QStringLiteral("root"))) {
        ordered.removeAll(QStringLiteral("root"));
        ordered.prepend(QStringLiteral("root"));
    }

    QString selectedConfig;
    bool sawPermDenied = false;
    QString invalidReason;

    for (const QString &name : ordered) {
        if (hasSysconf && !sysconfListed.isEmpty() && !sysconfListed.contains(name))
            continue;
        QFile cfgFile(cfgDir.filePath(name));
        if (!cfgFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (cfgFile.error() == QFileDevice::PermissionsError)
                sawPermDenied = true;
            else
                invalidReason = QStringLiteral("Unable to read Snapper configuration '%1'.").arg(name);
            continue;
        }
        QMap<QString, QString> kv;
        QString parseErr;
        if (!parseKeyValueFile(QString::fromUtf8(cfgFile.readAll()), kv, parseErr)) {
            invalidReason = parseErr;
            break;
        }
        const QString subvol = kv.value(QStringLiteral("SUBVOLUME"));
        const QString fstype = kv.value(QStringLiteral("FSTYPE"), QStringLiteral("btrfs"));
        if (name == QStringLiteral("root")) {
            if (subvol != QStringLiteral("/")) {
                invalidReason = QStringLiteral("Snapper 'root' configuration has invalid SUBVOLUME='%1'.").arg(subvol);
                break;
            }
            if (fstype.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) != 0) {
                invalidReason = QStringLiteral("Snapper 'root' configuration has unsupported FSTYPE='%1'.").arg(fstype);
                break;
            }
            selectedConfig = name;
            break;
        } else if (subvol == QStringLiteral("/")) {
            if (fstype.compare(QStringLiteral("btrfs"), Qt::CaseInsensitive) != 0) {
                invalidReason = QStringLiteral("Snapper '%1' configuration has unsupported FSTYPE='%2'.").arg(name, fstype);
                break;
            }
            selectedConfig = name;
            break;
        }
    }

    if (!invalidReason.isEmpty()) {
        m_configured = true;
        setDisconnectedState(
            QStringLiteral("invalid_configuration"),
            QStringLiteral("Invalid recovery configuration"),
            invalidReason);
        return;
    }

    if (selectedConfig.isEmpty() && sawPermDenied) {
        m_configured = true;
        setDisconnectedState(
            QStringLiteral("permission_denied"),
            QStringLiteral("Authorization required"),
            QStringLiteral("Administrator permission is required to read Snapper configuration files."),
            true);
        return;
    }

    if (selectedConfig.isEmpty()) {
        m_configured = false;
        setDisconnectedState(
            QStringLiteral("installed_not_configured"),
            QStringLiteral("Snapper is not configured"),
            QStringLiteral("Snapper is installed, but no configuration targets the root ('/') subvolume."));
        return;
    }

    m_configured = true;
    m_configName = selectedConfig;
    process.setProperty("timedOut", false);
    process.setProgram(resolvedBin);
    process.setArguments({QStringLiteral("--jsonout"), QStringLiteral("-c"), selectedConfig, QStringLiteral("list")});
    processTimeoutTimer.start();
    process.start();
}

bool RecoveryService::parseSnapperJsonOutput(const QString &output,
                                             const QString &configName,
                                             QString &errorReason)
{
    QJsonParseError jsonErr{};
    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8(), &jsonErr);
    if (jsonErr.error != QJsonParseError::NoError || !document.isObject()) {
        errorReason = QStringLiteral("Snapper output is not valid JSON.");
        return false;
    }

    const QJsonObject root = document.object();
    const QString key = root.contains(configName) ? configName : QStringLiteral("root");
    if (!root.contains(key) || !root.value(key).isArray()) {
        errorReason = QStringLiteral("Snapper JSON output does not contain snapshot array for '%1'.").arg(configName);
        return false;
    }

    const QJsonArray entries = root.value(key).toArray();
    QVariantList snapshots;
    int currentId = 0;

    for (const QJsonValue &value : entries) {
        if (!value.isObject()) {
            errorReason = QStringLiteral("Snapshot entry in JSON array is not an object.");
            return false;
        }
        const QJsonObject snapshot = value.toObject();
        if (!snapshot.contains(QStringLiteral("number")) || !snapshot.value(QStringLiteral("number")).isDouble()) {
            errorReason = QStringLiteral("Snapshot entry is missing a numeric 'number' field.");
            return false;
        }

        QVariantMap item;
        const int number = snapshot.value(QStringLiteral("number")).toInt(-1);
        if (number < 0) {
            errorReason = QStringLiteral("Snapshot entry has negative 'number' field.");
            return false;
        }

        const QString type = snapshot.value(QStringLiteral("type")).toString();
        const QString description = snapshot.value(QStringLiteral("description")).toString();
        const bool isCurrent = description == QStringLiteral("current");
        const bool isPost = !snapshot.value(QStringLiteral("pre-number")).isNull()
            && snapshot.value(QStringLiteral("pre-number")).toInt(-1) >= 0;
        const bool isPre = type == QStringLiteral("pre") && !isPost;

        item.insert(QStringLiteral("id"), number);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("isCurrent"), isCurrent);
        item.insert(QStringLiteral("isPre"), isPre);
        item.insert(QStringLiteral("isPost"), isPost);
        item.insert(QStringLiteral("pairedSnapshotId"), -1);
        item.insert(QStringLiteral("preNumber"),
                    isPost ? snapshot.value(QStringLiteral("pre-number")).toInt() : -1);
        item.insert(QStringLiteral("date"), snapshot.value(QStringLiteral("date")).toString());
        item.insert(QStringLiteral("user"), snapshot.value(QStringLiteral("user")).toString());
        item.insert(QStringLiteral("cleanup"), snapshot.value(QStringLiteral("cleanup")).toString());
        item.insert(QStringLiteral("description"), description);

        bool important = false;
        QString zethropolTag;
        const QJsonValue userdataValue = snapshot.value(QStringLiteral("userdata"));
        if (userdataValue.isObject()) {
            const QJsonObject userdata = userdataValue.toObject();
            important = userdata.value(QStringLiteral("important")).toString() == QStringLiteral("yes");
            zethropolTag = userdata.value(QStringLiteral("zethropol")).toString();
        }

        item.insert(QStringLiteral("important"), important);
        item.insert(QStringLiteral("zethropolTag"), zethropolTag);

        if (isCurrent)
            currentId = number;

        snapshots.append(item);
    }

    QVariantList recoveryEvents;
    for (QVariant &snapshotValue : snapshots) {
        QVariantMap snapshot = snapshotValue.toMap();
        const int postId = snapshot.value(QStringLiteral("id")).toInt();
        const int preId = snapshot.value(QStringLiteral("preNumber")).toInt();

        if (!snapshot.value(QStringLiteral("isPost")).toBool() || preId < 0)
            continue;

        for (QVariant &preValue : snapshots) {
            QVariantMap preSnapshot = preValue.toMap();
            if (preSnapshot.value(QStringLiteral("id")).toInt() != preId)
                continue;

            recoveryEvents.append(QVariantMap{
                {QStringLiteral("preId"), preId},
                {QStringLiteral("postId"), postId},
                {QStringLiteral("date"), snapshot.value(QStringLiteral("date"))},
                {QStringLiteral("description"), snapshot.value(QStringLiteral("description"))},
                {QStringLiteral("preDescription"), preSnapshot.value(QStringLiteral("description"))},
                {QStringLiteral("type"), snapshot.value(QStringLiteral("type"))},
                {QStringLiteral("important"), snapshot.value(QStringLiteral("important"))},
                {QStringLiteral("zethropolTag"), snapshot.value(QStringLiteral("zethropolTag"))}
            });
            break;
        }
    }

    for (int i = 0; i < snapshots.size(); ++i) {
        QVariantMap snapshot = snapshots.at(i).toMap();
        const int snapshotId = snapshot.value(QStringLiteral("id")).toInt();
        for (const QVariant &eventValue : recoveryEvents) {
            const QVariantMap event = eventValue.toMap();
            if (event.value(QStringLiteral("preId")).toInt() == snapshotId) {
                snapshot.insert(QStringLiteral("pairedSnapshotId"), event.value(QStringLiteral("postId")).toInt());
                break;
            }
            if (event.value(QStringLiteral("postId")).toInt() == snapshotId) {
                snapshot.insert(QStringLiteral("pairedSnapshotId"), event.value(QStringLiteral("preId")).toInt());
                break;
            }
        }
        snapshots[i] = snapshot;
    }

    QVariantList recoveryPoints;
    for (const QVariant &snapshotValue : snapshots) {
        const QVariantMap snapshot = snapshotValue.toMap();
        if (snapshot.value(QStringLiteral("isCurrent")).toBool())
            continue;
        if (snapshot.value(QStringLiteral("pairedSnapshotId")).toInt() >= 0)
            continue;
        recoveryPoints.append(snapshot);
    }

    m_snapshots = snapshots;
    m_recoveryEvents = recoveryEvents;
    m_recoveryPoints = recoveryPoints;
    m_snapshotCount = snapshots.size();
    m_currentSnapshotId = currentId;
    return true;
}

void RecoveryService::readOutput()
{
    processTimeoutTimer.stop();
    const QString errorOutput = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();

    if (process.property("timedOut").toBool()) {
        setDisconnectedState(
            QStringLiteral("backend_unavailable"),
            QStringLiteral("Recovery backend unavailable"),
            QStringLiteral("Snapper command timed out."));
        return;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString combined = (errorOutput + QLatin1Char('\n') + output).toLower();
        if (combined.contains(QStringLiteral("permission denied"))
            || combined.contains(QStringLiteral("error.no_permissions"))
            || combined.contains(QStringLiteral("accessdenied"))
            || combined.contains(QStringLiteral("not authorized"))
            || combined.contains(QStringLiteral("operation not permitted"))) {
            setDisconnectedState(
                QStringLiteral("permission_denied"),
                QStringLiteral("Authorization required"),
                QStringLiteral("Administrator permission is required to read Snapper snapshots"),
                true);
            return;
        }
        if (combined.contains(QStringLiteral("config"))
            && (combined.contains(QStringLiteral("not found"))
                || combined.contains(QStringLiteral("does not exist"))
                || combined.contains(QStringLiteral("unknown config")))) {
            setDisconnectedState(
                QStringLiteral("installed_not_configured"),
                QStringLiteral("Snapper is not configured"),
                QStringLiteral("Snapper is installed, but no root configuration was found."));
            return;
        }
        if (combined.contains(QStringLiteral("org.freedesktop.dbus.error"))
            || combined.contains(QStringLiteral("failure (org.freedesktop.dbus"))
            || combined.contains(QStringLiteral("dbus"))
            || combined.contains(QStringLiteral("serviceunknown"))
            || combined.contains(QStringLiteral("backend unavailable"))
            || combined.contains(QStringLiteral("snapperd"))) {
            setDisconnectedState(
                QStringLiteral("backend_unavailable"),
                QStringLiteral("Recovery backend unavailable"),
                QStringLiteral("Snapper D-Bus backend (snapperd) is unavailable or unreachable."));
            return;
        }
        if (combined.contains(QStringLiteral("not a btrfs subvolume"))
            || combined.contains(QStringLiteral("invalid config"))
            || combined.contains(QStringLiteral("broken config"))
            || combined.contains(QStringLiteral("reading failed"))
            || combined.contains(QStringLiteral("subvolume"))
            || combined.contains(QStringLiteral("io error"))) {
            setDisconnectedState(
                QStringLiteral("invalid_configuration"),
                QStringLiteral("Invalid recovery configuration"),
                QStringLiteral("Snapper configuration or Btrfs snapshot subvolume is invalid."));
            return;
        }
        setDisconnectedState(
            QStringLiteral("backend_unavailable"),
            QStringLiteral("Recovery backend unavailable"),
            errorOutput.isEmpty() ? QStringLiteral("Snapper backend failed.") : errorOutput);
        return;
    }

    QString parseError;
    if (!parseSnapperJsonOutput(output, m_configName.isEmpty() ? QStringLiteral("root") : m_configName, parseError)) {
        setDisconnectedState(
            QStringLiteral("invalid_configuration"),
            QStringLiteral("Invalid recovery configuration"),
            QStringLiteral("Malformed Snapper output: %1").arg(parseError));
        return;
    }

    m_checking = false;
    m_available = true;
    m_permissionRequired = false;
    m_detectionStatus = QStringLiteral("installed_and_configured");
    m_statusTitle = QStringLiteral("Recovery is available");
    m_statusMessage = QStringLiteral("%1 · %2 · %3 snapshots")
                          .arg(m_filesystem, m_snapshotTool)
                          .arg(m_snapshotCount);
    emit stateChanged();
}

void RecoveryService::readError()
{
    processTimeoutTimer.stop();
    if (process.error() == QProcess::FailedToStart) {
        setDisconnectedState(
            QStringLiteral("not_installed"),
            QStringLiteral("Snapper is not installed"),
            QStringLiteral("Snapper executable could not be started."));
        return;
    }
    if (process.state() == QProcess::NotRunning && m_checking) {
        setDisconnectedState(
            QStringLiteral("backend_unavailable"),
            QStringLiteral("Recovery backend unavailable"),
            process.errorString());
    }
}

bool RecoveryService::validatePreconditionsForCreate(const QString &description, QString &outTrimmed)
{
    if (m_actionBusy || m_checking || process.state() != QProcess::NotRunning || actionProcess.state() != QProcess::NotRunning) {
        m_lastOperationCode = QStringLiteral("busy");
        m_actionStatus = QStringLiteral("Recovery service is busy.");
        emit stateChanged();
        return false;
    }

    outTrimmed = description.trimmed();
    if (outTrimmed.isEmpty()) {
        m_lastOperationCode = QStringLiteral("invalid-argument");
        m_actionStatus = QStringLiteral("Recovery point description is required.");
        emit stateChanged();
        return false;
    }

    if (outTrimmed.size() > 256) {
        m_lastOperationCode = QStringLiteral("invalid-argument");
        m_actionStatus = QStringLiteral("Recovery point description exceeds 256 characters.");
        emit stateChanged();
        return false;
    }

    for (const QChar &ch : outTrimmed) {
        if (ch.isNonCharacter() || ch.category() == QChar::Other_Control) {
            m_lastOperationCode = QStringLiteral("invalid-argument");
            m_actionStatus = QStringLiteral("Recovery point description contains invalid control characters.");
            emit stateChanged();
            return false;
        }
    }

    if (m_detectionStatus == QStringLiteral("not_installed")) {
        m_lastOperationCode = QStringLiteral("not-found");
        m_actionStatus = QStringLiteral("Cannot create recovery point: Snapper is not installed.");
        emit stateChanged();
        return false;
    }

    if (m_detectionStatus == QStringLiteral("installed_not_configured")) {
        m_lastOperationCode = QStringLiteral("not-available");
        m_actionStatus = QStringLiteral("Cannot create recovery point: Snapper root configuration is missing.");
        emit stateChanged();
        return false;
    }

    if (m_detectionStatus == QStringLiteral("invalid_configuration")
        || m_filesystem.compare(QStringLiteral("Btrfs"), Qt::CaseInsensitive) != 0
        || (!m_configName.isEmpty() && m_configName != QStringLiteral("root"))) {
        m_lastOperationCode = QStringLiteral("failed");
        m_actionStatus = QStringLiteral("Cannot create recovery point: Snapper or Btrfs configuration is invalid.");
        emit stateChanged();
        return false;
    }

    if (m_detectionStatus == QStringLiteral("permission_denied")) {
        m_lastOperationCode = QStringLiteral("permission-denied");
        m_actionStatus = QStringLiteral("Cannot create recovery point: Administrator authorization is required.");
        emit stateChanged();
        return false;
    }

    if (m_detectionStatus == QStringLiteral("backend_unavailable") || !m_available) {
        m_lastOperationCode = QStringLiteral("not-available");
        m_actionStatus = QStringLiteral("Cannot create recovery point: Recovery backend is unavailable.");
        emit stateChanged();
        return false;
    }

    return true;
}

void RecoveryService::requestCreateRecoveryPoint(const QString &description)
{
    QString trimmed;
    if (!validatePreconditionsForCreate(description, trimmed)) {
        m_confirmationPending = false;
        m_pendingDescription.clear();
        emit stateChanged();
        return;
    }

    m_pendingDescription = trimmed;
    m_confirmationPending = true;
    emit stateChanged();
}

void RecoveryService::confirmCreateRecoveryPoint()
{
    if (!m_confirmationPending || m_pendingDescription.isEmpty()) {
        m_lastOperationCode = QStringLiteral("confirmation-required");
        m_actionStatus = QStringLiteral("User confirmation is required to create a recovery point.");
        emit stateChanged();
        return;
    }

    const QString desc = m_pendingDescription;
    m_confirmationPending = false;
    m_pendingDescription.clear();
    startCreateOperation(desc);
}

void RecoveryService::cancelCreateRecoveryPoint()
{
    m_confirmationPending = false;
    m_pendingDescription.clear();
    m_lastOperationCode = QStringLiteral("cancelled");
    m_actionStatus = QStringLiteral("Recovery point creation cancelled.");
    emit stateChanged();
}

void RecoveryService::createRecoveryPoint(const QString &description)
{
    m_confirmationPending = false;
    m_pendingDescription.clear();
    startCreateOperation(description);
}

void RecoveryService::startCreateOperation(const QString &description)
{
    QString trimmedDescription;
    if (!validatePreconditionsForCreate(description, trimmedDescription))
        return;

    const QDBusConnection connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        m_actionBusy = false;
        m_lastOperationCode = QStringLiteral("not-available");
        m_actionStatus = QStringLiteral("Cannot create recovery point: System D-Bus connection is unavailable.");
        emit stateChanged();
        return;
    }

    m_actionBusy = true;
    m_actionOperation = QStringLiteral("create");
    m_lastOperationId.clear();
    m_lastOperationCode = QStringLiteral("running");
    m_lastCreatedSnapshotId = 0;
    m_actionStatus = QStringLiteral("Creating recovery point...");
    emit stateChanged();

    const QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Recovery"),
        QStringLiteral("/org/zethropol/Recovery"),
        QStringLiteral("org.zethropol.Recovery.v1"),
        QStringLiteral("CreateRecoveryPoint"))
        << trimmedDescription
        << trimmedDescription;

    auto *watcher = new QDBusPendingCallWatcher(connection.asyncCall(msg, m_operationTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &RecoveryService::handleCreateReply);
}

void RecoveryService::handleCreateReply(QDBusPendingCallWatcher *watcher)
{
    if (!watcher)
        return;
    watcher->deleteLater();

    const QDBusMessage reply = watcher->reply();
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        m_actionBusy = false;
        const QString errName = reply.errorName();
        if (errName.contains(QStringLiteral("Timeout")) || errName.contains(QStringLiteral("NoReply"))) {
            m_lastOperationCode = QStringLiteral("timeout");
            m_actionStatus = QStringLiteral("Recovery point creation timed out.");
        } else {
            m_lastOperationCode = QStringLiteral("not-available");
            m_actionStatus = QStringLiteral("Cannot create recovery point: Recovery backend is unreachable.");
        }
        emit stateChanged();
        return;
    }

    const QVariantList args = reply.arguments();
    bool accepted = false;
    QString code;
    QString message;
    QString operationId;

    if (args.size() >= 4 && args.at(0).userType() == QMetaType::Bool) {
        accepted = args.at(0).toBool();
        code = args.at(1).toString();
        message = args.at(2).toString();
        operationId = args.at(3).toString();
    } else {
        const QVariant firstArg = dbusValueToVariant(args.constFirst());
        if (firstArg.userType() == QMetaType::QVariantMap) {
            const QVariantMap map = firstArg.toMap();
            accepted = map.value(QStringLiteral("success")).toBool();
            code = map.value(QStringLiteral("code")).toString();
            message = map.value(QStringLiteral("message")).toString();
            operationId = map.value(QStringLiteral("operation_id")).toString();
            if (operationId.isEmpty())
                operationId = map.value(QStringLiteral("operationId")).toString();
        }
    }

    if (!accepted || operationId.isEmpty()) {
        m_actionBusy = false;
        m_lastOperationCode = code.isEmpty() ? QStringLiteral("failed") : code;
        m_actionStatus = message.isEmpty()
            ? QStringLiteral("Recovery point creation failed.")
            : message;
        emit stateChanged();
        return;
    }

    m_lastOperationId = operationId;
    m_pollInFlight = false;
    operationElapsed.start();
    operationPollTimer.start();
    pollOperationState();
}

void RecoveryService::pollOperationState()
{
    if (!m_actionBusy || m_lastOperationId.isEmpty()) {
        operationPollTimer.stop();
        return;
    }

    if (operationElapsed.isValid() && operationElapsed.elapsed() > m_operationTimeoutMs) {
        operationPollTimer.stop();
        const QString timedOutOp = m_lastOperationId;
        m_actionBusy = false;
        m_lastOperationCode = QStringLiteral("timeout");
        m_actionStatus = QStringLiteral("Recovery point creation timed out.");
        emit stateChanged();

        const QDBusMessage cancelMsg = QDBusMessage::createMethodCall(
            QStringLiteral("org.zethropol.Recovery"),
            QStringLiteral("/org/zethropol/Recovery"),
            QStringLiteral("org.zethropol.Recovery.v1"),
            QStringLiteral("CancelOperation"))
            << timedOutOp;
        QDBusConnection::systemBus().asyncCall(cancelMsg, 1000);
        return;
    }

    if (m_pollInFlight)
        return;

    m_pollInFlight = true;
    const QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Recovery"),
        QStringLiteral("/org/zethropol/Recovery"),
        QStringLiteral("org.zethropol.Recovery.v1"),
        QStringLiteral("GetOperationState"))
        << m_lastOperationId;

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &RecoveryService::handleOperationStateReply);
}

void RecoveryService::handleOperationStateReply(QDBusPendingCallWatcher *watcher)
{
    m_pollInFlight = false;
    if (!watcher)
        return;
    watcher->deleteLater();

    if (!m_actionBusy)
        return;

    const QDBusMessage reply = watcher->reply();
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        operationPollTimer.stop();
        m_actionBusy = false;
        m_lastOperationCode = QStringLiteral("not-available");
        m_actionStatus = QStringLiteral("Lost connection to Recovery service while creating recovery point.");
        emit stateChanged();
        return;
    }

    const QVariantMap opMap = dbusValueToVariant(reply.arguments().constFirst()).toMap();
    const QString status = opMap.value(QStringLiteral("status")).toString();
    const QString code = opMap.value(QStringLiteral("code")).toString();
    const QString message = opMap.value(QStringLiteral("message")).toString();
    const int snapshotId = opMap.value(QStringLiteral("snapshot_id")).toString().toInt();

    if (status == QStringLiteral("running") || status == QStringLiteral("pending")) {
        if (!message.isEmpty() && m_actionStatus != message) {
            m_actionStatus = message;
            emit stateChanged();
        }
        return;
    }

    operationPollTimer.stop();
    m_actionBusy = false;

    if (status == QStringLiteral("ok")) {
        if (snapshotId <= 0) {
            m_lastOperationCode = QStringLiteral("verification-failed");
            m_lastCreatedSnapshotId = 0;
            m_actionStatus = QStringLiteral("Recovery point creation could not be verified: invalid snapshot ID.");
            emit stateChanged();
            return;
        }
        m_lastCreatedSnapshotId = snapshotId;
        m_lastOperationCode = QStringLiteral("ok");
        m_actionStatus = message.isEmpty()
            ? QStringLiteral("Recovery point #%1 created successfully.").arg(snapshotId)
            : message;
        emit stateChanged();
        refresh();
        return;
    }

    if (status == QStringLiteral("cancelled")) {
        m_lastOperationCode = QStringLiteral("cancelled");
        m_lastCreatedSnapshotId = 0;
        m_actionStatus = message.isEmpty()
            ? QStringLiteral("Recovery point creation was cancelled.")
            : message;
        emit stateChanged();
        return;
    }

    m_lastOperationCode = code.isEmpty() ? QStringLiteral("failed") : code;
    m_lastCreatedSnapshotId = 0;
    m_actionStatus = message.isEmpty()
        ? QStringLiteral("Recovery point creation failed.")
        : message;
    emit stateChanged();
}

void RecoveryService::cancelActiveOperation()
{
    if (!m_actionBusy)
        return;

    operationPollTimer.stop();
    const QString opId = m_lastOperationId;
    m_actionBusy = false;
    m_lastOperationCode = QStringLiteral("cancelled");
    m_lastCreatedSnapshotId = 0;
    m_actionStatus = QStringLiteral("Recovery point creation was cancelled.");
    emit stateChanged();

    if (!opId.isEmpty() && QDBusConnection::systemBus().isConnected()) {
        const QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.zethropol.Recovery"),
            QStringLiteral("/org/zethropol/Recovery"),
            QStringLiteral("org.zethropol.Recovery.v1"),
            QStringLiteral("CancelOperation"))
            << opId;
        QDBusConnection::systemBus().asyncCall(msg, 1000);
    }
}

void RecoveryService::preflightRollback(int pointId)
{
    if (m_preflightBusy)
        return;

    m_selectedRollbackPointId = pointId;

    if (pointId <= 0) {
        m_preflightResult = QVariantMap{
            {QStringLiteral("pointId"), pointId},
            {QStringLiteral("overallStatus"), QStringLiteral("blocked")},
            {QStringLiteral("canProceed"), false},
            {QStringLiteral("rollbackExecutionEnabled"), false},
            {QStringLiteral("summary"), QStringLiteral("Snapshot ID must be a positive integer (> 0).")},
            {QStringLiteral("checks"), QVariantList{}}
        };
        emit stateChanged();
        return;
    }

    const QDBusConnection connection = QDBusConnection::systemBus();
    if (!connection.isConnected()) {
        m_preflightResult = QVariantMap{
            {QStringLiteral("pointId"), pointId},
            {QStringLiteral("overallStatus"), QStringLiteral("blocked")},
            {QStringLiteral("canProceed"), false},
            {QStringLiteral("rollbackExecutionEnabled"), false},
            {QStringLiteral("summary"), QStringLiteral("System D-Bus connection is unavailable for rollback preflight.")},
            {QStringLiteral("checks"), QVariantList{}}
        };
        emit stateChanged();
        return;
    }

    m_preflightBusy = true;
    m_preflightResult.clear();
    emit stateChanged();

    const QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.zethropol.Recovery"),
        QStringLiteral("/org/zethropol/Recovery"),
        QStringLiteral("org.zethropol.Recovery.v1"),
        QStringLiteral("PreflightRollback"))
        << static_cast<uint>(pointId);

    auto *watcher = new QDBusPendingCallWatcher(connection.asyncCall(msg, 4000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &RecoveryService::handlePreflightReply);
}

void RecoveryService::handlePreflightReply(QDBusPendingCallWatcher *watcher)
{
    if (!watcher)
        return;
    watcher->deleteLater();
    m_preflightBusy = false;

    QDBusPendingReply<QVariantMap> reply = *watcher;
    if (reply.isError()) {
        m_preflightResult = QVariantMap{
            {QStringLiteral("pointId"), m_selectedRollbackPointId},
            {QStringLiteral("overallStatus"), QStringLiteral("blocked")},
            {QStringLiteral("canProceed"), false},
            {QStringLiteral("rollbackExecutionEnabled"), false},
            {QStringLiteral("summary"), QStringLiteral("Recovery D-Bus service is unreachable for rollback preflight.")},
            {QStringLiteral("checks"), QVariantList{}}
        };
        emit stateChanged();
        return;
    }

    const QVariantMap envelope = dbusValueToVariant(reply.value()).toMap();
    const QVariantMap data = dbusValueToVariant(envelope.value(QStringLiteral("data"))).toMap();
    if (data.isEmpty() || !data.contains(QStringLiteral("overallStatus"))) {
        m_preflightResult = QVariantMap{
            {QStringLiteral("pointId"), m_selectedRollbackPointId},
            {QStringLiteral("overallStatus"), QStringLiteral("unknown")},
            {QStringLiteral("canProceed"), false},
            {QStringLiteral("rollbackExecutionEnabled"), false},
            {QStringLiteral("summary"), QStringLiteral("Rollback preflight returned incomplete verification data.")},
            {QStringLiteral("checks"), QVariantList{}}
        };
        emit stateChanged();
        return;
    }

    m_preflightResult = data;
    emit stateChanged();
}

void RecoveryService::deleteRecoveryPoint(int snapshotId)
{
    if (m_actionBusy)
        return;

    if (snapshotId <= 0) {
        m_actionStatus = QStringLiteral("Invalid recovery point.");
        emit stateChanged();
        return;
    }

    bool knownRecoveryPoint = false;

    for (const QVariant &value : m_recoveryPoints) {
        const QVariantMap point = value.toMap();
        if (point.value(QStringLiteral("id")).toInt() == snapshotId) {
            knownRecoveryPoint = true;
            break;
        }
    }

    if (!knownRecoveryPoint) {
        m_actionStatus = QStringLiteral("Recovery point is not available for deletion.");
        emit stateChanged();
        return;
    }

    if (m_checking || process.state() != QProcess::NotRunning || actionProcess.state() != QProcess::NotRunning) {
        m_actionStatus = QStringLiteral("Recovery service is busy.");
        emit stateChanged();
        return;
    }

    m_actionBusy = true;
    m_actionOperation = QStringLiteral("delete");
    m_actionStatus = QStringLiteral("Deleting recovery point #%1...").arg(snapshotId);
    emit stateChanged();

    actionProcess.setProgram(QStringLiteral("/usr/bin/pkexec"));
    actionProcess.setArguments({
        QStringLiteral("/usr/bin/snapper"),
        QStringLiteral("delete"),
        QString::number(snapshotId)
    });
    actionProcess.setProcessChannelMode(QProcess::SeparateChannels);
    actionProcess.start();
}
