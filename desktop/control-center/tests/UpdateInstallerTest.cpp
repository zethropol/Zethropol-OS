#include "UpdateInstaller.h"
#include "UpdateScanner.h"

#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class UpdateInstallerTest : public QObject
{
    Q_OBJECT
    QTemporaryDir directory;
    QByteArray previousPath;
    QByteArray previousConfig;
    bool hadConfig = false;

    void script(const QString &name, const QByteArray &body)
    {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n" + body);
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    void setup(UpdateInstaller &installer) { installer.m_updater = directory.filePath("updater"); }

private slots:
    void init()
    {
        previousPath = qgetenv("PATH");
        previousConfig = qgetenv("XDG_CONFIG_HOME");
        hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("PATH", directory.path().toUtf8() + ':' + previousPath);
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        script("checkupdates", "printf 'pkg 1 -> 2\\n'\n");
        script("paru", "exit 0\n");
        script("flatpak", "exit 0\n");
        script("updater", "printf 'Paket kontrolü tamamlandı.\\n'\nexit 0\n");
    }

    void cleanup()
    {
        qputenv("PATH", previousPath);
        if (hadConfig) qputenv("XDG_CONFIG_HOME", previousConfig);
        else qunsetenv("XDG_CONFIG_HOME");
    }

    void embeddedSessionProvidesATtyAndProtectsEnteredPasswords()
    {
        script("updater", R"SH(
[ -t 0 ] && [ -t 1 ] || exit 90
printf '\033[31mParola:\033[0m '
IFS= read -r password
[ "$password" = 'test-private-password' ] || exit 91
printf '\nDevam [y/N]: '
IFS= read -r answer
[ "$answer" = y ] || exit 92
printf '\nPaket kontrolü tamamlandı.\n'
)SH");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QVERIFY(installer.busy());
        installer.start();
        scanner.scan();
        QCOMPARE(scanned.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Parola:"), 5000);
        installer.sendInput("test-private-password");
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Devam [y/N]:"), 5000);
        installer.sendInput("y");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QCOMPARE(installer.status(), "finished");
        QVERIFY(installer.output().contains("Paket kontrolü tamamlandı."));
        QVERIFY(!installer.output().contains("test-private-password"));
        QVERIFY(!installer.output().contains(QChar(0x1b)));
        QVERIFY(scanner.hasUpdates()); // A zero exit is not proof that all sources installed.
    }

    void singleKeyDoesNotAnswerTheNextLinePrompt()
    {
        script("updater", R"SH(
/usr/bin/stty -icanon min 1 time 0
printf 'Tek tuş: '
key=$(/usr/bin/dd bs=1 count=1 status=none)
[ "$key" = q ] || exit 94
/usr/bin/stty icanon
printf '\nSatır yanıtı: '
IFS= read -r answer
[ "$answer" = confirm ] || exit 95
)SH");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Tek tuş:"), 5000);
        installer.sendKey("q");
        QTRY_VERIFY_WITH_TIMEOUT(installer.output().contains("Satır yanıtı:"), 5000);
        QCOMPARE(finished.count(), 0);
        installer.sendInput("confirm");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(installer.status(), "finished");
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
    }

    void cancellationAndFailureRemainVisible_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("state");
        QTest::newRow("cancelled") << 4 << QString("cancelled");
        QTest::newRow("official-failure") << 5 << QString("error");
        QTest::newRow("interrupted") << 130 << QString("error");
    }

    void cancellationAndFailureRemainVisible()
    {
        QFETCH(int, code);
        QFETCH(QString, state);
        script("updater", "exit " + QByteArray::number(code) + "\n");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QCOMPARE(installer.status(), state);
        QVERIFY(!installer.busy());
    }

    void launchFailureDoesNotLockScanning()
    {
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        installer.m_sessionTool = "/missing/program";
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        installer.start();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QCOMPARE(installer.status(), "error");
        QVERIFY(!installer.busy());
    }

    void qmlRequiresConfirmationAndAcceptsInputInsideThePage()
    {
        script("updater", "printf 'Yanıt bekleniyor: '\nIFS= read -r answer\n[ \"$answer\" = continue ] || exit 93\n");
        UpdateScanner scanner;
        QSignalSpy scanned(&scanner, &UpdateScanner::scanFinished);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 1, 5000);
        UpdateInstaller installer(&scanner);
        setup(installer);
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(UPDATE_REPORT_QML)));
        std::unique_ptr<QObject> root(component.createWithInitialProperties({
            {"scanner", QVariant::fromValue<QObject *>(&scanner)},
            {"installer", QVariant::fromValue<QObject *>(&installer)}}));
        QVERIFY2(root, qPrintable(component.errorString()));
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        item->setSize(QSizeF(1000, 760));
        window.resize(1000, 760);
        window.show();
        auto *start = root->findChild<QObject *>("startUpdateInstallation");
        auto *confirmation = root->findChild<QObject *>("updateInstallationConfirmation");
        auto *cancel = root->findChild<QObject *>("cancelUpdateInstallation");
        auto *confirm = root->findChild<QObject *>("confirmUpdateInstallation");
        auto *input = root->findChild<QObject *>("updateInstallationInput");
        auto *send = root->findChild<QObject *>("sendUpdateInstallationInput");
        auto *output = root->findChild<QObject *>("updateInstallationOutput");
        QVERIFY(start && confirmation && cancel && confirm && input && send && output);
        QCOMPARE(installer.status(), "idle");
        QVERIFY(!confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QCOMPARE(installer.status(), "idle");
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QTRY_VERIFY(!confirmation->property("visible").toBool());
        QCOMPARE(installer.status(), "idle");
        QVERIFY(QMetaObject::invokeMethod(start, "clicked"));
        QTRY_VERIFY(confirmation->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
        QVERIFY(installer.busy());
        QVERIFY(!start->property("enabled").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(output->property("text").toString().contains("Yanıt bekleniyor:"), 5000);
        QCOMPARE(input->property("echoMode").toInt(), 2); // Password input.
        input->setProperty("text", "continue");
        QSignalSpy finished(&installer, &UpdateInstaller::sessionFinished);
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        QVERIFY(input->property("text").toString().isEmpty());
        input->setProperty("text", "unsubmitted-private-input");
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(scanned.count(), 2, 5000);
        QVERIFY(!installer.output().contains("continue"));
        QVERIFY(!installer.output().contains("unsubmitted-private-input"));
        QVERIFY(input->property("text").toString().isEmpty());
        QCOMPARE(installer.status(), "finished");
    }
};

QTEST_MAIN(UpdateInstallerTest)
#include "UpdateInstallerTest.moc"
