#include "ApplicationService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>

namespace {

const QSet<QString> s_protectedExactPackages = {
    QStringLiteral("base"),
    QStringLiteral("base-devel"),
    QStringLiteral("glibc"),
    QStringLiteral("gcc-libs"),
    QStringLiteral("libgcc"),
    QStringLiteral("dbus"),
    QStringLiteral("dbus-broker"),
    QStringLiteral("dbus-broker-units"),
    QStringLiteral("polkit"),
    QStringLiteral("polkit-kde-agent"),
    QStringLiteral("pacman"),
    QStringLiteral("pacman-contrib"),
    QStringLiteral("arch-update"),
    QStringLiteral("cachy-update"),
    QStringLiteral("snapper"),
    QStringLiteral("snap-pac"),
    QStringLiteral("limine-snapper-sync"),
    QStringLiteral("btrfs-progs"),
    QStringLiteral("mkinitcpio"),
    QStringLiteral("dracut"),
    QStringLiteral("sudo"),
    QStringLiteral("sudo-rs"),
    QStringLiteral("networkmanager"),
    QStringLiteral("ufw"),
    QStringLiteral("firewalld"),
    QStringLiteral("pipewire"),
    QStringLiteral("wireplumber"),
    QStringLiteral("mesa"),
    QStringLiteral("konsole"),
    QStringLiteral("dolphin"),
    QStringLiteral("systemsettings"),
    QStringLiteral("sddm"),
    QStringLiteral("power-profiles-daemon"),
    QStringLiteral("smartmontools"),
    QStringLiteral("fwupd"),
    QStringLiteral("util-linux")
};

const QStringList s_protectedPrefixes = {
    QStringLiteral("zethropol-"),
    QStringLiteral("linux"),
    QStringLiteral("systemd"),
    QStringLiteral("plasma-"),
    QStringLiteral("kwin"),
    QStringLiteral("qt6-"),
    QStringLiteral("limine"),
    QStringLiteral("grub")
};

} // namespace

bool ApplicationService::isValidPackageName(const QString &package)
{
    if (package.isEmpty() || package.size() > 128)
        return false;
    if (package.startsWith(QLatin1Char('-')) || package.startsWith(QLatin1Char('.')))
        return false;
    static const QRegularExpression pkgRe(QStringLiteral("^[a-z0-9@._+-]+$"));
    return pkgRe.match(package).hasMatch();
}

bool ApplicationService::isProtectedPackage(const QString &package)
{
    const QString lower = package.trimmed().toLower();
    if (lower.isEmpty())
        return true;
    if (s_protectedExactPackages.contains(lower))
        return true;
    for (const QString &prefix : s_protectedPrefixes) {
        if (lower.startsWith(prefix))
            return true;
    }
    return false;
}

QHash<QString, QString> ApplicationService::parsePacmanOwnerOutput(const QString &stdoutText)
{
    QHash<QString, QString> owners;
    static const QRegularExpression englishRe(
        QStringLiteral("^(.+?)\\s+is owned by\\s+(\\S+)\\s+\\S+$"));
    const QStringList lines = stdoutText.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = englishRe.match(line.trimmed());
        if (m.hasMatch()) {
            owners.insert(m.captured(1), m.captured(2));
        }
    }
    return owners;
}

ApplicationService::ApplicationService(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QVariantList ApplicationService::applications() const { return m_applications; }
int ApplicationService::total() const { return m_total; }
bool ApplicationService::removing() const { return m_removing; }
QString ApplicationService::removeStatus() const { return m_removeStatus; }

void ApplicationService::refresh()
{
    const QStringList applicationPaths = {
        QStringLiteral("/usr/share/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)
    };

    QSet<QString> seenFiles;
    QVariantList applications;
    QStringList systemDesktopFiles;

    for (const QString &path : applicationPaths) {
        QDir directory(path);
        if (!directory.exists())
            continue;

        const QFileInfoList files = directory.entryInfoList(
            {QStringLiteral("*.desktop")},
            QDir::Files | QDir::Readable,
            QDir::Name);

        for (const QFileInfo &fileInfo : files) {
            const QString filePath = fileInfo.absoluteFilePath();

            if (seenFiles.contains(filePath))
                continue;

            seenFiles.insert(filePath);

            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            QTextStream stream(&file);
            QVariantMap application;
            bool desktopEntry = false;
            bool hidden = false;
            bool noDisplay = false;

            while (!stream.atEnd()) {
                const QString line = stream.readLine().trimmed();

                if (line == QStringLiteral("[Desktop Entry]")) {
                    desktopEntry = true;
                    continue;
                }

                if (!desktopEntry)
                    continue;

                if (line.startsWith(QStringLiteral("[")) &&
                    line.endsWith(QStringLiteral("]")))
                    break;

                const int separator = line.indexOf(QLatin1Char('='));
                if (separator <= 0)
                    continue;

                const QString key = line.left(separator);
                const QString value = line.mid(separator + 1);

                if (key == QStringLiteral("Name"))
                    application.insert(QStringLiteral("name"), value);
                else if (key == QStringLiteral("GenericName"))
                    application.insert(QStringLiteral("genericName"), value);
                else if (key == QStringLiteral("Comment"))
                    application.insert(QStringLiteral("description"), value);
                else if (key == QStringLiteral("Icon"))
                    application.insert(QStringLiteral("icon"), value);
                else if (key == QStringLiteral("Categories"))
                    application.insert(QStringLiteral("categories"), value);
                else if (key == QStringLiteral("Exec"))
                    application.insert(QStringLiteral("exec"), value);
                else if (key == QStringLiteral("Hidden"))
                    hidden = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
                else if (key == QStringLiteral("NoDisplay"))
                    noDisplay = value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
            }

            if (!desktopEntry || hidden || noDisplay)
                continue;

            if (!application.contains(QStringLiteral("name")))
                continue;

            application.insert(QStringLiteral("desktopFile"), fileInfo.fileName());
            application.insert(QStringLiteral("path"), filePath);
            application.insert(
                QStringLiteral("launchable"),
                application.contains(QStringLiteral("exec")));
            application.insert(QStringLiteral("package"), QString());
            application.insert(QStringLiteral("packageManager"), QString());
            application.insert(QStringLiteral("protected"), false);
            application.insert(QStringLiteral("removable"), false);

            if (filePath.startsWith(QStringLiteral("/usr/share/applications/")) ||
                filePath.startsWith(QStringLiteral("/usr/local/share/applications/"))) {
                systemDesktopFiles.append(filePath);
            }

            applications.append(application);
        }
    }

    // Resolve package ownership in ONE batched LC_ALL=C pacman -Qo call instead of per-file blocking.
    QHash<QString, QString> owners;
    if (!systemDesktopFiles.isEmpty()) {
        QProcess ownerProcess;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        ownerProcess.setProcessEnvironment(env);
        QStringList args{QStringLiteral("-Qo"), QStringLiteral("--")};
        args.append(systemDesktopFiles);
        ownerProcess.start(QStringLiteral("/usr/bin/pacman"), args);
        if (ownerProcess.waitForFinished(5000)) {
            owners = parsePacmanOwnerOutput(
                QString::fromUtf8(ownerProcess.readAllStandardOutput()));
        }
    }

    for (QVariant &entry : applications) {
        QVariantMap app = entry.toMap();
        const QString filePath = app.value(QStringLiteral("path")).toString();
        const QString pkg = owners.value(filePath);
        if (!pkg.isEmpty() && isValidPackageName(pkg)) {
            const bool isProtected = isProtectedPackage(pkg);
            app.insert(QStringLiteral("package"), pkg);
            app.insert(QStringLiteral("packageManager"), QStringLiteral("pacman"));
            app.insert(QStringLiteral("protected"), isProtected);
            app.insert(QStringLiteral("removable"), !isProtected);
            entry = app;
        }
    }

    std::sort(
        applications.begin(),
        applications.end(),
        [](const QVariant &left, const QVariant &right) {
            return left.toMap().value(QStringLiteral("name")).toString().localeAwareCompare(
                       right.toMap().value(QStringLiteral("name")).toString()) < 0;
        });

    m_applications = applications;
    m_total = applications.size();

    emit stateChanged();
}

void ApplicationService::removeApplication(const QString &package, bool userConfirmed)
{
    if (m_removing)
        return;

    if (!userConfirmed) {
        m_removeStatus = QStringLiteral("Paket kaldırma işlemi açık kullanıcı onayı gerektirir.");
        emit stateChanged();
        return;
    }

    if (!isValidPackageName(package)) {
        m_removeStatus = QStringLiteral("Geçersiz paket adı.");
        emit stateChanged();
        return;
    }

    if (isProtectedPackage(package)) {
        m_removeStatus = QStringLiteral("Kritik sistem veya Zethropol paketi (%1) kaldırılamaz.").arg(package);
        emit stateChanged();
        return;
    }

    bool knownPackage = false;
    for (const QVariant &item : m_applications) {
        const QVariantMap application = item.toMap();
        if (application.value(QStringLiteral("package")).toString() == package &&
            application.value(QStringLiteral("packageManager")).toString() == QStringLiteral("pacman") &&
            application.value(QStringLiteral("removable")).toBool() &&
            !application.value(QStringLiteral("protected")).toBool()) {
            knownPackage = true;
            break;
        }
    }

    if (!knownPackage) {
        m_removeStatus = QStringLiteral("Uygulama paketi güvenli kaldırma için uygun değil.");
        emit stateChanged();
        return;
    }

    // Pre-check dependency safety via read-only pacman -R --print before elevating privileges.
    QProcess precheck;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    precheck.setProcessEnvironment(env);
    precheck.setProcessChannelMode(QProcess::MergedChannels);
    precheck.start(QStringLiteral("/usr/bin/pacman"),
                   {QStringLiteral("-R"), QStringLiteral("--print"), QStringLiteral("--"), package});
    if (!precheck.waitForFinished(5000)
        || precheck.exitStatus() != QProcess::NormalExit
        || precheck.exitCode() != 0) {
        const QString err = QString::fromUtf8(precheck.readAllStandardOutput()).trimmed();
        m_removeStatus = err.isEmpty()
            ? QStringLiteral("Bağımlılık kontrolü başarısız; %1 kaldırılamaz.").arg(package)
            : QStringLiteral("Bağımlılık kontrolü engelledi: %1").arg(err);
        emit stateChanged();
        return;
    }

    m_removing = true;
    m_removeStatus = QStringLiteral("Removing %1...").arg(package);
    emit stateChanged();

    auto *process = new QProcess(this);
    process->setProcessEnvironment(env);

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_removeStatus = QStringLiteral("Could not start application removal.");
        m_removing = false;
        process->deleteLater();
        emit stateChanged();
    });

    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output =
            QString::fromUtf8(process->readAllStandardOutput()).trimmed();

        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            m_removeStatus = QStringLiteral("Application removed successfully.");
        } else if (output.contains(QStringLiteral("Authentication cancelled"), Qt::CaseInsensitive)
                   || output.contains(QStringLiteral("dismissed"), Qt::CaseInsensitive)
                   || exitCode == 126) {
            m_removeStatus = QStringLiteral("Application removal cancelled.");
        } else if (!output.isEmpty()) {
            m_removeStatus = output;
        } else {
            m_removeStatus = QStringLiteral("Application removal failed.");
        }

        m_removing = false;
        process->deleteLater();
        refresh();
        emit stateChanged();
    });

    QTimer::singleShot(125000, process, [process]() {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });

    // Safe invocation: explicit -- separator, never --noconfirm/--nodeps/--force.
    process->setProgram(QStringLiteral("/usr/bin/pkexec"));
    process->setArguments({
        QStringLiteral("/usr/bin/pacman"),
        QStringLiteral("-R"),
        QStringLiteral("--"),
        package
    });
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->start();
    if (process->waitForStarted(5000)) {
        // Explicit confirmation already verified (userConfirmed == true + Polkit auth).
        process->write("y\n");
        process->closeWriteChannel();
    }
}
