#include "EpgManager.h"
#include "StreamSecurity.h"

#include <QDateTime>
#include <QTimeZone>
#include <QtTest>

#include <zlib.h>

using namespace zethropol::tv;

class EpgWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void xmltvTimestampParsingHandlesOffsetsAndRejectsInvalid()
    {
        const QDateTime dtUtc = EpgManager::parseXmltvTimestamp(QStringLiteral("20261126120000 +0000"));
        QVERIFY(dtUtc.isValid());
        QCOMPARE(dtUtc, QDateTime(QDate(2026, 11, 26), QTime(12, 0, 0), QTimeZone::utc()));

        // 15:00 +0300 == 12:00 UTC
        const QDateTime dtTr = EpgManager::parseXmltvTimestamp(QStringLiteral("20261126150000 +0300"));
        QVERIFY(dtTr.isValid());
        QCOMPARE(dtTr, dtUtc);

        QVERIFY(!EpgManager::parseXmltvTimestamp(QStringLiteral("not-a-timestamp")).isValid());
        QVERIFY(!EpgManager::parseXmltvTimestamp(QStringLiteral("20261345120000 +0000")).isValid());
    }

    void computesCurrentProgressAndExactTwoHourUpcomingWindow()
    {
        // Fixed reference time: 2026-11-26 12:00:00 UTC
        const QDateTime nowUtc(QDate(2026, 11, 26), QTime(12, 0, 0), QTimeZone::utc());

        const QByteArray xmltv = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<tv>\n"
            "  <channel id=\"ch.news\">\n"
            "    <display-name>Zethropol Haber</display-name>\n"
            "  </channel>\n"
            "  <!-- Past program (10:00 - 11:30): MUST be excluded -->\n"
            "  <programme start=\"20261126100000 +0000\" stop=\"20261126113000 +0000\" channel=\"ch.news\">\n"
            "    <title>Sabah Bülteni</title>\n"
            "  </programme>\n"
            "  <!-- Current live program (11:30 - 12:30): midpoint at 12:00 => progress 0.5, remaining 30m -->\n"
            "  <programme start=\"20261126113000 +0000\" stop=\"20261126123000 +0000\" channel=\"ch.news\">\n"
            "    <title>Öğle Haberleri</title>\n"
            "    <desc>Canlı gündem özeti.</desc>\n"
            "    <category>Haber</category>\n"
            "  </programme>\n"
            "  <!-- Upcoming +30m (12:30 - 13:15): INSIDE 2-hour window -->\n"
            "  <programme start=\"20261126123000 +0000\" stop=\"20261126131500 +0000\" channel=\"ch.news\">\n"
            "    <title>Ekonomi Dosyası</title>\n"
            "    <category>Ekonomi</category>\n"
            "  </programme>\n"
            "  <!-- Upcoming +120m exact boundary (14:00 - 14:45): INSIDE 2-hour window -->\n"
            "  <programme start=\"20261126140000 +0000\" stop=\"20261126144500 +0000\" channel=\"ch.news\">\n"
            "    <title>Bilim ve Teknoloji</title>\n"
            "    <category>Bilim</category>\n"
            "  </programme>\n"
            "  <!-- Upcoming +121m (14:01 - 15:00): OUTSIDE 2-hour window, MUST be excluded -->\n"
            "  <programme start=\"20261126140100 +0000\" stop=\"20261126150000 +0000\" channel=\"ch.news\">\n"
            "    <title>Akşam Kuşağı</title>\n"
            "  </programme>\n"
            "</tv>\n");

        EpgManager epg(false);
        QVERIFY(epg.loadXmltvBytes(xmltv, QStringLiteral("unit-test")));

        epg.setActiveChannel(QStringLiteral("ch.news"), QStringLiteral("Zethropol Haber"));
        epg.refreshForTime(nowUtc);

        QVERIFY(epg.hasEpgData());
        QVERIFY(epg.hasCurrentProgram());
        QCOMPARE(epg.currentTitle(), QStringLiteral("Öğle Haberleri"));
        QVERIFY(qAbs(epg.currentProgress() - 0.5) < 1e-6);
        QCOMPARE(epg.currentRemainingMinutes(), 30);

        const QVariantList upcoming = epg.upcomingPrograms();
        QCOMPARE(upcoming.size(), 2);
        QCOMPARE(upcoming[0].toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Ekonomi Dosyası"));
        QCOMPARE(upcoming[0].toMap().value(QStringLiteral("startsInMinutes")).toInt(), 30);
        QCOMPARE(upcoming[0].toMap().value(QStringLiteral("durationMinutes")).toInt(), 45);
        QCOMPARE(upcoming[1].toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Bilim ve Teknoloji"));
        QCOMPARE(upcoming[1].toMap().value(QStringLiteral("startsInMinutes")).toInt(), 120);
    }

    void missingEpgChannelFallsBackGracefullyAndRejectsOversizedXml()
    {
        EpgManager epg(false);
        epg.setActiveChannel(QStringLiteral("unknown.channel"), QStringLiteral("EPG Olmayan Kanal"));
        epg.refreshForTime(QDateTime(QDate(2026, 11, 26), QTime(12, 0, 0), QTimeZone::utc()));

        QVERIFY(!epg.hasEpgData());
        QVERIFY(!epg.hasCurrentProgram());
        QVERIFY(epg.upcomingPrograms().isEmpty());
        QCOMPARE(epg.fallbackMessage(), QStringLiteral("Yayın akışı bilgisi bekleniyor"));

        // Oversized (> 16 MiB) XMLTV rejected cleanly without crashing
        const QByteArray oversized(StreamSecurity::kMaxXmltvBytes + 32, ' ');
        const EpgParseResult parsed = EpgManager::parseXmltvContent(oversized);
        QVERIFY(!parsed.errorMessage.isEmpty());
        QCOMPARE(parsed.totalPrograms, 0);
    }

    void gzipCompressedXmltvDecompressesSafely()
    {
        const QByteArray plainXml = QByteArrayLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<tv><programme start=\"20261126113000 +0000\" stop=\"20261126123000 +0000\" channel=\"TRT1.tr\">"
            "<title>Gzip Test Programı</title></programme></tv>\n");

        // Produce a valid RFC 1952 gzip payload using zlib.
        z_stream zs{};
        QCOMPARE(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
        QByteArray gz(deflateBound(&zs, static_cast<uLong>(plainXml.size())) + 32, '\0');
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(plainXml.constData()));
        zs.avail_in = static_cast<uInt>(plainXml.size());
        zs.next_out = reinterpret_cast<Bytef *>(gz.data());
        zs.avail_out = static_cast<uInt>(gz.size());
        QCOMPARE(deflate(&zs, Z_FINISH), Z_STREAM_END);
        gz.resize(static_cast<qsizetype>(zs.total_out));
        deflateEnd(&zs);

        const EpgParseResult parsed = EpgManager::parseXmltvContent(gz);
        QVERIFY2(parsed.errorMessage.isEmpty(), qPrintable(parsed.errorMessage));
        QCOMPARE(parsed.totalPrograms, 1);
        QCOMPARE(parsed.programsByChannel.value(QStringLiteral("TRT1.tr")).first().title,
                 QStringLiteral("Gzip Test Programı"));
    }

    void builtInTurkishDemoScheduleCoversPrimaryNationalAndNewsChannels()
    {
        EpgManager epg(true);

        struct DefaultChannelRef {
            const char *tvgId;
            const char *name;
        };
        static const DefaultChannelRef kChannels[41] = {
            {"TRT1.tr", "TRT 1"},
            {"ATV.tr", "ATV"},
            {"ShowTV.tr", "Show TV"},
            {"StarTV.tr", "Star TV"},
            {"KanalD.tr", "Kanal D"},
            {"NOW.tr", "NOW"},
            {"TV8.tr", "TV8"},
            {"Kanal7.tr", "Kanal 7"},
            {"BeyazTV.tr", "Beyaz TV"},
            {"TV85.tr", "TV8.5"},
            {"TRTHaber.tr", "TRT Haber"},
            {"NTV.tr", "NTV"},
            {"CNNTurk.tr", "CNN Türk"},
            {"Haberturk.tr", "Habertürk"},
            {"HalkTV.tr", "Halk TV"},
            {"AHaber.tr", "A Haber"},
            {"HaberGlobal.tr", "Haber Global"},
            {"TGRTHaber.tr", "TGRT Haber"},
            {"UlkeTV.tr", "Ülke TV"},
            {"24TV.tr", "Kanal 24"},
            {"TRTSpor.tr", "TRT Spor"},
            {"TRTSpor2.tr", "TRT Spor Yıldız"},
            {"RedBullTV.int", "Red Bull TV"},
            {"FIFAPlus.int", "FIFA+"},
            {"beINSPORTSTurkiye.tr", "beIN SPORTS Türkiye"},
            {"NASATV.us", "NASA TV"},
            {"DWPlanetA.de", "DW Planet A"},
            {"TeknoSeyir.tr", "TeknoSeyir"},
            {"HardwarePlus.tr", "Hardware Plus (HWP)"},
            {"DonanimHaber.tr", "DonanımHaber"},
            {"TRTBelgesel.tr", "TRT Belgesel"},
            {"DMAX.tr", "DMAX"},
            {"TRTCocuk.tr", "TRT Çocuk"},
            {"MinikaGO.tr", "MinikaGO"},
            {"MinikaCocuk.tr", "MinikaÇOCUK"},
            {"PowerTurkTV.tr", "PowerTürk TV"},
            {"PowerTV.tr", "Power TV"},
            {"KralPopTV.tr", "Kral Pop TV"},
            {"Number1TurkTV.tr", "Number1 Türk TV"},
            {"Number1TV.tr", "Number1 TV"},
            {"TRT2.tr", "TRT 2"},
        };

        for (const DefaultChannelRef &ref : kChannels) {
            const QString id = QString::fromUtf8(ref.tvgId);
            const QString name = QString::fromUtf8(ref.name);
            epg.setActiveChannel(id, name);
            QVERIFY2(epg.hasEpgData(), qPrintable(QStringLiteral("Missing EPG data for %1").arg(id)));
            QVERIFY2(epg.hasCurrentProgram(), qPrintable(QStringLiteral("Missing current program for %1").arg(id)));
            QVERIFY2(!epg.currentTitle().isEmpty(), qPrintable(QStringLiteral("Empty current title for %1").arg(id)));
            QVERIFY2(epg.upcomingPrograms().size() >= 1,
                     qPrintable(QStringLiteral("Missing 2-hour upcoming programs for %1").arg(id)));
        }

        // Removed channels must NOT have built-in demo EPG entries
        for (const QString &removedId : {QStringLiteral("SozcuTV.tr"),
                                         QStringLiteral("Tele1.tr"),
                                         QStringLiteral("CartoonNetwork.tr")}) {
            epg.setActiveChannel(removedId, removedId);
            QVERIFY2(!epg.hasEpgData(), qPrintable(QStringLiteral("Unexpected EPG data for removed channel %1").arg(removedId)));
        }

        // Display-name and @SD suffix lookups also resolve cleanly
        epg.setActiveChannel(QString(), QStringLiteral("CNN Türk"));
        QVERIFY(epg.hasCurrentProgram());

        epg.setActiveChannel(QStringLiteral("ShowTV.tr@SD"), QStringLiteral("Show TV"));
        QVERIFY(epg.hasCurrentProgram());
    }
};

QTEST_MAIN(EpgWindowTest)
#include "EpgWindowTest.moc"
