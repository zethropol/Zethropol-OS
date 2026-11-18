#include "UpdateScanner.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class UpdateScannerTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir directory;
    QByteArray previousPath;

    void script(const QString &name, const QByteArray &body)
    {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n" + body);
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    void configure(UpdateScanner &scanner, const QByteArray &text = {})
    {
        scanner.m_configurationPath = directory.filePath("arch-update.conf");
        QFile file(scanner.m_configurationPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text);
    }

    QVariantMap report(const UpdateScanner &scanner, int index)
    {
        return scanner.reports()[index].toMap();
    }

private slots:
    void init()
    {
        previousPath = qgetenv("PATH");
        qputenv("PATH", directory.path().toUtf8());
        script("checkupdates", "exit 2\n");
        script("paru", "exit 1\n");
        script("flatpak", "exit 0\n");
    }

    void cleanup() { qputenv("PATH", previousPath); }

    void startupScanAndExactReadOnlyCommands()
    {
        const auto log = directory.filePath("commands").toUtf8();
        QFile::remove(QString::fromUtf8(log));
        script("checkupdates", "printf 'official:%s:%s\\n' \"$*\" \"$CHECKUPDATES_DB\" >> '" + log
               + "'\nprintf 'pkg 1-1 -> 2-1\\n'\n");
        script("paru", "printf 'aur:%s\\n' \"$*\" >> '" + log
               + "'\nprintf 'browser 1-1 -> 2-1\\nignored 1 -> 2 [ignored]\\n'\n");
        script("flatpak", "printf 'flatpak:%s\\n' \"$*\" >> '" + log
               + "'\nprintf 'org.app.Editor\\tEditor\\t2.0\\norg.app.Runtime\\tRuntime\\n'\n");
        UpdateScanner scanner;
        configure(scanner);
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 0)["packages"].toList().size(), 1);
        QCOMPARE(report(scanner, 1)["packages"].toList().size(), 1);
        const auto flatpakPackages = report(scanner, 2)["packages"].toList();
        QCOMPARE(flatpakPackages.size(), 2);
        QCOMPARE(flatpakPackages[0].toMap()["newVersion"].toString(), "2.0");
        QCOMPARE(flatpakPackages[1].toMap()["identifier"].toString(), "org.app.Runtime");
        QCOMPARE(flatpakPackages[1].toMap()["name"].toString(), "Runtime");
        QCOMPARE(flatpakPackages[1].toMap()["newVersion"].toString(), "");
        QCOMPARE(report(scanner, 2)["state"].toString(), "complete");
        QVERIFY(!scanner.completedAt().isEmpty());
        QFile file(QString::fromUtf8(log));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto commands = file.readAll();
        QVERIFY(commands.contains("official:--nocolor:"));
        QVERIFY(commands.contains(scanner.m_database.path().toUtf8() + "/sync"));
        QVERIFY(commands.contains("aur:--color never -Qua"));
        QVERIFY(commands.contains("flatpak:remote-ls --updates --columns=application,name,version"));
        QVERIFY(!commands.contains("-Syu"));
        QVERIFY(!commands.contains("--appstream"));
    }

    void errorsAndMalformedResultsAreNotReportedAsUpToDate()
    {
        script("checkupdates", "printf 'network unavailable\\n' >&2\nexit 1\n");
        script("paru", "printf 'unexpected format\\n'\n");
        UpdateScanner scanner;
        configure(scanner);
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 0)["state"].toString(), "error");
        QVERIFY(report(scanner, 0)["detail"].toString().contains("network unavailable"));
        QCOMPARE(report(scanner, 1)["state"].toString(), "warning");
        QVERIFY(scanner.summary().contains("uyarılarla"));
    }

    void existingDisabledSourceConfigurationIsRespected()
    {
        script("paru", "exit 99\n");
        script("flatpak", "exit 99\n");
        UpdateScanner scanner;
        configure(scanner, "NoAUR\nNoFlatpak\n");
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 0)["state"].toString(), "complete");
        QCOMPARE(report(scanner, 1)["state"].toString(), "disabled");
        QCOMPARE(report(scanner, 2)["state"].toString(), "disabled");
    }

    void duplicateRequestsAreIgnoredAndRefreshClearsPreviousResults()
    {
        script("checkupdates", "printf 'pkg 1 -> 2\\n'\n");
        UpdateScanner scanner;
        configure(scanner);
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 0)["packages"].toList().size(), 1);
        script("checkupdates", "exit 2\n");
        scanner.scan();
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QVERIFY(report(scanner, 0)["packages"].toList().isEmpty());
        QVERIFY(scanner.summary().contains("güncelleme bulunamadı"));
    }

    void timeoutAndFailedStartCompleteTheScan()
    {
        script("checkupdates", "exec /usr/bin/sleep 10\n");
        QFile file(directory.filePath("paru"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/missing/interpreter\n");
        file.close();
        UpdateScanner scanner;
        configure(scanner, "UpdateCheckTimeout=1\n");
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 0)["state"].toString(), "error");
        QVERIFY(report(scanner, 0)["detail"].toString().contains("zaman aşımı"));
        QCOMPARE(report(scanner, 1)["state"].toString(), "error");
        QVERIFY(!scanner.busy());
    }

    void aurNoUpdatesExitCodeOneIsHandledAsCleanCompletion()
    {
        script("paru", "exit 1\n");
        UpdateScanner scanner;
        configure(scanner);
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 1)["state"].toString(), "complete");
        QCOMPARE(report(scanner, 1)["detail"].toString(), "Güncelleme bulunamadı.");
        QVERIFY(report(scanner, 1)["packages"].toList().isEmpty());
    }

    void aurRealErrorExitCodeOneDisplaysErrorMessage()
    {
        script("paru", "printf 'aur connection refused\\n' >&2\nexit 1\n");
        UpdateScanner scanner;
        configure(scanner);
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 1)["state"].toString(), "error");
        QVERIFY(report(scanner, 1)["detail"].toString().contains("aur connection refused"));
    }

    void flatpakTwoColumnRuntimeAndMalformedOutputsAreClassifiedAccurately()
    {
        script("flatpak", "printf 'org.kde.Platform\\tKDE Application Platform\\n'\n");
        UpdateScanner scanner;
        configure(scanner);
        QSignalSpy finished(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(report(scanner, 2)["state"].toString(), "complete");
        QCOMPARE(report(scanner, 2)["detail"].toString(), "1 güncelleme bulundu.");
        const auto packages = report(scanner, 2)["packages"].toList();
        QCOMPARE(packages.size(), 1);
        QCOMPARE(packages[0].toMap()["identifier"].toString(), "org.kde.Platform");
        QCOMPARE(packages[0].toMap()["name"].toString(), "KDE Application Platform");
        QCOMPARE(packages[0].toMap()["newVersion"].toString(), "");

        script("flatpak", "exit 0\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 5000);
        QCOMPARE(report(scanner, 2)["state"].toString(), "complete");
        QCOMPARE(report(scanner, 2)["detail"].toString(), "Güncelleme bulunamadı.");
        QVERIFY(report(scanner, 2)["packages"].toList().isEmpty());

        script("flatpak", "printf 'single-column\\n'\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, 5000);
        QCOMPARE(report(scanner, 2)["state"].toString(), "warning");
        QVERIFY(report(scanner, 2)["detail"].toString().contains("eksik olabilir"));
        QVERIFY(report(scanner, 2)["packages"].toList().isEmpty());

        script("flatpak", "printf '\\tMissing Id\\t1.0\\n'\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 4, 5000);
        QCOMPARE(report(scanner, 2)["state"].toString(), "warning");
        QVERIFY(report(scanner, 2)["packages"].toList().isEmpty());

        script("flatpak", "printf 'org.app.Extra\\tExtra\\t1.0\\textra\\n'\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 5, 5000);
        QCOMPARE(report(scanner, 2)["state"].toString(), "warning");
        QVERIFY(report(scanner, 2)["packages"].toList().isEmpty());

        script("flatpak", "printf 'error: Unable to load summary from remote flathub\\n' >&2\nexit 1\n");
        scanner.scan();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 6, 5000);
        QCOMPARE(report(scanner, 2)["state"].toString(), "error");
        QVERIFY(report(scanner, 2)["detail"].toString().contains("Unable to load summary"));
    }
};

QTEST_GUILESS_MAIN(UpdateScannerTest)
#include "UpdateScannerTest.moc"
