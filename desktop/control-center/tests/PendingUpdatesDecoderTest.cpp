#include "core/replacement_plan_decoder.h"
#include "core/zethropol_update_client.h"
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusServer>
#include <QDBusVirtualObject>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <future>
#include <chrono>

struct PlanAddition { QString name, current, next, repository; qulonglong bytes = 1; QString filename, checksum, metadata; };
Q_DECLARE_METATYPE(PlanAddition)
QDBusArgument &operator<<(QDBusArgument &a, const PlanAddition &p) {
    a.beginStructure(); a << p.name << p.current << p.next << p.repository << p.bytes << p.filename << p.checksum << p.metadata; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, PlanAddition &p) {
    a.beginStructure(); a >> p.name >> p.current >> p.next >> p.repository >> p.bytes >> p.filename >> p.checksum >> p.metadata; a.endStructure(); return a;
}
class PendingReplyFixture : public QDBusVirtualObject {
public:
    QList<PlanAddition> packages;
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        return connection.send(message.createReply({QVariant::fromValue(packages)}));
    }
};

class PendingUpdatesDecoderTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qDBusRegisterMetaType<PlanAddition>();
        qDBusRegisterMetaType<QList<PlanAddition>>();
    }
    void pendingContract_data() {
        QTest::addColumn<QString>("field");
        QTest::newRow("listing-without-plan-snapshot") << QString();
        QTest::newRow("listing-with-plan-snapshot") << QStringLiteral("valid_metadata");
        for (const char *field : {"package_name", "current_version", "new_version", "repository", "filename", "sha256", "repository_metadata_sha256"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void pendingContract() {
        QFETCH(QString, field);
        PlanAddition package{QStringLiteral("btrfs-progs"), QStringLiteral("7.1-1"),
            QStringLiteral("7.3-1"), QStringLiteral("core"), 1350090,
            QStringLiteral("btrfs-progs-7.3-1-x86_64.pkg.tar.zst"),
            QString(64, QLatin1Char('a')), QString()};
        PendingReplyFixture fixture;
        fixture.packages.append(package);
        if (field == QStringLiteral("valid_metadata")) fixture.packages[0].metadata = QString(64, QLatin1Char('b'));
        const bool valid = field.isEmpty() || field == QStringLiteral("valid_metadata");
        if (!valid) {
            if (field == "package_name") package.name.clear();
            if (field == "current_version") package.current.clear();
            if (field == "new_version") package.next.clear();
            if (field == "repository") package.repository.clear();
            if (field == "filename") package.filename = "../unsafe.pkg.tar.zst";
            if (field == "sha256") package.checksum.clear();
            if (field == "repository_metadata_sha256") package.metadata = "invalid-digest";
            fixture.packages.append(package);
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDBusServer server(QStringLiteral("unix:tmpdir=") + directory.path());
        QVERIFY2(server.isConnected(), qPrintable(server.lastError().message()));
        connect(&server, &QDBusServer::newConnection, &fixture, [&fixture](QDBusConnection connection) {
            connection.registerVirtualObject(QStringLiteral("/updates"), &fixture);
        });
        const QString address = server.address();
        auto future = std::async(std::launch::async, [address] {
            auto connection = QDBusConnection::connectToPeer(address, QStringLiteral("pending-test"));
            auto request = QDBusMessage::createMethodCall(QString(), QStringLiteral("/updates"),
                QStringLiteral("org.zethropol.Update.v1"), QStringLiteral("ListPendingUpdates"));
            auto reply = connection.call(request, QDBus::Block, 5000);
            QDBusConnection::disconnectFromPeer(QStringLiteral("pending-test"));
            return reply;
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
        const auto reply = future.get();
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.signature(), QStringLiteral("a(sssstsss)"));
        const QVariantList sentinel{QVariantMap{{"preserved", true}}};
        QVariantList result = sentinel;
        QString error;
        QCOMPARE(ZethropolUpdateWire::decodePendingPackages(reply.arguments().first(), &result, &error), valid);
        if (!valid) {
            QCOMPARE(result, sentinel); // Never return a misleading partial list.
            QVERIFY2(error.contains(field), qPrintable(error));
            QVERIFY(error.contains(QStringLiteral("2")));
        } else {
            QCOMPARE(result.size(), 1);
            const QVariantMap decoded = result.first().toMap();
            QCOMPARE(decoded.value("package_name").toString(), package.name);
            QCOMPARE(decoded.value("current_version").toString(), package.current);
            QCOMPARE(decoded.value("new_version").toString(), package.next);
            QCOMPARE(decoded.value("repository").toString(), package.repository);
            QCOMPARE(decoded.value("download_size").toULongLong(), package.bytes);
            QCOMPARE(decoded.value("sha256").toString(), package.checksum);
            QCOMPARE(decoded.value("repository_metadata_sha256").toString(), fixture.packages.first().metadata);
            QVariantList plan = sentinel;
            QCOMPARE(ZethropolUpdateWire::decodePlanPackages(reply.arguments().first(), &plan, &error), !field.isEmpty());
            if (field.isEmpty()) {
                QCOMPARE(plan, sentinel);
                QVERIFY(error.contains(QStringLiteral("repository_metadata_sha256")));
            }
        }
    }
    void livePreviewPlan() {
        if (!qEnvironmentVariableIsSet("ZETHROPOL_TEST_LIVE_UPDATE"))
            QSKIP("Explicit opt-in required for production PreviewPlan; never triggers installation");
        ZethropolUpdateClient client;
        QSignalSpy finished(&client, &ZethropolUpdateClient::transactionPreviewFinished);
        QSignalSpy effects(&client, &ZethropolUpdateClient::transactionPlanReceived);
        client.requestTransactionPreview();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 95000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        QCOMPARE(effects.count(), 1);
        const auto packages = finished.first().at(1).toList();
        const auto removals = effects.first().at(0).toList();
        QCOMPARE(effects.first().at(1).toString().size(), 64);
        qInfo() << "Validated production plan:" << packages.size() << "packages; removals:" << removals.size()
                << "hash:" << effects.first().at(1).toString();
        for (const auto &entry : packages) {
            const auto package = entry.toMap();
            QCOMPARE(package.value("sha256").toString().size(), 64);
            QCOMPARE(package.value("repository_metadata_sha256").toString().size(), 64);
        }
    }
    void livePendingUpdates() {
        if (!qEnvironmentVariableIsSet("ZETHROPOL_TEST_LIVE_UPDATE"))
            QSKIP("Explicit opt-in required for read-only production D-Bus verification");
        ZethropolUpdateClient client;
        QSignalSpy finished(&client, &ZethropolUpdateClient::refreshFinished);
        client.refreshUpdateState();
        QCOMPARE(finished.count(), 1);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        const auto packages = client.pendingUpdates();
        for (const auto &entry : packages) {
            const auto package = entry.toMap();
            QVERIFY(!package.value("package_name").toString().isEmpty());
            QVERIFY(!package.value("current_version").toString().isEmpty());
            QVERIFY(!package.value("new_version").toString().isEmpty());
            QVERIFY(!package.value("repository").toString().isEmpty());
        }
        qInfo() << "Production D-Bus decoded packages:" << packages.size();
        if (!packages.isEmpty()) {
            const auto first = packages.first().toMap();
            qInfo() << first.value("package_name") << first.value("current_version")
                    << first.value("new_version") << first.value("repository");
        }
    }
};
QTEST_GUILESS_MAIN(PendingUpdatesDecoderTest)
#include "PendingUpdatesDecoderTest.moc"
