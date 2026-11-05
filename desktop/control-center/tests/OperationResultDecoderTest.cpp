#include "core/operation_result_decoder.h"
#include "core/zethropol_update_client.h"
#include <QDBusServer>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <future>
#include <chrono>

class ResultFixture : public QDBusVirtualObject {
public:
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        const QString mode = message.member();
        bool success = mode != "Rejected" && mode != "WorkerStartFailed";
        QString code = success ? "started" : mode == "Rejected" ? "no_updates" : "worker_start_failed";
        QString id = mode == "Rejected" ? QString() : QStringLiteral("1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e");
        QVariantMap data{{"detail", QStringLiteral("safe detail")}};
        QVariantList fields{success, code, QStringLiteral("service explanation"), id, data};
        if (mode == "Nested") {
            QDBusArgument wire;
            wire.beginStructure(); wire << success << code << QStringLiteral("service explanation") << id << data; wire.endStructure();
            fields = {QVariant::fromValue(wire)};
        }
        if (mode == "Truncated") fields = {true};
        if (mode == "WrongType") fields[0] = QStringLiteral("true");
        if (mode == "MissingId") fields[3] = QString();
        if (mode == "WrongMap") fields[4] = QStringLiteral("not-a-map");
        return connection.send(message.createReply(fields));
    }
};

class OperationResultDecoderTest : public QObject {
    Q_OBJECT
    QTemporaryDir busGuard;
private slots:
    void initTestCase() {
        QVERIFY(busGuard.isValid());
        // Client objects in these tests must never contact the real service.
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", ("unix:path=" + busGuard.filePath("absent")).toUtf8());
    }
    void operationReply_data() {
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"Flat", "Nested", "Rejected", "WorkerStartFailed", "Truncated", "WrongType", "MissingId", "WrongMap"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void operationReply() {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QDBusServer server("unix:tmpdir=" + directory.path());
        QVERIFY2(server.isConnected(), qPrintable(server.lastError().message()));
        ResultFixture fixture;
        connect(&server, &QDBusServer::newConnection, &fixture, [&fixture](QDBusConnection connection) {
            connection.registerVirtualObject("/result", &fixture);
        });
        const auto address = server.address();
        auto future = std::async(std::launch::async, [address, mode] {
            auto bus = QDBusConnection::connectToPeer(address, "result-fixture");
            const auto call = QDBusMessage::createMethodCall(QString(), "/result", "org.zethropol.Test", mode);
            auto reply = bus.call(call, QDBus::Block, 5000);
            QDBusConnection::disconnectFromPeer("result-fixture");
            return reply;
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
        const auto reply = future.get();
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        const bool valid = mode == "Flat" || mode == "Nested" || mode == "Rejected" || mode == "WorkerStartFailed";
        QVariantMap result{{"preserved", true}};
        QString error;
        QCOMPARE(ZethropolUpdateWire::decodeOperationResult(reply.arguments(), &result, &error), valid);
        if (!valid) {
            QCOMPARE(result, (QVariantMap{{"preserved", true}}));
            QVERIFY(error.contains("invalid_trigger_reply"));
        } else {
            QCOMPARE(reply.signature(), mode == "Nested" ? QStringLiteral("(bsssa{sv})") : QStringLiteral("bsssa{sv}"));
            QCOMPARE(result.value("message").toString(), QStringLiteral("service explanation"));
            QCOMPARE(result.value("data").toMap().value("detail").toString(), QStringLiteral("safe detail"));
        }
        ZethropolUpdateClient client;
        QSignalSpy started(&client, &ZethropolUpdateClient::operationInitiated);
        QSignalSpy failed(&client, &ZethropolUpdateClient::triggerUpdateFailed);
        QSignalSpy uncertain(&client, &ZethropolUpdateClient::triggerUpdateUncertain);
        QVERIFY(QMetaObject::invokeMethod(&client, "handleTriggerReply", Q_ARG(QDBusMessage, reply)));
        const bool accepted = mode == "Flat" || mode == "Nested";
        QCOMPARE(started.size(), accepted ? 1 : 0);
        QCOMPARE(failed.size(), mode == "Rejected" ? 1 : 0);
        QCOMPARE(uncertain.size(), !accepted && mode != "Rejected" ? 1 : 0);
        if (mode == "WorkerStartFailed") {
            QVERIFY(uncertain.first()[0].toString().contains("worker_start_failed"));
            QCOMPARE(uncertain.first()[1].toString(), QStringLiteral("1096233b-f5bb-4d2b-a9fb-1ba5cdc85c5e"));
        }
    }
    void dbusFailuresPreserveCodesAndUncertainty() {
        ZethropolUpdateClient client;
        QSignalSpy failed(&client, &ZethropolUpdateClient::triggerUpdateFailed);
        QSignalSpy uncertain(&client, &ZethropolUpdateClient::triggerUpdateUncertain);
        const auto request = QDBusMessage::createMethodCall("org.zethropol.Test", "/result", "org.zethropol.Test", "Fixture");
        const auto expired = request.createErrorReply("org.freedesktop.DBus.Error.Failed", "Update broker rejected the request: Expired");
        QVERIFY(QMetaObject::invokeMethod(&client, "handleTriggerReply", Q_ARG(QDBusMessage, expired)));
        QCOMPARE(failed.size(), 1);
        QVERIFY(failed.first()[0].toString().contains("org.freedesktop.DBus.Error.Failed"));
        QVERIFY(failed.first()[0].toString().contains("Expired"));
        const auto lost = request.createErrorReply("org.freedesktop.DBus.Error.NoReply", "Timed out");
        QVERIFY(QMetaObject::invokeMethod(&client, "handleTriggerReply", Q_ARG(QDBusMessage, lost)));
        QCOMPARE(uncertain.size(), 1);
        QVERIFY(uncertain.first()[0].toString().contains("NoReply"));
    }
};
QTEST_GUILESS_MAIN(OperationResultDecoderTest)
#include "OperationResultDecoderTest.moc"
