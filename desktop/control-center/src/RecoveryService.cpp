#include "RecoveryService.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

RecoveryService::RecoveryService(QObject *parent)
    : QObject(parent)
{
    connect(&process, &QProcess::finished, this, &RecoveryService::readOutput);
    connect(&process, &QProcess::errorOccurred, this, &RecoveryService::readError);

    check();
}

bool RecoveryService::checking() const
{
    return m_checking;
}

bool RecoveryService::available() const
{
    return m_available;
}

bool RecoveryService::permissionRequired() const
{
    return m_permissionRequired;
}

QString RecoveryService::filesystem() const
{
    return m_filesystem;
}

QString RecoveryService::snapshotTool() const
{
    return m_snapshotTool;
}

int RecoveryService::snapshotCount() const
{
    return m_snapshotCount;
}

int RecoveryService::currentSnapshotId() const
{
    return m_currentSnapshotId;
}

QVariantList RecoveryService::snapshots() const
{
    return m_snapshots;
}

QVariantList RecoveryService::recoveryEvents() const
{
    return m_recoveryEvents;
}

QVariantList RecoveryService::recoveryPoints() const
{
    return m_recoveryPoints;
}

void RecoveryService::check()
{
    if (process.state() != QProcess::NotRunning)
        return;

    m_checking = true;
    m_permissionRequired = false;
    emit stateChanged();

    startProcess(QStringLiteral("/usr/bin/snapper"),
                 {QStringLiteral("--jsonout"), QStringLiteral("list")});
}

void RecoveryService::refresh()
{
    if (process.state() != QProcess::NotRunning)
        return;

    m_checking = true;
    m_permissionRequired = false;
    emit stateChanged();

    startProcess(QStringLiteral("/usr/bin/pkexec"),
                 {QStringLiteral("/usr/bin/snapper"),
                  QStringLiteral("--jsonout"),
                  QStringLiteral("list")});
}

void RecoveryService::startProcess(const QString &program, const QStringList &arguments)
{
    process.setProgram(program);
    process.setArguments(arguments);
    process.start();
}

void RecoveryService::readOutput()
{
    const QString errorOutput = QString::fromLocal8Bit(process.readAllStandardError());
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());

    m_checking = false;

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        m_available = false;
        m_permissionRequired = true;
        m_filesystem = QStringLiteral("Btrfs");
        m_snapshotTool = QStringLiteral("Snapper");
        m_snapshotCount = 0;
        m_currentSnapshotId = 0;
        m_snapshots.clear();
        m_recoveryEvents.clear();
        Q_UNUSED(errorOutput);
        emit stateChanged();
        return;
    }

    Q_UNUSED(errorOutput);

    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8());

    if (!document.isObject()) {
        m_available = false;
        m_snapshotCount = 0;
        m_currentSnapshotId = 0;
        m_snapshots.clear();
        m_recoveryEvents.clear();
        emit stateChanged();
        return;
    }

    const QJsonObject root = document.object();
    const QJsonArray entries = root.value(QStringLiteral("root")).toArray();

    QVariantList snapshots;
    int currentId = 0;

    for (const QJsonValue &value : entries) {
        const QJsonObject snapshot = value.toObject();

        QVariantMap item;
        const int number = snapshot.value(QStringLiteral("number")).toInt();

        const QString type = snapshot.value(QStringLiteral("type")).toString();
        const QString description = snapshot.value(QStringLiteral("description")).toString();
        const bool isCurrent = description == QStringLiteral("current");
        const bool isPost = !snapshot.value(QStringLiteral("pre-number")).isNull();
        const bool isPre = type == QStringLiteral("pre") && !isPost;

        item.insert(QStringLiteral("id"), number);
        item.insert(QStringLiteral("type"), type);
        item.insert(QStringLiteral("isCurrent"), isCurrent);
        item.insert(QStringLiteral("isPre"), isPre);
        item.insert(QStringLiteral("isPost"), isPost);
        item.insert(QStringLiteral("pairedSnapshotId"), -1);
        item.insert(QStringLiteral("preNumber"),
                    snapshot.value(QStringLiteral("pre-number")).isNull()
                        ? -1
                        : snapshot.value(QStringLiteral("pre-number")).toInt());
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

            snapshot.insert(QStringLiteral("pairedSnapshotId"), preId);
            preSnapshot.insert(QStringLiteral("pairedSnapshotId"), postId);
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

    m_available = true;
    m_permissionRequired = false;
    m_filesystem = QStringLiteral("Btrfs");
    m_snapshotTool = QStringLiteral("Snapper");
    m_snapshotCount = snapshots.size();
    m_currentSnapshotId = currentId;
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

    emit stateChanged();
}

void RecoveryService::readError()
{
    if (process.state() == QProcess::NotRunning)
        return;

    m_checking = false;
    emit stateChanged();
}
