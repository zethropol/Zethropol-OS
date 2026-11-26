#include "ChannelManager.h"
#include "MpvItem.h"
#include "StreamSecurity.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace zethropol::tv;

class M3uSecurityTest : public QObject
{
    Q_OBJECT

private slots:
    void urlSchemeAllowlistRejectsDangerousSchemesAndInjections()
    {
        // Allowed http/https URLs
        QVERIFY(StreamSecurity::isAllowedMediaUrl(QStringLiteral("https://example.org/live/stream.m3u8")));
        QVERIFY(StreamSecurity::isAllowedMediaUrl(QStringLiteral("http://streams.example.org:8080/ch1/index.m3u8?token=abc")));

        // Forbidden schemes and injection vectors
        const QStringList forbidden = {
            QStringLiteral("file:///etc/passwd"),
            QStringLiteral("FILE:///home/user/.ssh/id_rsa"),
            QStringLiteral("exec://sh -c id"),
            QStringLiteral("lua://os.execute('id')"),
            QStringLiteral("edl://%12%/etc/passwd"),
            QStringLiteral("av://lavfi:testsrc"),
            QStringLiteral("avdevice://v4l2:/dev/video0"),
            QStringLiteral("fd://0"),
            QStringLiteral("memory://payload"),
            QStringLiteral("mpv://script"),
            QStringLiteral("data:text/plain;base64,SGVsbG8="),
            QStringLiteral("javascript:alert(1)"),
            QStringLiteral("pipe:0"),
            QStringLiteral("/etc/passwd"),
            QStringLiteral("../local.m3u8"),
            QStringLiteral("--script=/opt/evil.lua"),
            QStringLiteral("https://example.org/a.m3u8\n--script=/opt/x.lua"),
            QStringLiteral("https://example.org/a%00.m3u8"),
            QStringLiteral("https://user:pass@example.org/live.m3u8"),
            QStringLiteral("https://"),
            QStringLiteral(""),
        };

        for (const QString &bad : forbidden) {
            QString reason;
            QVERIFY2(!StreamSecurity::isAllowedMediaUrl(bad, &reason),
                     qPrintable(QStringLiteral("Expected rejection for: %1").arg(bad)));
            QVERIFY(!reason.isEmpty());
        }
    }

    void m3uParserDropsForbiddenStreamsSanitizesLogosAndEnforcesSizeCap()
    {
        const QByteArray sampleM3u = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"ch.safe1\" tvg-logo=\"https://cdn.example.org/logo1.png\" group-title=\"Haber\",Güvenli Kanal 1\n"
            "https://live.example.org/ch1/master.m3u8\n"
            "#EXTVLCOPT:http-user-agent=evil\n"
            "#EXTINF:-1 tvg-id=\"ch.file\" tvg-logo=\"file:///etc/hostname\" group-title=\"Zararlı\",Yerel Dosya Saldırısı\n"
            "file:///etc/passwd\n"
            "#EXTINF:-1 tvg-id=\"ch.exec\" group-title=\"Zararlı\",Komut Çalıştırma\n"
            "exec://sh\n"
            "#EXTINF:-1 tvg-id=\"ch.lua\" group-title=\"Zararlı\",Lua Enjeksiyonu\n"
            "lua://exploit\n"
            "#EXTINF:-1 tvg-id=\"ch.safe2\" tvg-logo=\"file:///etc/shadow\" group-title=\"Belgesel\",Güvenli Kanal 2\n"
            "http://live.example.org/ch2/stream.m3u8\n");

        const M3uParseResult parsed = ChannelManager::parseM3uContent(sampleM3u);
        QVERIFY(parsed.errorMessage.isEmpty());
        QCOMPARE(parsed.channels.size(), 2);
        QCOMPARE(parsed.rejectedCount, 3);

        QCOMPARE(parsed.channels[0].name, QStringLiteral("Güvenli Kanal 1"));
        QCOMPARE(parsed.channels[0].group, QStringLiteral("Haber"));
        QCOMPARE(parsed.channels[0].logoUrl, QStringLiteral("https://cdn.example.org/logo1.png"));
        QCOMPARE(parsed.channels[0].streamUrl, QStringLiteral("https://live.example.org/ch1/master.m3u8"));

        // Safe channel 2 keeps its http stream, but its `file://` logo MUST be stripped.
        QCOMPARE(parsed.channels[1].name, QStringLiteral("Güvenli Kanal 2"));
        QCOMPARE(parsed.channels[1].logoUrl, QString());

        // Enforce 4 MiB hard cap.
        const QByteArray oversized(StreamSecurity::kMaxM3uBytes + 16, 'A');
        const M3uParseResult tooBig = ChannelManager::parseM3uContent(oversized);
        QVERIFY(!tooBig.errorMessage.isEmpty());
        QVERIFY(tooBig.channels.isEmpty());
    }

    void channelSelectionFavoritesFilteringAndPersistenceWorkInIsolatedConfig()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString m3uFile = tempDir.filePath(QStringLiteral("test.m3u8"));
        {
            QFile f(m3uFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#EXTM3U\n"
                    "#EXTINF:-1 tvg-id=\"alpha\" group-title=\"Haber\",Alpha TV\n"
                    "https://stream.example.org/alpha.m3u8\n"
                    "#EXTINF:-1 tvg-id=\"beta\" group-title=\"Spor\",Beta Spor\n"
                    "https://stream.example.org/beta.m3u8\n"
                    "#EXTINF:-1 tvg-id=\"evil\" group-title=\"Spor\",Kötü Akış\n"
                    "file:///etc/passwd\n");
        }

        QString betaId;
        {
            ChannelManager mgr(tempDir.path());
            QVERIFY(mgr.importM3uFromFile(m3uFile, true));
            QCOMPARE(mgr.totalChannelCount(), 2);
            QCOMPARE(mgr.lastRejectedUrlCount(), 1);

            QSignalSpy selectSpy(&mgr, &ChannelManager::channelSelected);
            QVERIFY(mgr.selectChannel(0));
            QCOMPARE(selectSpy.count(), 1);
            QCOMPARE(mgr.currentChannelName(), QStringLiteral("Alpha TV"));

            // Next channel switches immediately to Beta Spor
            QVERIFY(mgr.nextChannel());
            QCOMPARE(mgr.currentChannelName(), QStringLiteral("Beta Spor"));
            betaId = mgr.currentChannelId();

            // Favorite toggle + category filter
            mgr.toggleFavorite(betaId);
            QVERIFY(mgr.currentChannelFavorite());
            mgr.setSelectedCategory(QStringLiteral("Favoriler"));
            QCOMPARE(mgr.filteredChannelCount(), 1);

            // Search filter
            mgr.setSelectedCategory(QStringLiteral("Tümü"));
            mgr.setSearchQuery(QStringLiteral("alpha"));
            QCOMPARE(mgr.filteredChannelCount(), 1);
        }

        // Reload from isolated config dir: channels, favorite, and lastChannelId persist
        {
            ChannelManager reloaded(tempDir.path());
            QCOMPARE(reloaded.totalChannelCount(), 2);
            QCOMPARE(reloaded.currentChannelId(), betaId);
            QVERIFY(reloaded.currentChannelFavorite());
        }
    }

    void mpvItemRejectsUnsafeSchemesBeforePlayback()
    {
        MpvItem player;
        QVERIFY(!player.playUrl(QStringLiteral("file:///etc/passwd")));
        QCOMPARE(player.playbackState(), QStringLiteral("error"));
        QVERIFY(player.errorMessage().contains(QStringLiteral("Güvenlik")));

        QVERIFY(!player.playUrl(QStringLiteral("exec://id")));
        QCOMPARE(player.playbackState(), QStringLiteral("error"));

        QVERIFY(!player.playUrl(QStringLiteral("lua://os.exit()")));
        QCOMPARE(player.playbackState(), QStringLiteral("error"));

        // MpvItem must also reject unresolved resolve:// URIs directly.
        QVERIFY(!player.playUrl(QStringLiteral("resolve://https://www.trtizle.com/canli/tv/trt-1")));
        QCOMPARE(player.playbackState(), QStringLiteral("error"));
    }

    void resolveSchemeParsingSingleChannelAndPhase1StateMigrationWorkSafely()
    {
        // M3U with resolve:// valid https page vs forbidden resolve://file://
        const QByteArray resolveM3u = QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"TRT1.tr\" group-title=\"Ulusal\",TRT 1 Sayfa\n"
            "resolve://https://www.trtizle.com/canli/tv/trt-1\n"
            "#EXTINF:-1 tvg-id=\"evil.res\" group-title=\"Zararlı\",Kötü Çözücü\n"
            "resolve://file:///etc/passwd\n");
        const M3uParseResult parsed = ChannelManager::parseM3uContent(resolveM3u);
        QVERIFY(parsed.errorMessage.isEmpty());
        QCOMPARE(parsed.channels.size(), 1);
        QCOMPARE(parsed.rejectedCount, 1);
        QVERIFY(parsed.channels[0].streamUrl.isEmpty());
        QCOMPARE(parsed.channels[0].sourcePageUrl, QStringLiteral("https://www.trtizle.com/canli/tv/trt-1"));

        // Single-channel official page add + security rejection
        QTemporaryDir singleDir;
        QVERIFY(singleDir.isValid());
        {
            ChannelManager mgr(singleDir.path());
            QVERIFY(!mgr.addSingleChannel(QStringLiteral("Yasak"), QStringLiteral("exec://id")));
            QVERIFY(mgr.addSingleChannel(QStringLiteral("Özel Canlı"),
                                         QStringLiteral("https://yayinci.example.org/canli"),
                                         QStringLiteral("Haber")));
            QCOMPARE(mgr.currentChannelName(), QStringLiteral("Özel Canlı"));
            QVERIFY(mgr.currentChannelStreamUrl().isEmpty());
            QCOMPARE(mgr.currentChannelSourcePageUrl(), QStringLiteral("https://yayinci.example.org/canli"));
        }

        // Phase 1 demo-only state.json (version: 1) auto-migrates to v3 Turkish 25-channel lineup
        QTemporaryDir migrateDir;
        QVERIFY(migrateDir.isValid());
        {
            QFile oldState(migrateDir.filePath(QStringLiteral("state.json")));
            QVERIFY(oldState.open(QIODevice::WriteOnly));
            oldState.write(
                "{\"version\":1,\"favorites\":[\"custom-fav\"],\"lastChannelId\":\"old\",\"channels\":["
                "{\"id\":\"old\",\"name\":\"Blender Sinema\",\"group\":\"Açık Kaynak Yayın\","
                "\"streamUrl\":\"https://demo.unified-streaming.com/test.m3u8\",\"tvgId\":\"zethropol.demo.tears\"}"
                "]}");
        }
        {
            ChannelManager migrated(migrateDir.path());
            QCOMPARE(migrated.totalChannelCount(), 31);
            QVERIFY(migrated.selectChannel(0));
            QCOMPARE(migrated.currentChannelName(), QStringLiteral("TRT 1"));
            QCOMPARE(migrated.currentChannelTvgId(), QStringLiteral("TRT1.tr"));
            QVERIFY(!migrated.currentChannelSourcePageUrl().isEmpty());

            QFile upgradedFile(migrated.stateFilePath());
            QVERIFY(upgradedFile.open(QIODevice::ReadOnly));
            const QJsonObject upgradedObj = QJsonDocument::fromJson(upgradedFile.readAll()).object();
            QCOMPARE(upgradedObj.value(QStringLiteral("version")).toInt(), 5);
        }
    }

    void turkishDefaultLineupOrderAndZeroForeignChannels()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        ChannelManager mgr(tempDir.path());
        QCOMPARE(mgr.totalChannelCount(), 31);
        QCOMPARE(mgr.filteredChannelCount(), 31);

        struct ExpectedChannel {
            int number;
            const char *name;
            const char *group;
            const char *tvgId;
        };
        static const ExpectedChannel kExpected[31] = {
            {1, "TRT 1", "Ulusal", "TRT1.tr"},
            {2, "ATV", "Ulusal", "ATV.tr"},
            {3, "Show TV", "Ulusal", "ShowTV.tr"},
            {4, "Star TV", "Ulusal", "StarTV.tr"},
            {5, "Kanal D", "Ulusal", "KanalD.tr"},
            {6, "NOW", "Ulusal", "NOW.tr"},
            {7, "TV8", "Ulusal", "TV8.tr"},
            {8, "Kanal 7", "Ulusal", "Kanal7.tr"},
            {9, "Beyaz TV", "Ulusal", "BeyazTV.tr"},
            {10, "TV8.5", "Ulusal", "TV85.tr"},
            {11, "TRT Haber", "Haber", "TRTHaber.tr"},
            {12, "NTV", "Haber", "NTV.tr"},
            {13, "CNN Türk", "Haber", "CNNTurk.tr"},
            {14, "Habertürk", "Haber", "Haberturk.tr"},
            {15, "Halk TV", "Haber", "HalkTV.tr"},
            {16, "Sözcü TV", "Haber", "SozcuTV.tr"},
            {17, "A Haber", "Haber", "AHaber.tr"},
            {18, "Tele1", "Haber", "Tele1.tr"},
            {19, "Haber Global", "Haber", "HaberGlobal.tr"},
            {20, "TGRT Haber", "Haber", "TGRTHaber.tr"},
            {21, "Ülke TV", "Haber", "UlkeTV.tr"},
            {22, "Kanal 24", "Haber", "24TV.tr"},
            {23, "TRT Spor", "Spor", "TRTSpor.tr"},
            {24, "TRT Spor Yıldız", "Spor", "TRTSpor2.tr"},
            {25, "TRT Belgesel", "Belgesel", "TRTBelgesel.tr"},
            {26, "DMAX", "Belgesel", "DMAX.tr"},
            {27, "TRT Çocuk", "Çocuk", "TRTCocuk.tr"},
            {28, "Cartoon Network Türkiye", "Çocuk", "CartoonNetwork.tr"},
            {29, "MinikaGO", "Çocuk", "MinikaGO.tr"},
            {30, "MinikaÇOCUK", "Çocuk", "MinikaCocuk.tr"},
            {31, "TRT 2", "Kültür", "TRT2.tr"},
        };

        const QStringList forbiddenForeign = {
            QStringLiteral("TRT World"),
            QStringLiteral("Al Jazeera English"),
            QStringLiteral("France 24 English"),
            QStringLiteral("NHK World-Japan"),
        };

        QHash<QString, int> categoryCounts;
        for (int i = 0; i < 31; ++i) {
            const QModelIndex idx = mgr.index(i, 0);
            const int num = mgr.data(idx, ChannelManager::NumberRole).toInt();
            const QString name = mgr.data(idx, ChannelManager::NameRole).toString();
            const QString group = mgr.data(idx, ChannelManager::GroupRole).toString();
            const QString tvgId = mgr.data(idx, ChannelManager::TvgIdRole).toString();
            const QString streamUrl = mgr.data(idx, ChannelManager::StreamUrlRole).toString();
            const QString sourcePageUrl = mgr.data(idx, ChannelManager::SourcePageUrlRole).toString();

            QCOMPARE(num, kExpected[i].number);
            QCOMPARE(name, QString::fromUtf8(kExpected[i].name));
            QCOMPARE(group, QString::fromUtf8(kExpected[i].group));
            QCOMPARE(tvgId, QString::fromUtf8(kExpected[i].tvgId));
            QVERIFY2(!forbiddenForeign.contains(name), qPrintable(name));
            categoryCounts[group] += 1;

            // Strict security & official HTTPS check on every default channel URL & fallback URL (no raw-IP http://)
            QVERIFY2(StreamSecurity::isAllowedMediaUrl(sourcePageUrl)
                         && sourcePageUrl.startsWith(QStringLiteral("https://")),
                     qPrintable(QStringLiteral("Invalid sourcePageUrl for %1: %2").arg(name, sourcePageUrl)));
            if (!streamUrl.isEmpty()) {
                QVERIFY2(StreamSecurity::isAllowedMediaUrl(streamUrl)
                             && streamUrl.startsWith(QStringLiteral("https://")),
                         qPrintable(QStringLiteral("Invalid streamUrl for %1: %2").arg(name, streamUrl)));
            }
            const QStringList fallbacks = mgr.data(idx, ChannelManager::FallbackUrlsRole).toStringList();
            for (const QString &fb : fallbacks) {
                QVERIFY2(StreamSecurity::isAllowedMediaUrl(fb)
                             && fb.startsWith(QStringLiteral("https://")),
                         qPrintable(QStringLiteral("Invalid fallbackUrl for %1: %2").arg(name, fb)));
            }
        }

        // Exact category distribution across 31 channels
        QCOMPARE(categoryCounts.value(QStringLiteral("Ulusal")), 10);
        QCOMPARE(categoryCounts.value(QStringLiteral("Haber")), 12);
        QCOMPARE(categoryCounts.value(QStringLiteral("Spor")), 2);
        QCOMPARE(categoryCounts.value(QStringLiteral("Belgesel")), 2);
        QCOMPARE(categoryCounts.value(QStringLiteral("Çocuk")), 4);
        QCOMPARE(categoryCounts.value(QStringLiteral("Kültür")), 1);

        const QStringList expectedCategories = {
            QStringLiteral("Tümü"),
            QStringLiteral("Favoriler"),
            QStringLiteral("Ulusal"),
            QStringLiteral("Haber"),
            QStringLiteral("Spor"),
            QStringLiteral("Belgesel"),
            QStringLiteral("Çocuk"),
            QStringLiteral("Kültür"),
        };
        QCOMPARE(mgr.categories(), expectedCategories);
    }

    void stateJsonV2ToV3MigrationPreservesCustomChannelsAndFavorites()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        {
            QFile v2State(tempDir.filePath(QStringLiteral("state.json")));
            QVERIFY(v2State.open(QIODevice::WriteOnly));
            v2State.write(
                "{\n"
                "  \"version\": 2,\n"
                "  \"favorites\": [\"legacy-trt-haber\", \"legacy-custom-1\", \"legacy-trt-world\"],\n"
                "  \"lastChannelId\": \"legacy-trt-world\",\n"
                "  \"channels\": [\n"
                "    {\"id\":\"legacy-trt1\",\"name\":\"TRT 1\",\"group\":\"Ulusal\","
                "     \"streamUrl\":\"https://tv-trt1.medya.trt.com.tr/master.m3u8\","
                "     \"sourcePageUrl\":\"https://www.trtizle.com/canli/tv/trt-1\",\"tvgId\":\"TRT1.tr\"},\n"
                "    {\"id\":\"legacy-trt-haber\",\"name\":\"TRT Haber\",\"group\":\"Haber\","
                "     \"streamUrl\":\"https://tv-trthaber.medya.trt.com.tr/master.m3u8\","
                "     \"sourcePageUrl\":\"https://www.trthaber.com/canli-yayin\",\"tvgId\":\"TRTHaber.tr\"},\n"
                "    {\"id\":\"legacy-trt-world\",\"name\":\"TRT World\",\"group\":\"Haber\","
                "     \"streamUrl\":\"https://tv-trtworld.live.trt.com.tr/master.m3u8\","
                "     \"sourcePageUrl\":\"https://www.trtizle.com/canli/tv/trt-world\",\"tvgId\":\"TRTWorld.tr\"},\n"
                "    {\"id\":\"legacy-aje\",\"name\":\"Al Jazeera English\",\"group\":\"Haber\","
                "     \"streamUrl\":\"https://live-hls-web-aje-fa.thehlive.com/AJE/index.m3u8\","
                "     \"sourcePageUrl\":\"https://www.aljazeera.com/live\",\"tvgId\":\"AlJazeeraEnglish.qa\"},\n"
                "    {\"id\":\"legacy-f24\",\"name\":\"France 24 English\",\"group\":\"Haber\","
                "     \"streamUrl\":\"https://live.france24.com/hls/live/2037218/F24_EN_HI_HLS/master_5000.m3u8\","
                "     \"sourcePageUrl\":\"https://www.france24.com/en/live\",\"tvgId\":\"France24English.fr\"},\n"
                "    {\"id\":\"legacy-nhk\",\"name\":\"NHK World-Japan\",\"group\":\"Belgesel\","
                "     \"streamUrl\":\"https://nhkwlive-xjp.akamaized.net/hls/live/2003458/nhkwlive-xjp-en/index_4M.m3u8\","
                "     \"sourcePageUrl\":\"https://www3.nhk.or.jp/nhkworld/en/live/\",\"tvgId\":\"NHKWorldJapan.jp\"},\n"
                "    {\"id\":\"legacy-custom-1\",\"name\":\"Özel Yerel TV\",\"group\":\"Özel\","
                "     \"streamUrl\":\"https://ozel.example.org/live/master.m3u8\","
                "     \"sourcePageUrl\":\"\",\"tvgId\":\"OzelYerel.tr\"}\n"
                "  ]\n"
                "}\n");
        }

        ChannelManager upgraded(tempDir.path());
        // 31 Turkish v5 defaults + 1 preserved custom channel = 32
        QCOMPARE(upgraded.totalChannelCount(), 32);
        // Removed foreign channel (TRT World) should not remain dangling as currentChannelId
        QVERIFY(upgraded.currentChannelId().isEmpty());

        // Check order & favorites preservation
        QCOMPARE(upgraded.data(upgraded.index(0, 0), ChannelManager::NameRole).toString(), QStringLiteral("TRT 1"));
        QCOMPARE(upgraded.data(upgraded.index(1, 0), ChannelManager::NameRole).toString(), QStringLiteral("ATV"));
        QCOMPARE(upgraded.data(upgraded.index(10, 0), ChannelManager::NameRole).toString(), QStringLiteral("TRT Haber"));
        QCOMPARE(upgraded.data(upgraded.index(10, 0), ChannelManager::FavoriteRole).toBool(), true);
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::NameRole).toString(), QStringLiteral("Özel Yerel TV"));
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::NumberRole).toInt(), 32);
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::FavoriteRole).toBool(), true);

        // Ensure zero foreign channels survived migration
        for (int i = 0; i < upgraded.rowCount(); ++i) {
            const QString n = upgraded.data(upgraded.index(i, 0), ChannelManager::NameRole).toString();
            QVERIFY(n != QStringLiteral("TRT World"));
            QVERIFY(n != QStringLiteral("Al Jazeera English"));
            QVERIFY(n != QStringLiteral("France 24 English"));
            QVERIFY(n != QStringLiteral("NHK World-Japan"));
        }

        // Verify state.json on disk is upgraded to version 5
        QFile stateFile(upgraded.stateFilePath());
        QVERIFY(stateFile.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(stateFile.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("version")).toInt(), 5);
    }

    void stateJsonV3ToV4MigrationRepairsEndpointsAndPreservesCustomChannels()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        {
            QFile v3State(tempDir.filePath(QStringLiteral("state.json")));
            QVERIFY(v3State.open(QIODevice::WriteOnly));
            v3State.write(
                "{\n"
                "  \"version\": 3,\n"
                "  \"favorites\": [\"v3-atv-id\", \"v3-custom-id\"],\n"
                "  \"lastChannelId\": \"v3-atv-id\",\n"
                "  \"channels\": [\n"
                "    {\"id\":\"v3-trt1\",\"name\":\"TRT 1\",\"group\":\"Ulusal\","
                "     \"streamUrl\":\"https://tv-trt1.medya.trt.com.tr/master.m3u8\","
                "     \"sourcePageUrl\":\"https://www.trtizle.com/canli/tv/trt-1\",\"tvgId\":\"TRT1.tr\"},\n"
                "    {\"id\":\"v3-atv-id\",\"name\":\"ATV\",\"group\":\"Ulusal\","
                "     \"streamUrl\":\"https://trkvz-live.daioncdn.net/atv/atv.m3u8\","
                "     \"sourcePageUrl\":\"https://www.atv.com.tr/canli-yayin\",\"tvgId\":\"ATV.tr\"},\n"
                "    {\"id\":\"v3-show\",\"name\":\"Show TV\",\"group\":\"Ulusal\","
                "     \"streamUrl\":\"https://ciner-live.daioncdn.net/showtv/showtv.m3u8\","
                "     \"sourcePageUrl\":\"https://www.showtv.com.tr/canli-yayin\",\"tvgId\":\"ShowTV.tr\"},\n"
                "    {\"id\":\"v3-custom-id\",\"name\":\"Benim Özel Kanalım\",\"group\":\"Özel\","
                "     \"streamUrl\":\"https://custom.example.org/stream/master.m3u8\","
                "     \"sourcePageUrl\":\"\",\"tvgId\":\"Custom.tr\"}\n"
                "  ]\n"
                "}\n");
        }

        ChannelManager upgraded(tempDir.path());
        // 31 repaired Turkish v5 defaults + 1 custom channel = 32
        QCOMPARE(upgraded.totalChannelCount(), 32);

        // ATV (index 1) upgraded from broken 404 daioncdn URL to Turkuvaz official page + token API fallback
        QCOMPARE(upgraded.data(upgraded.index(1, 0), ChannelManager::NameRole).toString(), QStringLiteral("ATV"));
        QVERIFY(upgraded.data(upgraded.index(1, 0), ChannelManager::StreamUrlRole).toString().isEmpty());
        QCOMPARE(upgraded.data(upgraded.index(1, 0), ChannelManager::SourcePageUrlRole).toString(),
                 QStringLiteral("https://www.atv.com.tr/canli-yayin"));
        QVERIFY(upgraded.data(upgraded.index(1, 0), ChannelManager::FallbackUrlsRole).toStringList().size() >= 2);
        QCOMPARE(upgraded.data(upgraded.index(1, 0), ChannelManager::FavoriteRole).toBool(), true);
        // lastChannelId remapped via tvgId to upgraded ATV
        QCOMPARE(upgraded.currentChannelName(), QStringLiteral("ATV"));

        // Show TV (index 2) upgraded to live-verified app-parameterized DaionCDN URL
        QVERIFY(upgraded.data(upgraded.index(2, 0), ChannelManager::StreamUrlRole)
                    .toString()
                    .contains(QStringLiteral("ciner.daioncdn.net/showtv/showtv.m3u8?ce=3&app=")));

        // Custom channel preserved at #32 with its favorite flag intact
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::NameRole).toString(),
                 QStringLiteral("Benim Özel Kanalım"));
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::FavoriteRole).toBool(), true);

        QFile stateFile(upgraded.stateFilePath());
        QVERIFY(stateFile.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(stateFile.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("version")).toInt(), 5);
    }

    void stateJsonV4ToV5MigrationAddsSixNewChannelsAndPreservesCustom()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        {
            QFile v4State(tempDir.filePath(QStringLiteral("state.json")));
            QVERIFY(v4State.open(QIODevice::WriteOnly));
            v4State.write(
                "{\n"
                "  \"version\": 4,\n"
                "  \"favorites\": [\"v4-ntv\", \"v4-custom\"],\n"
                "  \"lastChannelId\": \"v4-ntv\",\n"
                "  \"channels\": [\n"
                "    {\"id\":\"v4-trt1\",\"name\":\"TRT 1\",\"group\":\"Ulusal\","
                "     \"streamUrl\":\"https://tv-trt1.medya.trt.com.tr/master.m3u8\","
                "     \"sourcePageUrl\":\"https://www.trtizle.com/canli/tv/trt-1\",\"tvgId\":\"TRT1.tr\"},\n"
                "    {\"id\":\"v4-ntv\",\"name\":\"NTV\",\"group\":\"Haber\","
                "     \"streamUrl\":\"https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3\","
                "     \"sourcePageUrl\":\"https://www.ntv.com.tr/canli-yayin/ntv\",\"tvgId\":\"NTV.tr\"},\n"
                "    {\"id\":\"v4-custom\",\"name\":\"Özel Belgesel\",\"group\":\"Özel\","
                "     \"streamUrl\":\"https://custom.example.org/doc.m3u8\","
                "     \"sourcePageUrl\":\"\",\"tvgId\":\"OzelDoc.tr\"}\n"
                "  ]\n"
                "}\n");
        }

        ChannelManager upgraded(tempDir.path());
        // 31 v5 defaults + 1 preserved custom channel = 32
        QCOMPARE(upgraded.totalChannelCount(), 32);
        QCOMPARE(upgraded.currentChannelName(), QStringLiteral("NTV"));
        QCOMPARE(upgraded.data(upgraded.index(11, 0), ChannelManager::FavoriteRole).toBool(), true);

        // Verify all 6 new channels placed at the end of their respective categories
        QCOMPARE(upgraded.data(upgraded.index(20, 0), ChannelManager::NameRole).toString(), QStringLiteral("Ülke TV"));
        QCOMPARE(upgraded.data(upgraded.index(21, 0), ChannelManager::NameRole).toString(), QStringLiteral("Kanal 24"));
        QCOMPARE(upgraded.data(upgraded.index(25, 0), ChannelManager::NameRole).toString(), QStringLiteral("DMAX"));
        QCOMPARE(upgraded.data(upgraded.index(27, 0), ChannelManager::NameRole).toString(), QStringLiteral("Cartoon Network Türkiye"));
        QVERIFY(upgraded.data(upgraded.index(27, 0), ChannelManager::StreamUrlRole).toString().isEmpty()); // Option A: no raw IP
        QCOMPARE(upgraded.data(upgraded.index(28, 0), ChannelManager::NameRole).toString(), QStringLiteral("MinikaGO"));
        QCOMPARE(upgraded.data(upgraded.index(29, 0), ChannelManager::NameRole).toString(), QStringLiteral("MinikaÇOCUK"));

        // Custom channel preserved at #32 with favorite intact
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::NameRole).toString(), QStringLiteral("Özel Belgesel"));
        QCOMPARE(upgraded.data(upgraded.index(31, 0), ChannelManager::FavoriteRole).toBool(), true);

        QFile stateFile(upgraded.stateFilePath());
        QVERIFY(stateFile.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(stateFile.readAll()).object();
        QCOMPARE(root.value(QStringLiteral("version")).toInt(), 5);
    }
};

QTEST_MAIN(M3uSecurityTest)
#include "M3uSecurityTest.moc"
