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

        item.insert(QStringLiteral("id"), number);
        item.insert(QStringLiteral("type"), snapshot.value(QStringLiteral("type")).toString());
        item.insert(QStringLiteral("preNumber"),
                    snapshot.value(QStringLiteral("pre-number")).isNull()
                        ? -1
                        : snapshot.value(QStringLiteral("pre-number")).toInt());
        item.insert(QStringLiteral("date"), snapshot.value(QStringLiteral("date")).toString());
        item.insert(QStringLiteral("user"), snapshot.value(QStringLiteral("user")).toString());
        item.insert(QStringLiteral("cleanup"), snapshot.value(QStringLiteral("cleanup")).toString());
        item.insert(QStringLiteral("description"),
                    snapshot.value(QStringLiteral("description")).toString());

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

        if (number == 0 && snapshot.value(QStringLiteral("description")).toString() == QStringLiteral("current"))
            currentId = 0;

        snapshots.append(item);
    }

    m_available = true;
    m_permissionRequired = false;
    m_filesystem = QStringLiteral("Btrfs");
    m_snapshotTool = QStringLiteral("Snapper");
    m_snapshotCount = snapshots.size();
    m_currentSnapshotId = currentId;
    m_snapshots = snapshots;

    emit stateChanged();
}

void RecoveryService::readError()
{
    if (process.state() == QProcess::NotRunning)
        return;

    m_checking = false;
    emit stateChanged();
}
