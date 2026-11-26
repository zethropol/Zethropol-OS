#include "MpvItem.h"
#include "StreamResolver.h"
#include "StreamSecurity.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <clocale>

using namespace zethropol::tv;

class StreamResolverTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        std::setlocale(LC_NUMERIC, "C");
    }

    void mpvOrientationIsUprightWithoutDoubleFlip()
    {
        // Compile-time + runtime contract preventing the Phase 1 upside-down FBO regression:
        // libmpv with MPV_RENDER_PARAM_FLIP_Y = 0 renders upright in Qt Quick's top-down
        // texture space, so QQuickFramebufferObject::mirrorVertically MUST remain false.
        static_assert(!MpvItem::kMirrorVertically, "mirrorVertically must be false");
        static_assert(MpvItem::kRenderFlipY == 0, "MPV_RENDER_PARAM_FLIP_Y must be 0");
        static_assert(MpvItem::isOrientationUpright(), "net vertical flip parity must be 0");

        MpvItem player;
        QCOMPARE(player.mirrorVertically(), false);
        QCOMPARE(MpvItem::kRenderFlipY, 0);
        QVERIFY(MpvItem::isOrientationUpright());
    }

    void mpvAudioConfigurationAndRuntimeControlsAreInitializedProperly()
    {
        MpvItem player;
        QVERIFY(player.backendAvailable());

        // Verify explicit PipeWire -> PulseAudio -> ALSA output chain, client identity, and unmuted volume
        QCOMPARE(player.configuredAudioOutput(), QStringLiteral("pipewire,pulse,alsa"));
        QCOMPARE(player.configuredAudioClientName(), QStringLiteral("Zethropol TV"));
        QCOMPARE(player.configuredAudioTrackSelection(), QStringLiteral("auto"));
        QVERIFY(player.volume() > 0);
        QCOMPARE(player.volume(), MpvItem::kDefaultVolume);
        QCOMPARE(player.muted(), false);
        QCOMPARE(player.isMpvMuted(), false);
        QCOMPARE(player.mpvVolume(), static_cast<double>(MpvItem::kDefaultVolume));
        QVERIFY(!player.audioStatus().isEmpty());

        // Verify mandatory libmpv security hardening options remain locked
        QCOMPARE(player.mpvPropertyString(QStringLiteral("load-scripts")), QStringLiteral("no"));
        QCOMPARE(player.mpvPropertyString(QStringLiteral("ytdl")), QStringLiteral("no"));
        QCOMPARE(player.mpvPropertyString(QStringLiteral("load-unsafe-playlists")), QStringLiteral("no"));

        // Verify runtime volume & mute controls propagate into live libmpv
        player.setVolume(55);
        QCOMPARE(player.volume(), 55);
        QTRY_COMPARE_WITH_TIMEOUT(player.mpvVolume(), 55.0, 2000);

        player.setMuted(true);
        QCOMPARE(player.muted(), true);
        QTRY_COMPARE_WITH_TIMEOUT(player.isMpvMuted(), true, 2000);

        player.setMuted(false);
        QCOMPARE(player.muted(), false);
        QTRY_COMPARE_WITH_TIMEOUT(player.isMpvMuted(), false, 2000);

        // Unmuting from 0% restores audible default volume instead of remaining silent
        player.setVolume(0);
        player.setMuted(true);
        player.setMuted(false);
        QVERIFY(player.volume() > 0);
        QCOMPARE(player.muted(), false);
    }

    void stageOneDirectStreamsPassAndUnsafeResolverSchemesFail()
    {
        QVERIFY(StreamResolver::isDirectStreamUrl(QStringLiteral("https://tv-trt1.medya.trt.com.tr/master.m3u8")));
        QVERIFY(StreamResolver::isDirectStreamUrl(QStringLiteral("http://live.example.org/stream/index.m3u8?token=xyz")));
        QVERIFY(!StreamResolver::isDirectStreamUrl(QStringLiteral("https://www.trtizle.com/canli/tv/trt-1")));
        QVERIFY(!StreamResolver::isDirectStreamUrl(QStringLiteral("file:///var/local/test.m3u8")));

        StreamResolver resolver;
        QSignalSpy resolvedSpy(&resolver, &StreamResolver::streamResolved);
        QSignalSpy failedSpy(&resolver, &StreamResolver::resolveFailed);

        // Direct HLS URL resolves synchronously without network round-trip.
        resolver.resolveChannel(QStringLiteral("https://tv-trt1.medya.trt.com.tr/master.m3u8"),
                                QStringLiteral("https://www.trtizle.com/canli/tv/trt-1"),
                                QStringLiteral("TRT 1"));
        QCOMPARE(resolvedSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(resolvedSpy.first().at(0).toString(), QStringLiteral("https://tv-trt1.medya.trt.com.tr/master.m3u8"));
        QCOMPARE(resolvedSpy.first().at(2).toBool(), false);

        // Dangerous schemes wrapped inside resolve:// MUST be rejected.
        const QStringList dangerous = {
            QStringLiteral("resolve://file:///etc/passwd"),
            QStringLiteral("resolve://exec://sh -c id"),
            QStringLiteral("resolve://lua://os.execute('id')"),
            QStringLiteral("resolve://javascript:alert(1)"),
            QStringLiteral("file:///etc/shadow"),
        };
        for (const QString &bad : dangerous) {
            failedSpy.clear();
            resolver.resolveUrl(bad, QStringLiteral("Zararlı"));
            QCOMPARE(failedSpy.count(), 1);
            QVERIFY(!resolver.resolving());
        }
    }

    void stageTwoHtmlExtractorSkipsAdsAndUnsafeUrlsAndPicksOfficialMasterHls()
    {
        const QByteArray sampleHtml = QByteArrayLiteral(
            "<!doctype html><html><head><title>Resmi Canlı Yayın</title></head><body>\n"
            "<script>\n"
            "  var adTag = 'https://pubads.g.doubleclick.net/gampad/preroll.m3u8';\n"
            "  var vast = 'https://ads.example.org/vast/bumper.m3u8';\n"
            "  var evil = 'file:///etc/passwd.m3u8';\n"
            "  var evil2 = 'javascript:alert(1).m3u8';\n"
            "  window.__PLAYER__ = {\n"
            "    \"hlsManifestUrl\": \"https:\\/\\/tv-trt1.medya.trt.com.tr\\/master.m3u8?hd=1\\u0026lang=tr\"\n"
            "  };\n"
            "</script>\n"
            "</body></html>\n");

        const StreamExtractResult res = StreamResolver::extractStreamFromHtml(
            sampleHtml,
            QUrl(QStringLiteral("https://www.trtizle.com/canli/tv/trt-1")),
            QStringLiteral("text/html; charset=utf-8"));

        QVERIFY2(res.ok, qPrintable(res.errorMessage));
        QVERIFY(!res.drmDetected);
        QCOMPARE(res.streamUrl, QStringLiteral("https://tv-trt1.medya.trt.com.tr/master.m3u8?hd=1&lang=tr"));
        QVERIFY(StreamSecurity::isAllowedMediaUrl(res.streamUrl));
    }

    void stageTwoDetectsDrmProtectedPagesGracefully()
    {
        const QByteArray drmHtml = QByteArrayLiteral(
            "<!doctype html><html><body>\n"
            "<script>window.playerConfig = {\"isDrm\":true,\"licenseServer\":\"https://drm.example.org/widevine\","
            "\"keySystem\":\"com.widevine.alpha\"};</script>\n"
            "</body></html>\n");

        const StreamExtractResult res = StreamResolver::extractStreamFromHtml(
            drmHtml,
            QUrl(QStringLiteral("https://drm.example.org/live")),
            QStringLiteral("text/html"));

        QVERIFY(!res.ok);
        QVERIFY(res.drmDetected);
        QCOMPARE(res.errorMessage, QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı"));
    }

    void asyncLocalHttpServerResolvesRedirectHandlesDrmAndRespectsSizeLimit()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, port]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, port]() {
                    const QByteArray req = sock->readAll();
                    const QByteArray firstLine = req.split('\n').value(0);
                    QByteArray resp;
                    if (firstLine.contains("GET /redirect ")) {
                        resp = "HTTP/1.1 302 Found\r\n"
                               "Location: http://127.0.0.1:"
                               + QByteArray::number(port)
                               + "/live-page\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
                    } else if (firstLine.contains("GET /live-page ")) {
                        const QByteArray body =
                            "<html><body><video><source src=\"https://cdn.example.org/official/master.m3u8\" "
                            "type=\"application/x-mpegURL\"></video></body></html>";
                        resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html; charset=utf-8\r\n"
                               "Content-Length: "
                               + QByteArray::number(body.size())
                               + "\r\nConnection: close\r\n\r\n" + body;
                    } else if (firstLine.contains("GET /drm-page ")) {
                        const QByteArray body =
                            "<html><body>{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\"},"
                            "\"drm\":\"com.widevine.alpha\"}</body></html>";
                        resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html\r\n"
                               "Content-Length: "
                               + QByteArray::number(body.size())
                               + "\r\nConnection: close\r\n\r\n" + body;
                    } else if (firstLine.contains("GET /oversized ")) {
                        const QByteArray huge(StreamSecurity::kMaxResolverPageBytes + 65536, 'X');
                        resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html\r\n"
                               "Content-Length: "
                               + QByteArray::number(huge.size())
                               + "\r\nConnection: close\r\n\r\n" + huge;
                    } else if (firstLine.contains("GET /evil-redirect ")) {
                        resp = "HTTP/1.1 302 Found\r\n"
                               "Location: file:///etc/passwd\r\n"
                               "Content-Length: 0\r\n"
                               "Connection: close\r\n\r\n";
                    }
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
                QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            }
        });

        StreamResolver resolver;
        QSignalSpy resolvedSpy(&resolver, &StreamResolver::streamResolved);
        QSignalSpy failedSpy(&resolver, &StreamResolver::resolveFailed);

        // 1. Redirect -> HTML live page -> extracted official master.m3u8
        resolver.resolveUrl(QStringLiteral("resolve://http://127.0.0.1:%1/redirect").arg(port),
                            QStringLiteral("Resmi Kanal"));
        QTRY_COMPARE_WITH_TIMEOUT(resolvedSpy.count(), 1, 5000);
        QCOMPARE(resolvedSpy.first().at(0).toString(), QStringLiteral("https://cdn.example.org/official/master.m3u8"));
        QCOMPARE(resolvedSpy.first().at(2).toBool(), true);

        // 2. DRM page -> clean non-blocking failure with Turkish message
        failedSpy.clear();
        resolver.resolveUrl(QStringLiteral("http://127.0.0.1:%1/drm-page").arg(port),
                            QStringLiteral("DRM Kanal"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(failedSpy.first().at(0).toString(),
                 QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı"));
        QCOMPARE(failedSpy.first().at(2).toBool(), true);

        // 3. Oversized (> 2 MiB) response aborted cleanly
        failedSpy.clear();
        resolver.resolveUrl(QStringLiteral("http://127.0.0.1:%1/oversized").arg(port),
                            QStringLiteral("Büyük Sayfa"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QVERIFY(failedSpy.first().at(0).toString().contains(QStringLiteral("2 MiB")));

        // 4. Redirect to forbidden file:// scheme blocked
        failedSpy.clear();
        resolver.resolveUrl(QStringLiteral("http://127.0.0.1:%1/evil-redirect").arg(port),
                            QStringLiteral("Kötü Yönlendirme"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QVERIFY(failedSpy.first().at(0).toString().contains(QStringLiteral("Güvensiz yönlendirme")));
    }

    void turkishBroadcasterConfigDataAttrAndYoutubeLiveEmbedExtraction()
    {
        // 1. Unquoted JS config + Turkish ad/reklam URLs filtered out
        const QByteArray jsConfigHtml = QByteArrayLiteral(
            "<html><body><script>\n"
            "var adUrl = 'https://cdn.example.tr/reklam/preroll.m3u8';\n"
            "var adTag2 = 'https://ads.medyanet.example/vast/stream.m3u8';\n"
            "var playerOpts = { autoplay: true, source: 'https://ciner-live.daioncdn.net/showtv/showtv.m3u8?hd=1' };\n"
            "</script></body></html>");
        const StreamExtractResult jsRes = StreamResolver::extractStreamFromHtml(
            jsConfigHtml, QUrl(QStringLiteral("https://www.showtv.com.tr/canli-yayin")), QStringLiteral("text/html"));
        QVERIFY2(jsRes.ok, qPrintable(jsRes.errorMessage));
        QCOMPARE(jsRes.streamUrl, QStringLiteral("https://ciner-live.daioncdn.net/showtv/showtv.m3u8?hd=1"));

        // 2. HTML data-hls with protocol-relative URL resolved safely
        const QByteArray dataAttrHtml = QByteArrayLiteral(
            "<html><body><div id=\"player\" data-hls=\"//dogus-live.daioncdn.net/ntv/ntv.m3u8\"></div></body></html>");
        const StreamExtractResult dataRes = StreamResolver::extractStreamFromHtml(
            dataAttrHtml, QUrl(QStringLiteral("https://www.ntv.com.tr/canli-yayin/ntv")), QStringLiteral("text/html"));
        QVERIFY2(dataRes.ok, qPrintable(dataRes.errorMessage));
        QCOMPARE(dataRes.streamUrl, QStringLiteral("https://dogus-live.daioncdn.net/ntv/ntv.m3u8"));

        // 3. Official YouTube Live iframe embed -> canonical watch URL as secondaryPageUrl
        const QByteArray ytEmbedHtml = QByteArrayLiteral(
            "<html><body><iframe src=\"https://www.youtube.com/embed/AbCdEfGhIjK?autoplay=1&mute=0\"></iframe></body></html>");
        const StreamExtractResult ytRes = StreamResolver::extractStreamFromHtml(
            ytEmbedHtml, QUrl(QStringLiteral("https://www.szctv.com.tr/canli-yayin")), QStringLiteral("text/html"));
        QVERIFY(!ytRes.ok);
        QCOMPARE(ytRes.secondaryPageUrl, QStringLiteral("https://www.youtube.com/watch?v=AbCdEfGhIjK"));

        // 4. Official YouTube live_stream?channel=UC... embed -> canonical channel /live URL
        const QByteArray ytChannelHtml = QByteArrayLiteral(
            "<html><body><iframe src=\"https://www.youtube-nocookie.com/embed/live_stream?channel=UC1234567890ab&autoplay=1\"></iframe></body></html>");
        const StreamExtractResult ytChRes = StreamResolver::extractStreamFromHtml(
            ytChannelHtml, QUrl(QStringLiteral("https://tele1.com.tr/canli-yayin/")), QStringLiteral("text/html"));
        QVERIFY(!ytChRes.ok);
        QCOMPARE(ytChRes.secondaryPageUrl, QStringLiteral("https://www.youtube.com/channel/UC1234567890ab/live"));

        // 5. Forbidden iframe schemes (javascript:, file://) MUST be ignored
        const QByteArray evilIframeHtml = QByteArrayLiteral(
            "<html><body>"
            "<iframe src=\"javascript:alert(1)\"></iframe>"
            "<iframe src=\"file:///etc/passwd\"></iframe>"
            "</body></html>");
        const StreamExtractResult evilRes = StreamResolver::extractStreamFromHtml(
            evilIframeHtml, QUrl(QStringLiteral("https://www.example.tr/canli-yayin")), QStringLiteral("text/html"));
        QVERIFY(!evilRes.ok);
        QVERIFY(evilRes.secondaryPageUrl.isEmpty());
    }

    void asyncMultiHopEmbedAndYoutubeManifestResolutionWithCycleGuard()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, port]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, port]() {
                    const QByteArray req = sock->readAll();
                    const QByteArray firstLine = req.split('\n').value(0);
                    QByteArray body;
                    if (firstLine.contains("GET /broadcaster ")) {
                        body = "<html><body><iframe src=\"http://127.0.0.1:"
                               + QByteArray::number(port)
                               + "/player-embed\"></iframe></body></html>";
                    } else if (firstLine.contains("GET /player-embed ")) {
                        body = "<html><body><script>var ytInitialPlayerResponse = {\"streamingData\":"
                               "{\"hlsManifestUrl\":\"https:\\/\\/manifest.googlevideo.com\\/api\\/manifest\\/hls_variant\\/live\\/file\\/index.m3u8\"}};"
                               "</script></body></html>";
                    } else if (firstLine.contains("GET /loop-a ")) {
                        body = "<html><body><iframe src=\"http://127.0.0.1:"
                               + QByteArray::number(port)
                               + "/loop-b\"></iframe></body></html>";
                    } else if (firstLine.contains("GET /loop-b ")) {
                        body = "<html><body><iframe src=\"http://127.0.0.1:"
                               + QByteArray::number(port)
                               + "/loop-a\"></iframe></body></html>";
                    } else if (firstLine.contains("GET /drm-outer ")) {
                        body = "<html><body><iframe src=\"http://127.0.0.1:"
                               + QByteArray::number(port)
                               + "/drm-inner\"></iframe></body></html>";
                    } else if (firstLine.contains("GET /drm-inner ")) {
                        body = "<html><body><script>var cfg = {\"isDrm\":true,\"keySystem\":\"com.widevine.alpha\"};</script></body></html>";
                    }
                    const QByteArray resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: " + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n" + body;
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
                QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            }
        });

        StreamResolver resolver;
        QSignalSpy resolvedSpy(&resolver, &StreamResolver::streamResolved);
        QSignalSpy failedSpy(&resolver, &StreamResolver::resolveFailed);

        // 1. Two-hop broadcaster -> embedded player -> YouTube hlsManifestUrl
        resolver.resolveUrl(QStringLiteral("http://127.0.0.1:%1/broadcaster").arg(port),
                            QStringLiteral("Sözcü TV"));
        QTRY_COMPARE_WITH_TIMEOUT(resolvedSpy.count(), 1, 5000);
        QCOMPARE(resolvedSpy.first().at(0).toString(),
                 QStringLiteral("https://manifest.googlevideo.com/api/manifest/hls_variant/live/file/index.m3u8"));
        QCOMPARE(resolvedSpy.first().at(2).toBool(), true);

        // 2. Cycle guard stops iframe loops cleanly without hanging
        failedSpy.clear();
        resolver.resolveUrl(QStringLiteral("http://127.0.0.1:%1/loop-a").arg(port),
                            QStringLiteral("Döngü Testi"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QVERIFY(!resolver.resolving());

        // 3. DRM detected inside embedded player iframe returns clean Turkish DRM fallback
        failedSpy.clear();
        resolver.resolveUrl(QStringLiteral("http://127.0.0.1:%1/drm-outer").arg(port),
                            QStringLiteral("DRM Kanal"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QCOMPARE(failedSpy.first().at(0).toString(),
                 QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı"));
        QCOMPARE(failedSpy.first().at(2).toBool(), true);
    }

    void broadcasterApisDemirorenTurkuvazAndYoutubeLiveExtraction()
    {
        // 1. Demirören official media JSON API (CNN Türk ServiceUrl + SecurePath)
        const QByteArray demirorenJson = QByteArrayLiteral(
            "{\"Id\":\"62d6814670380e2cdc7c124c\",\"Media\":{\"Link\":{"
            "\"ServiceUrl\":\"https://live.duhnet.tv\","
            "\"SecurePath\":\"/S2/HLS_LIVE/cnnturknp/playlist.m3u8?&live=true&app=com.cnnturk&st=abc123&e=1799999999\"}}}");
        const StreamExtractResult cnnRes = StreamResolver::extractStreamFromHtml(
            demirorenJson,
            QUrl(QStringLiteral("https://www.cnnturk.com/action/media/62d6814670380e2cdc7c124c")),
            QStringLiteral("application/json"));
        QVERIFY2(cnnRes.ok, qPrintable(cnnRes.errorMessage));
        QCOMPARE(cnnRes.streamUrl,
                 QStringLiteral("https://live.duhnet.tv/S2/HLS_LIVE/cnnturknp/playlist.m3u8?&live=true&app=com.cnnturk&st=abc123&e=1799999999"));

        // 2. Turkuvaz HTML tmdvpcontainer -> videojs.tmgrup.com.tr/getvideo/<ws>/<vid>
        const QByteArray atvHtml = QByteArrayLiteral(
            "<html><body><div data-videoid=\"00000000-0000-0000-0000-000000000000\" "
            "data-vp=\"tmdvpcontainer\" data-websiteid=\"0fe2a405-8afa-4238-b429-e5f96aec3a5c\"></div></body></html>");
        const StreamExtractResult atvStep1 = StreamResolver::extractStreamFromHtml(
            atvHtml, QUrl(QStringLiteral("https://www.atv.com.tr/canli-yayin")), QStringLiteral("text/html"));
        QVERIFY(!atvStep1.ok);
        QCOMPARE(atvStep1.secondaryPageUrl,
                 QStringLiteral("https://videojs.tmgrup.com.tr/getvideo/0fe2a405-8afa-4238-b429-e5f96aec3a5c/00000000-0000-0000-0000-000000000000"));

        // 3. Turkuvaz getvideo JSON -> securevideotoken API
        const QByteArray getVideoJson = QByteArrayLiteral(
            "{\"success\":true,\"video\":{\"VideoUrl\":\"https://trkvz-live.ercdn.net/atvhd/atvhd.m3u8\"}}");
        const StreamExtractResult atvStep2 = StreamResolver::extractStreamFromHtml(
            getVideoJson,
            QUrl(QStringLiteral("https://videojs.tmgrup.com.tr/getvideo/0fe2a405-8afa-4238-b429-e5f96aec3a5c/00000000-0000-0000-0000-000000000000")),
            QStringLiteral("application/json"));
        QVERIFY(!atvStep2.ok);
        QCOMPARE(atvStep2.secondaryPageUrl,
                 QStringLiteral("https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fatvhd%2Fatvhd.m3u8"));

        // 4. Turkuvaz securevideotoken JSON -> signed HLS URL
        const QByteArray tokenJson = QByteArrayLiteral(
            "{\"Success\":true,\"Url\":\"https://trkvz-live.ercdn.net/atvhd/atvhd.m3u8?st=tok123&e=1799999999\"}");
        const StreamExtractResult atvStep3 = StreamResolver::extractStreamFromHtml(
            tokenJson,
            QUrl(QStringLiteral("https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fatvhd%2Fatvhd.m3u8")),
            QStringLiteral("application/json"));
        QVERIFY2(atvStep3.ok, qPrintable(atvStep3.errorMessage));
        QCOMPARE(atvStep3.streamUrl,
                 QStringLiteral("https://trkvz-live.ercdn.net/atvhd/atvhd.m3u8?st=tok123&e=1799999999"));

        // 5. YouTube /@.../live mobile HTML with hlsManifestUrl vs offline YouTube /@.../live
        const QByteArray ytLiveHtml = QByteArrayLiteral(
            "<html><body><script>var ytInitialPlayerResponse={\"playabilityStatus\":{\"status\":\"OK\"},"
            "\"streamingData\":{\"hlsManifestUrl\":\"https:\\/\\/manifest.googlevideo.com\\/api\\/manifest\\/hls_variant\\/d\\/file\\/index.m3u8\"}};</script></body></html>");
        const StreamExtractResult ytLiveRes = StreamResolver::extractStreamFromHtml(
            ytLiveHtml, QUrl(QStringLiteral("https://m.youtube.com/@Sozcutelevizyonu/live")), QStringLiteral("text/html"));
        QVERIFY2(ytLiveRes.ok, qPrintable(ytLiveRes.errorMessage));
        QCOMPARE(ytLiveRes.streamUrl,
                 QStringLiteral("https://manifest.googlevideo.com/api/manifest/hls_variant/d/file/index.m3u8"));

        const QByteArray ytOfflineHtml = QByteArrayLiteral(
            "<html><body><script>var ytInitialPlayerResponse={\"playabilityStatus\":{\"status\":\"LIVE_STREAM_OFFLINE\"}};</script></body></html>");
        const StreamExtractResult ytOffRes = StreamResolver::extractStreamFromHtml(
            ytOfflineHtml, QUrl(QStringLiteral("https://m.youtube.com/@tele1comtr/live")), QStringLiteral("text/html"));
        QVERIFY(!ytOffRes.ok);
        QVERIFY(!ytOffRes.drmDetected);
        QVERIFY(ytOffRes.errorMessage.contains(QStringLiteral("çevrimdışı")));

        // 6. DRM playlist URLs (-hls-drm) are deprioritized below open HLS streams
        const QByteArray tabiiLike = QByteArrayLiteral(
            "<html><body><script>"
            "var a='https://cdn.trt.example/out/tabii-hls-drm/stream.m3u8';"
            "var b='https://tv-trt1.medya.trt.com.tr/master.m3u8';"
            "</script></body></html>");
        const StreamExtractResult tabiiRes = StreamResolver::extractStreamFromHtml(
            tabiiLike, QUrl(QStringLiteral("https://www.tabii.com/tr/watch/live/trt1")), QStringLiteral("text/html"));
        QVERIFY(tabiiRes.ok);
        QCOMPARE(tabiiRes.streamUrl, QStringLiteral("https://tv-trt1.medya.trt.com.tr/master.m3u8"));
    }

    void asyncFallbackChainCascadesAcross404And403ToWorkingStreamAndSurfacesClearError()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock]() {
                    const QByteArray req = sock->readAll();
                    const QByteArray firstLine = req.split('\n').value(0);
                    QByteArray resp;
                    if (firstLine.contains("GET /primary-404 ")) {
                        resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    } else if (firstLine.contains("GET /backup-403 ")) {
                        resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    } else if (firstLine.contains("GET /backup-ok.m3u8 ")) {
                        const QByteArray m3u = "#EXTM3U\n#EXT-X-VERSION:3\nchunk.ts\n";
                        resp = "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: "
                               + QByteArray::number(m3u.size()) + "\r\nConnection: close\r\n\r\n" + m3u;
                    }
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
                QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            }
        });

        StreamResolver resolver;
        QSignalSpy resolvedSpy(&resolver, &StreamResolver::streamResolvedWithHeaders);
        QSignalSpy failedSpy(&resolver, &StreamResolver::resolveFailed);

        // 1. Primary 404 -> Fallback 1 (403) -> Fallback 2 (valid #EXTM3U) succeeds automatically!
        resolver.resolveChannel(
            QString(),
            QStringLiteral("http://127.0.0.1:%1/primary-404").arg(port),
            QStringLiteral("Yedekli Kanal"),
            false,
            {
                QStringLiteral("http://127.0.0.1:%1/backup-403").arg(port),
                QStringLiteral("http://127.0.0.1:%1/backup-ok.m3u8").arg(port),
            });
        QTRY_COMPARE_WITH_TIMEOUT(resolvedSpy.count(), 1, 5000);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(resolvedSpy.first().at(0).toString(),
                 QStringLiteral("http://127.0.0.1:%1/backup-ok.m3u8").arg(port));
        QCOMPARE(resolvedSpy.first().at(3).toString(),
                 QStringLiteral("http://127.0.0.1:%1/").arg(port));

        // 2. All sources fail (404 -> 403) -> concise Turkish HTTP 403 error surfaced cleanly
        failedSpy.clear();
        resolver.resolveChannel(
            QString(),
            QStringLiteral("http://127.0.0.1:%1/primary-404").arg(port),
            QStringLiteral("Kapalı Kanal"),
            true,
            {QStringLiteral("http://127.0.0.1:%1/backup-403").arg(port)});
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
        QVERIFY(failedSpy.first().at(0).toString().contains(QStringLiteral("HTTP 403")));
        QVERIFY(!resolver.resolving());
        QVERIFY(!resolver.hasPendingFallbacks());
    }

    void mpvPlayUrlWithHeadersAppliesSafeRefererAndRejectsUnsafeInputs()
    {
        MpvItem player;
        QVERIFY(player.backendAvailable());

        // Unsafe media URL still rejected
        QVERIFY(!player.playUrlWithHeaders(QStringLiteral("file:///etc/passwd"),
                                           QStringLiteral("https://www.atv.com.tr/")));
        QCOMPARE(player.playbackState(), QStringLiteral("error"));

        // Safe referer applied cleanly; unsafe CRLF / file:// referer stripped to empty
        QVERIFY(player.playUrlWithHeaders(QStringLiteral("https://example.org/test.m3u8"),
                                          QStringLiteral("https://www.atv.com.tr/")));
        QTRY_COMPARE_WITH_TIMEOUT(player.mpvPropertyString(QStringLiteral("referrer")),
                                  QStringLiteral("https://www.atv.com.tr/"), 2000);

        QVERIFY(player.playUrlWithHeaders(QStringLiteral("https://example.org/test2.m3u8"),
                                          QStringLiteral("file:///etc/passwd")));
        QTRY_COMPARE_WITH_TIMEOUT(player.mpvPropertyString(QStringLiteral("referrer")),
                                  QString(), 2000);

        QVERIFY(player.playUrlWithHeaders(QStringLiteral("https://example.org/test3.m3u8"),
                                          QStringLiteral("https://www.atv.com.tr/\r\nX-Inject: 1")));
        QTRY_COMPARE_WITH_TIMEOUT(player.mpvPropertyString(QStringLiteral("referrer")),
                                  QString(), 2000);

        // Mandatory security locks remain intact
        QCOMPARE(player.mpvPropertyString(QStringLiteral("load-scripts")), QStringLiteral("no"));
        QCOMPARE(player.mpvPropertyString(QStringLiteral("ytdl")), QStringLiteral("no"));
        QCOMPARE(player.mpvPropertyString(QStringLiteral("load-unsafe-playlists")), QStringLiteral("no"));
        player.stop();
    }
};

QTEST_MAIN(StreamResolverTest)
#include "StreamResolverTest.moc"
