#include "SecurityService.h"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>

SecurityService::SecurityService(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QString SecurityService::firewall() const
{
    return m_firewall;
}

QString SecurityService::secureBoot() const
{
    return m_secureBoot;
}

QString SecurityService::kernelLockdown() const
{
    return m_kernelLockdown;
}

QStringList SecurityService::lsm() const
{
    return m_lsm;
}

QString SecurityService::appArmor() const
{
    return m_appArmor;
}

int SecurityService::failedServices() const
{
    return m_failedServices;
}

void SecurityService::refresh()
{
    QString firewallState = QStringLiteral("unknown");

    QProcess firewallProcess;
    firewallProcess.start(QStringLiteral("/usr/bin/systemctl"),
                          {QStringLiteral("is-active"),
                           QStringLiteral("ufw")});
    firewallProcess.waitForFinished(2000);

    if (firewallProcess.exitCode() == 0 &&
        QString::fromLocal8Bit(firewallProcess.readAllStandardOutput()).trimmed() == QStringLiteral("active")) {
        firewallState = QStringLiteral("active");
    } else {
        firewallProcess.start(QStringLiteral("/usr/bin/systemctl"),
                              {QStringLiteral("is-active"),
                               QStringLiteral("firewalld")});
        firewallProcess.waitForFinished(2000);

        if (firewallProcess.exitCode() == 0 &&
            QString::fromLocal8Bit(firewallProcess.readAllStandardOutput()).trimmed() == QStringLiteral("active")) {
            firewallState = QStringLiteral("active");
        } else if (firewallProcess.exitStatus() == QProcess::NormalExit) {
            firewallState = QStringLiteral("inactive");
        }
    }

    QString secureBootState = QStringLiteral("unknown");

    const QString efivarsPath = QStringLiteral("/sys/firmware/efi/efivars");
    if (QFile::exists(efivarsPath)) {
        QProcess secureBootProcess;
        secureBootProcess.start(QStringLiteral("/usr/bin/bootctl"),
                                 {QStringLiteral("status")});
        secureBootProcess.waitForFinished(2000);

        const QString output = QString::fromLocal8Bit(
            secureBootProcess.readAllStandardOutput());

        if (output.contains(QStringLiteral("Secure Boot: enabled"),
                            Qt::CaseInsensitive)) {
            secureBootState = QStringLiteral("enabled");
        } else if (output.contains(QStringLiteral("Secure Boot: disabled"),
                                   Qt::CaseInsensitive)) {
            secureBootState = QStringLiteral("disabled");
        }
    } else {
        secureBootState = QStringLiteral("unsupported");
    }

    QString lockdownState = QStringLiteral("unknown");

    QFile lockdownFile(QStringLiteral("/sys/kernel/security/lockdown"));
    if (lockdownFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString lockdown =
            QString::fromUtf8(lockdownFile.readAll()).trimmed();

        const int begin = lockdown.indexOf(QLatin1Char(static_cast<char>(91)));
        const int end = lockdown.indexOf(QLatin1Char(static_cast<char>(93)));

        if (begin >= 0 && end > begin) {
            lockdownState =
                lockdown.mid(begin + 1, end - begin - 1).trimmed();

            if (lockdownState.isEmpty())
                lockdownState = QStringLiteral("none");
        }
    }

    QStringList lsmList;

    QFile lsmFile(QStringLiteral("/sys/kernel/security/lsm"));
    if (lsmFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        lsmList = QString::fromUtf8(lsmFile.readAll())
                      .trimmed()
                      .split(QLatin1Char(static_cast<char>(44)),
                             Qt::SkipEmptyParts);

        for (QString &entry : lsmList)
            entry = entry.trimmed();
    }

    QString appArmorState = QStringLiteral("unknown");

    QProcess appArmorProcess;
    appArmorProcess.start(QStringLiteral("/usr/bin/systemctl"),
                          {QStringLiteral("is-active"),
                           QStringLiteral("apparmor")});
    appArmorProcess.waitForFinished(2000);

    const QString appArmorOutput =
        QString::fromLocal8Bit(
            appArmorProcess.readAllStandardOutput()).trimmed();

    if (appArmorOutput == QStringLiteral("active"))
        appArmorState = QStringLiteral("active");
    else if (appArmorOutput == QStringLiteral("inactive"))
        appArmorState = QStringLiteral("inactive");
    else if (appArmorOutput == QStringLiteral("unknown"))
        appArmorState = QStringLiteral("not-installed");

    int failedServiceCount = 0;

    QProcess failedServicesProcess;
    failedServicesProcess.start(
        QStringLiteral("/usr/bin/systemctl"),
        {QStringLiteral("--failed"),
         QStringLiteral("--no-legend"),
         QStringLiteral("--plain")});

    failedServicesProcess.waitForFinished(3000);

    if (failedServicesProcess.exitStatus() == QProcess::NormalExit) {
        const QString output =
            QString::fromLocal8Bit(
                failedServicesProcess.readAllStandardOutput()).trimmed();

        if (!output.isEmpty()) {
            failedServiceCount =
                output.split(QLatin1Char(static_cast<char>(10)), Qt::SkipEmptyParts)
                    .size();
        }
    }

    m_firewall = firewallState;
    m_secureBoot = secureBootState;
    m_kernelLockdown = lockdownState;
    m_lsm = lsmList;
    m_appArmor = appArmorState;
    m_failedServices = failedServiceCount;

    emit stateChanged();
}
