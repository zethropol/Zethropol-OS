#include "core/update_state_decoder.h"
#include "core/replacement_plan_decoder.h"
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QTest>
#include <QDBusServer>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusVirtualObject>
#include <QTemporaryDir>
#include <future>
#include <chrono>

struct PlanAddition { QString name, current, next, repository; qulonglong bytes = 1; QString filename, checksum, metadata; };
struct PlanRemoval { QString name, version, reason, replacement; };
Q_DECLARE_METATYPE(PlanAddition)
Q_DECLARE_METATYPE(PlanRemoval)
QDBusArgument &operator<<(QDBusArgument &a, const PlanAddition &p) {
    a.beginStructure(); a << p.name << p.current << p.next << p.repository << p.bytes << p.filename << p.checksum << p.metadata; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, PlanAddition &p) {
    a.beginStructure(); a >> p.name >> p.current >> p.next >> p.repository >> p.bytes >> p.filename >> p.checksum >> p.metadata; a.endStructure(); return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const PlanRemoval &p) {
    a.beginStructure(); a << p.name << p.version << p.reason << p.replacement; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, PlanRemoval &p) {
    a.beginStructure(); a >> p.name >> p.version >> p.reason >> p.replacement; a.endStructure(); return a;
}

class StateReplyFixture : public QDBusVirtualObject {
public:
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        QVariantList fields{true, false, QStringLiteral("Just now"), uint(10)};
        if (message.member() == QStringLiteral("Nested")) {
            QDBusArgument structure;
            structure.beginStructure();
            structure << true << false << QStringLiteral("Just now") << uint(10);
            structure.endStructure();
            fields = {QVariant::fromValue(structure)};
        }
        if (message.member() == QStringLiteral("StringMap")) {
            QDBusArgument map;
            map.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QString>());
            map.beginMapEntry();
            map << QStringLiteral("status") << QStringLiteral("not-found");
            map.endMapEntry();
            map.endMap();
            fields = {QVariant::fromValue(map)};
        }
        if (message.member() == QStringLiteral("EvidenceMap")) {
            QDBusArgument map;
            map.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QString>());
            const QMap<QString, QString> evidence{
                {QStringLiteral("status"), QStringLiteral("ok_with_warnings")},
                {QStringLiteral("package_commit"), QStringLiteral("committed")},
                {QStringLiteral("hooks"), QStringLiteral("unknown")},
                {QStringLiteral("boot_preparation"), QStringLiteral("unknown")},
                {QStringLiteral("pre_snapshot"), QStringLiteral("301")},
                {QStringLiteral("post_snapshot"), QStringLiteral("302")},
                {QStringLiteral("kernel_artifacts"), QStringLiteral("verified")},
                {QStringLiteral("dkms_artifacts"), QStringLiteral("failed")},
                {QStringLiteral("initramfs_artifacts"), QStringLiteral("unknown")},
                {QStringLiteral("limine_artifacts"), QStringLiteral("verified")},
                {QStringLiteral("new_kernel_boot"), QStringLiteral("unknown")},
                {QStringLiteral("running_kernel"), QStringLiteral("old-running")},
                {QStringLiteral("installed_kernels"), QStringLiteral("new-installed")},
                {QStringLiteral("updated_kernels"), QStringLiteral("new-installed")},
                {QStringLiteral("boot_details"), QStringLiteral("line one\nline two")},
                {QStringLiteral("hook_details"), QStringLiteral("returned (exit status unavailable)")},
                {QStringLiteral("cleanup_status"), QStringLiteral("warnings")},
                {QStringLiteral("cleanup_removed_files"), QStringLiteral("3")},
                {QStringLiteral("cleanup_removed_bytes"), QStringLiteral("8000")},
                {QStringLiteral("cleanup_reclaimed_bytes"), QStringLiteral("4096")},
                {QStringLiteral("cleanup_details"), QStringLiteral("Busy archive retained")},
            };
            for (auto it = evidence.cbegin(); it != evidence.cend(); ++it) {
                map.beginMapEntry();
                map << it.key() << it.value();
                map.endMapEntry();
            }
            map.endMap();
            fields = {QVariant::fromValue(map)};
        }
        if (message.member().startsWith(QStringLiteral("Plan"))) {
            const QList<PlanAddition> adds{{QStringLiteral("new-demo"), QStringLiteral("not installed (new dependency)"), QStringLiteral("2-1"), QStringLiteral("core"), 42, QStringLiteral("new-demo.pkg.tar.zst"), QString(64, QLatin1Char('b')), QString(64, QLatin1Char('c'))}};
            QList<PlanRemoval> removals{{QStringLiteral("old-demo"), QStringLiteral("1-1"), QStringLiteral("replacement"), QStringLiteral("new-demo")}};
            QString hash(64, QLatin1Char('a'));
            if (message.member() == QStringLiteral("PlanConflict")) removals[0].reason = QStringLiteral("conflict");
            if (message.member() == QStringLiteral("PlanMissingTarget")) removals[0].replacement = QStringLiteral("absent");
            if (message.member() == QStringLiteral("PlanDuplicate")) removals.append(removals.first());
            if (message.member() == QStringLiteral("PlanHashless")) hash.clear();
            if (message.member() == QStringLiteral("PlanNoRemovals")) removals.clear();
            fields = {QVariant::fromValue(adds), QVariant::fromValue(removals), hash};
        }
        return connection.send(message.createReply(fields));
    }
};

class UpdateStateDecoderTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qDBusRegisterMetaType<PlanAddition>();
        qDBusRegisterMetaType<PlanRemoval>();
        qDBusRegisterMetaType<QList<PlanAddition>>();
        qDBusRegisterMetaType<QList<PlanRemoval>>();
    }
    void deployedFlatReplyPreservesAllFields()
    {
        // Same four QVariant arguments as the real bbsu system-bus reply.
        const auto call = QDBusMessage::createMethodCall("org.zethropol.Update",
            "/org/zethropol/Update", "org.zethropol.Update.v1", "GetUpdateState");
        const auto reply = call.createReply({true, false, QStringLiteral("Just now"), uint(10)});
        QVariantMap state;
        QString error;
        QVERIFY(ZethropolUpdateWire::decodeState(reply.arguments(), &state, &error));
        QCOMPARE(state.size(), 4);
        QCOMPARE(state.value("is_checking").toBool(), true);
        QCOMPARE(state.value("is_updating").toBool(), false);
        QCOMPARE(state.value("last_check_time").toString(), QStringLiteral("Just now"));
        QCOMPARE(state.value("pending_updates_count").toUInt(), uint(10));
    }
    void realTransportSupportsFlatAndNested_data()
    {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("signature");
        for (const char *method : {"PlanValid", "PlanNoRemovals", "PlanConflict", "PlanMissingTarget", "PlanDuplicate", "PlanHashless"})
            QTest::newRow(method) << QString::fromLatin1(method) << QStringLiteral("a(sssstsss)a(ssss)s");
        QTest::newRow("deployed-bbsu") << QStringLiteral("Flat") << QStringLiteral("bbsu");
        QTest::newRow("wrapped-bbsu") << QStringLiteral("Nested") << QStringLiteral("(bbsu)");
        QTest::newRow("operation-string-map") << QStringLiteral("StringMap") << QStringLiteral("a{ss}");
        QTest::newRow("execution-evidence-map") << QStringLiteral("EvidenceMap") << QStringLiteral("a{ss}");
    }
    void realTransportSupportsFlatAndNested()
    {
        QFETCH(QString, method);
        QFETCH(QString, signature);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QDBusServer server(QStringLiteral("unix:tmpdir=") + directory.path());
        QVERIFY2(server.isConnected(), qPrintable(server.lastError().message()));
        StateReplyFixture fixture;
        QObject::connect(&server, &QDBusServer::newConnection,
                         &fixture, [&fixture](QDBusConnection connection) {
            connection.registerVirtualObject(QStringLiteral("/state"), &fixture);
        });
        const QString address = server.address();
        auto future = std::async(std::launch::async, [address, method] {
            const auto connection = QDBusConnection::connectToPeer(address, QStringLiteral("state-test"));
            const auto request = QDBusMessage::createMethodCall(QString(), QStringLiteral("/state"),
                QStringLiteral("org.zethropol.Update.Test"), method);
            auto reply = connection.call(request, QDBus::Block, 5000);
            QDBusConnection::disconnectFromPeer(QStringLiteral("state-test"));
            return reply;
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 10000);
        const auto reply = future.get();
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.signature(), signature);
        if (method.startsWith(QStringLiteral("Plan"))) {
            QVariantList packages, removals;
            QString hash, error;
            const bool valid = ZethropolUpdateWire::decodePlan(reply.arguments(), &packages, &removals, &hash, &error);
            const bool expected = method == QStringLiteral("PlanValid") || method == QStringLiteral("PlanNoRemovals");
            QCOMPARE(valid, expected);
            if (expected) {
                QCOMPARE(packages.size(), 1);
                QCOMPARE(removals.size(), method == QStringLiteral("PlanValid") ? 1 : 0);
                QCOMPARE(hash, QString(64, QLatin1Char('a')));
                if (!removals.isEmpty()) {
                    QCOMPARE(removals.first().toMap().value(QStringLiteral("package_name")).toString(), QStringLiteral("old-demo"));
                    QCOMPARE(removals.first().toMap().value(QStringLiteral("replacement")).toString(), QStringLiteral("new-demo"));
                }
            } else {
                QVERIFY(!error.isEmpty());
                QVERIFY(packages.isEmpty());
                QVERIFY(removals.isEmpty());
                QVERIFY(hash.isEmpty());
            }
            return;
        }
        if (method == QStringLiteral("StringMap")) {
            const auto map = ZethropolUpdateWire::decodeMap(reply.arguments().first());
            QCOMPARE(map.value("status").toString(), QStringLiteral("not-found"));
            return;
        }
        if (method == QStringLiteral("EvidenceMap")) {
            const auto map = ZethropolUpdateWire::decodeMap(reply.arguments().first());
            QCOMPARE(map.value("status").toString(), QStringLiteral("ok_with_warnings"));
            QCOMPARE(map.value("package_commit").toString(), QStringLiteral("committed"));
            QCOMPARE(map.value("hooks").toString(), QStringLiteral("unknown"));
            QCOMPARE(map.value("boot_preparation").toString(), QStringLiteral("unknown"));
            QCOMPARE(map.value("pre_snapshot").toString(), QStringLiteral("301"));
            QCOMPARE(map.value("post_snapshot").toString(), QStringLiteral("302"));
            QCOMPARE(map.value("kernel_artifacts").toString(), QStringLiteral("verified"));
            QCOMPARE(map.value("dkms_artifacts").toString(), QStringLiteral("failed"));
            QCOMPARE(map.value("initramfs_artifacts").toString(), QStringLiteral("unknown"));
            QCOMPARE(map.value("new_kernel_boot").toString(), QStringLiteral("unknown"));
            QCOMPARE(map.value("running_kernel").toString(), QStringLiteral("old-running"));
            QCOMPARE(map.value("boot_details").toString(), QStringLiteral("line one\nline two"));
            QVERIFY(map.value("hook_details").toString().contains(QStringLiteral("exit status unavailable")));
            QCOMPARE(map.value("cleanup_status").toString(), QStringLiteral("warnings"));
            QCOMPARE(map.value("cleanup_removed_files").toString(), QStringLiteral("3"));
            QCOMPARE(map.value("cleanup_reclaimed_bytes").toString(), QStringLiteral("4096"));
            QCOMPARE(map.value("cleanup_details").toString(), QStringLiteral("Busy archive retained"));
            return;
        }
        QVariantMap state;
        QString error;
        QVERIFY2(ZethropolUpdateWire::decodeState(reply.arguments(), &state, &error), qPrintable(error));
        QCOMPARE(state.value("pending_updates_count").toUInt(), uint(10));
        QVERIFY(state.value("is_checking").toBool());
        QVERIFY(!state.value("is_updating").toBool());
    }
    void zeroUpdatesAndUpdatingAreNotLost()
    {
        QVariantMap state;
        QVERIFY(ZethropolUpdateWire::decodeState({false, true, QString(), uint(0)}, &state, nullptr));
        QVERIFY(state.value("is_updating").toBool());
        QCOMPARE(state.value("pending_updates_count").toUInt(), uint(0));
    }
    void malformedReplyDoesNotReplaceExistingState()
    {
        for (const QVariantList &args : QList<QVariantList>{
                 {}, {false}, {false, false, QString("now")},
                 {false, false, QString("now"), QString("10")},
                 {false, false, QString("now"), int(-1)},
                 {QString("false"), false, QString("now"), uint(10)},
                 {false, false, QString("now"), uint(10), true}}) {
            QVariantMap state{{QStringLiteral("preserved"), true}};
            QString error;
            QVERIFY(!ZethropolUpdateWire::decodeState(args, &state, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(state, (QVariantMap{{QStringLiteral("preserved"), true}}));
        }
    }
    void existingMapFormRemainsSupported()
    {
        const QVariantMap expected{{"is_checking", false}, {"is_updating", false},
            {"last_check_time", QString("now")}, {"pending_updates_count", uint(3)}};
        QVariantMap state;
        QVERIFY(ZethropolUpdateWire::decodeState({expected}, &state, nullptr));
        QCOMPARE(state, expected);
    }
};
QTEST_GUILESS_MAIN(UpdateStateDecoderTest)
#include "UpdateStateDecoderTest.moc"
