#include "UpdateService.h"

#include "core/zethropol_update_client.h"

#include <QRegularExpression>
#include <QStringList>
#include <QStandardPaths>

UpdateService::UpdateService(QObject *parent)
    : QObject(parent)
{
    connect(&m_updateClient, &ZethropolUpdateClient::refreshFinished,
            this, &UpdateService::finishOfficialScan);
    connect(&orphanProcess, &QProcess::finished,
            this, &UpdateService::finishOrphanScan);
    connect(&aurProcess, &QProcess::finished,
            this, &UpdateService::finishAurScan);
    connect(&flatpakProcess, &QProcess::finished,
            this, &UpdateService::finishFlatpakScan);

    connect(&aurProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking && !m_installing)
            handleScanStartFailure(QStringLiteral("AUR scan"), aurProcess.errorString());
    });
    connect(&flatpakProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking && !m_installing)
            handleScanStartFailure(QStringLiteral("Flatpak scan"), flatpakProcess.errorString());
    });
    connect(&orphanProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_checking && !m_installing)
            handleScanStartFailure(QStringLiteral("Orphan package scan"),
                                   orphanProcess.errorString());
    });

    check();
}

bool UpdateService::checking() const
{
    return m_checking;
}

bool UpdateService::installing() const
{
    return m_installing;
}

bool UpdateService::available() const
{
    return m_available;
}

bool UpdateService::failed() const
{
    return m_failed;
}

int UpdateService::count() const
{
    return m_count;
}

QString UpdateService::status() const
{
    return m_status;
}

QString UpdateService::output() const
{
    return m_output;
}

QVariantList UpdateService::updates() const
{
    return m_updates;
}

QStringList UpdateService::orphanPackages() const
{
    return m_orphanPackages;
}

int UpdateService::orphanCount() const
{
    return m_orphanPackages.size();
}

bool UpdateService::removingOrphans() const
{
    return m_removingOrphans;
}

bool UpdateService::aurAvailable() const
{
    return m_aurAvailable;
}

bool UpdateService::flatpakAvailable() const
{
    return m_flatpakAvailable;
}

int UpdateService::aurCount() const
{
    return m_aurUpdates.size();
}

int UpdateService::flatpakCount() const
{
    return m_flatpakUpdates.size();
}

QVariantList UpdateService::aurUpdates() const
{
    return m_aurUpdates;
}

QVariantList UpdateService::flatpakUpdates() const
{
    return m_flatpakUpdates;
}

int UpdateService::selectedCount() const
{
    int total = 0;
    const auto countSelected = [](const QVariantList &items) {
        int count = 0;
        for (const QVariant &item : items) {
            if (item.toMap().value(QStringLiteral("selected")).toBool())
                ++count;
        }
        return count;
    };

    total += countSelected(m_updates);
    total += countSelected(m_aurUpdates);
    total += countSelected(m_flatpakUpdates);
    return total;
}

QString UpdateService::planPreview() const
{
    QStringList lines;
    lines << QStringLiteral("UPDATE PLAN PREVIEW — NOT APPROVED FOR EXECUTION");
    lines << QStringLiteral("Selected packages: %1").arg(selectedCount());
    lines << QString();

    const auto appendSelected = [&lines](
        const QString &source, const QVariantList &items) {
        lines << QStringLiteral("[%1]").arg(source);
        int found = 0;
        for (const QVariant &item : items) {
            const QVariantMap package = item.toMap();
            if (!package.value(QStringLiteral("selected")).toBool())
                continue;
            QString entry = package.value(QStringLiteral("name")).toString();
            const QString current = package.value(QStringLiteral("currentVersion")).toString();
            const QString next = package.value(QStringLiteral("newVersion")).toString();
            if (!current.isEmpty() || !next.isEmpty())
                entry += QStringLiteral("  %1 -> %2").arg(current, next);
            lines << QStringLiteral("  • %1").arg(entry);
            ++found;
        }
        if (found == 0)
            lines << QStringLiteral("  No packages selected");
        lines << QString();
    };

    appendSelected(QStringLiteral("Official repositories"), m_updates);
    appendSelected(QStringLiteral("AUR"), m_aurUpdates);
    appendSelected(QStringLiteral("Flatpak"), m_flatpakUpdates);

    lines << QStringLiteral("SAFETY CHECKS");
    if (selectedCount() == 0)
        lines << QStringLiteral("BLOCKED: Select at least one package.");

    bool official = false, aur = false, flatpak = false;
    for (const QVariant &item : m_updates)
        official |= item.toMap().value(QStringLiteral("selected")).toBool();
    for (const QVariant &item : m_aurUpdates)
        aur |= item.toMap().value(QStringLiteral("selected")).toBool();
    for (const QVariant &item : m_flatpakUpdates)
        flatpak |= item.toMap().value(QStringLiteral("selected")).toBool();

    if (official)
        lines << QStringLiteral("BLOCKED: Official dependency transaction and compatibility plan are unverified; partial upgrades are not permitted.");
    if (aur)
        lines << QStringLiteral("BLOCKED: AUR PKGBUILD changes, dependencies, conflicts and build effects are unverified.");
    if (flatpak)
        lines << QStringLiteral("BLOCKED: Stable Flatpak refs and runtime/dependency effects are unverified.");

    lines << QString();
    lines << QStringLiteral("This preview does not resolve dependencies or simulate the package-manager transaction.");
    lines << QStringLiteral("No packages will be installed. Installation remains disabled.");
    return lines.join(QStringLiteral("\n"));
}

void UpdateService::removeOrphans()
{
    m_removingOrphans = false;
    m_failed = true;
    m_status = QStringLiteral(
        "Orphan package removal is disabled until transaction preflight is implemented.");
    m_output = QStringLiteral("No package removal was started.");
    emit stateChanged();
}

void UpdateService::install()
{
    m_failed = true;
    m_status = QStringLiteral(
        "Updates blocked: package selection and transaction preflight are not implemented yet.");
    m_output = QStringLiteral(
        "No package changes were started. Dependency, conflict, and compatibility checks are required before execution.");
    emit stateChanged();
}

void UpdateService::setPackageSelected(
    const QString &source, const QString &name, bool selected)
{
    QVariantList *items = nullptr;

    if (source == QStringLiteral("official"))
        items = &m_updates;
    else if (source == QStringLiteral("aur"))
        items = &m_aurUpdates;
    else if (source == QStringLiteral("flatpak"))
        items = &m_flatpakUpdates;
    else
        return;

    for (QVariant &item : *items) {
        QVariantMap package = item.toMap();
        if (package.value(QStringLiteral("name")).toString() != name)
            continue;

        package.insert(QStringLiteral("selected"), selected);
        item = package;
        emit stateChanged();
        return;
    }
}

void UpdateService::check()
{
    if (m_checking ||
        orphanProcess.state() != QProcess::NotRunning ||
        aurProcess.state() != QProcess::NotRunning ||
        flatpakProcess.state() != QProcess::NotRunning)
        return;

    m_checking = true;
    m_installing = false;
    m_failed = false;
    m_status = QStringLiteral("Checking for updates...");
    m_output.clear();
    m_updates.clear();
    m_aurUpdates.clear();
    m_flatpakUpdates.clear();
    m_aurAvailable = false;
    m_flatpakAvailable = false;
    m_pendingChecks = 2;
    m_scanErrors.clear();
    m_scanWarnings.clear();

    m_aurHelper.clear();

    const QStringList aurHelpers = {
        QStringLiteral("paru"),
        QStringLiteral("yay"),
        QStringLiteral("pikaur")
    };

    for (const QString &helper : aurHelpers) {
        const QString path = QStandardPaths::findExecutable(helper);
        if (!path.isEmpty()) {
            m_aurHelper = path;
            break;
        }
    }

    const QString flatpakPath =
        QStandardPaths::findExecutable(QStringLiteral("flatpak"));

    if (m_aurHelper.isEmpty())
        m_scanWarnings.append(QStringLiteral("AUR helper unavailable"));
    if (flatpakPath.isEmpty())
        m_scanWarnings.append(QStringLiteral("Flatpak unavailable"));

    emit stateChanged();

    m_updateClient.refreshUpdateState();

    orphanProcess.start(QStringLiteral("/usr/bin/pacman"), {
        QStringLiteral("-Qtdq")
    });

    if (!m_aurHelper.isEmpty()) {
        ++m_pendingChecks;
        aurProcess.start(m_aurHelper, {
            QStringLiteral("-Qua")
        });
    }

    if (!flatpakPath.isEmpty()) {
        ++m_pendingChecks;
        flatpakProcess.start(flatpakPath, {
            QStringLiteral("remote-ls"),
            QStringLiteral("--updates"),
            QStringLiteral("--columns=application")
        });
    }
}

void UpdateService::finishOfficialScan(bool success, const QString &message)
{
    if (!m_checking || m_pendingChecks <= 0)
        return;

    if (!success) {
        m_scanErrors.append(
            QStringLiteral("Official repository scan failed: %1")
                .arg(message.isEmpty()
                    ? QStringLiteral("Update D-Bus request failed.")
                    : message));
        m_failed = true;
        m_updates.clear();
        m_count = 0;
        m_available = false;
    } else {
        const QVariantList pending = m_updateClient.pendingUpdates();
        QVariantList normalized;
        normalized.reserve(pending.size());

        for (const QVariant &entry : pending) {
            const QVariantMap source = entry.toMap();
            QVariantMap update;
            update.insert(QStringLiteral("name"),
                          source.value(QStringLiteral("package_name")));
            update.insert(QStringLiteral("currentVersion"),
                          source.value(QStringLiteral("current_version")));
            update.insert(QStringLiteral("newVersion"),
                          source.value(QStringLiteral("new_version")));
            update.insert(QStringLiteral("repository"),
                          source.value(QStringLiteral("repository")));
            update.insert(QStringLiteral("download_size"),
                          source.value(QStringLiteral("download_size")));
            update.insert(QStringLiteral("selected"), false);
            normalized.append(update);
        }

        m_updates = normalized;
        m_count = m_updates.size();
        m_available = !m_updates.isEmpty();
    }

    --m_pendingChecks;
    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::handleScanStartFailure(
    const QString &source, const QString &error)
{
    if (!m_checking || m_installing || m_pendingChecks <= 0)
        return;

    m_scanErrors.append(QStringLiteral("%1 could not start: %2")
                            .arg(source, error));
    m_failed = true;
    --m_pendingChecks;
    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::finishScanIfComplete()
{
    if (!m_checking || m_pendingChecks != 0)
        return;

    m_checking = false;

    if (!m_scanErrors.isEmpty()) {
        m_failed = true;
        m_status = QStringLiteral("Update scan incomplete: %1")
                       .arg(m_scanErrors.join(QStringLiteral("; ")));
    } else {
        m_failed = false;
        m_status = (m_available || m_aurAvailable || m_flatpakAvailable)
            ? QStringLiteral("Updates are available.")
            : QStringLiteral("System is up to date.");
    }

    if (!m_scanWarnings.isEmpty())
        m_status += QStringLiteral(" Unavailable sources: %1.")
                        .arg(m_scanWarnings.join(QStringLiteral(", ")));

    if (!m_scanErrors.isEmpty()) {
        emit operationError(QStringLiteral("Update scan failed"),
                            m_scanErrors.join(QStringLiteral("\n")));
    }
}

void UpdateService::finishOrphanScan()
{
    if (!m_checking || m_installing)
        return;

    const QString output =
        QString::fromLocal8Bit(orphanProcess.readAllStandardOutput()).trimmed();
    const QString error =
        QString::fromLocal8Bit(orphanProcess.readAllStandardError()).trimmed();
    const bool normalExit = orphanProcess.exitStatus() == QProcess::NormalExit;
    const int exitCode = orphanProcess.exitCode();

    // pacman -Qtdq exits with code 1 when no orphan packages match.
    if (normalExit && (exitCode == 0 || (exitCode == 1 && output.isEmpty()))) {
        m_orphanPackages = output.isEmpty()
            ? QStringList()
            : output.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                           Qt::SkipEmptyParts);
    } else {
        m_orphanPackages.clear();
        m_scanErrors.append(QStringLiteral("Orphan package scan failed: %1")
            .arg(error.isEmpty()
                ? QStringLiteral("Process exited with code %1.").arg(exitCode)
                : error));
        m_failed = true;
    }

    if (m_pendingChecks > 0)
        --m_pendingChecks;

    finishScanIfComplete();
    emit stateChanged();
}

void UpdateService::finishAurScan()
{
    const QString output =
        QString::fromLocal8Bit(aurProcess.readAllStandardOutput()).trimmed();

    if (aurProcess.exitStatus() != QProcess::NormalExit ||
        aurProcess.exitCode() != 0) {
        const QString error =
            QString::fromLocal8Bit(aurProcess.readAllStandardError()).trimmed();
        m_aurUpdates.clear();
        m_aurAvailable = false;
        m_scanErrors.append(QStringLiteral("AUR scan failed: %1")
            .arg(error.isEmpty()
                ? QStringLiteral("Process exited with code %1.").arg(aurProcess.exitCode())
                : error));
        m_failed = true;
        if (m_pendingChecks > 0)
            --m_pendingChecks;
        finishScanIfComplete();
        emit stateChanged();
        return;
    }

    QVariantList updates;

    const QStringList lines = output.split(
        QRegularExpression(QStringLiteral("[\r\n]+")),
        Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        const QStringList fields = line.split(
            QRegularExpression(QStringLiteral("[[:space:]]+")),
            Qt::SkipEmptyParts);

        if (fields.size() < 4 || fields.at(2) != QStringLiteral("->"))
            continue;

        QVariantMap update;
        update.insert(QStringLiteral("name"), fields.at(0));
        update.insert(QStringLiteral("currentVersion"), fields.at(1));
        update.insert(QStringLiteral("newVersion"), fields.at(3));
        update.insert(QStringLiteral("selected"), false);
        updates.append(update);
    }

    m_aurUpdates = updates;
    m_aurAvailable = !m_aurUpdates.isEmpty();

    if (m_pendingChecks > 0)
        --m_pendingChecks;

    finishScanIfComplete();

    emit stateChanged();
}

void UpdateService::finishFlatpakScan()
{
    const QString output =
        QString::fromLocal8Bit(flatpakProcess.readAllStandardOutput()).trimmed();

    if (flatpakProcess.exitStatus() != QProcess::NormalExit ||
        flatpakProcess.exitCode() != 0) {
        const QString error =
            QString::fromLocal8Bit(flatpakProcess.readAllStandardError()).trimmed();
        m_flatpakUpdates.clear();
        m_flatpakAvailable = false;
        m_scanErrors.append(QStringLiteral("Flatpak scan failed: %1")
            .arg(error.isEmpty()
                ? QStringLiteral("Process exited with code %1.").arg(flatpakProcess.exitCode())
                : error));
        m_failed = true;
        if (m_pendingChecks > 0)
            --m_pendingChecks;
        finishScanIfComplete();
        emit stateChanged();
        return;
    }

    QVariantList updates;

    const QStringList lines = output.split(
        QRegularExpression(QStringLiteral("[\r\n]+")),
        Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        QVariantMap update;
        update.insert(QStringLiteral("name"), line.trimmed());
        update.insert(QStringLiteral("selected"), false);
        updates.append(update);
    }

    m_flatpakUpdates = updates;
    m_flatpakAvailable = !m_flatpakUpdates.isEmpty();

    if (m_pendingChecks > 0)
        --m_pendingChecks;

    finishScanIfComplete();

    emit stateChanged();
}
