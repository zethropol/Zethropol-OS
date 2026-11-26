#include "ChannelCatalog.h"
#include "StreamResolver.h"
#include "StreamSecurity.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace zethropol::tvwin7;

namespace {

int g_failed = 0;

void check(bool cond, const char *expr, int line)
{
    if (!cond) {
        std::cerr << "FAIL [line " << line << "]: " << expr << "\n";
        ++g_failed;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void testSecurityAllowlist()
{
    CHECK(StreamSecurity::isAllowedMediaUrl("https://tv-trt1.medya.trt.com.tr/master.m3u8"));
    CHECK(StreamSecurity::isAllowedMediaUrl("http://streams.example.org:8080/live/index.m3u8?k=1"));

    const std::vector<std::string> forbidden = {
        "",
        "file:///etc/passwd",
        "FILE:///C:/Windows/win.ini",
        "exec://cmd.exe",
        "lua://os.execute('calc')",
        "edl://%12%/etc/passwd",
        "javascript:alert(1)",
        "data:text/plain;base64,SGVsbG8=",
        "--script=/opt/evil.lua",
        "https://example.org/a.m3u8\n--script=/opt/x.lua",
        "https://example.org/a%00.m3u8",
        "https://user:pass@example.org/live.m3u8",
        "https://",
        "/etc/passwd",
        "../local.m3u8",
    };
    for (const std::string &bad : forbidden) {
        std::string reason;
        CHECK(!StreamSecurity::isAllowedMediaUrl(bad, &reason));
        CHECK(!reason.empty());
    }
}

void testTurkish31ChannelsAndOptionA()
{
    const auto channels = ChannelCatalog::buildTurkish31Channels();
    CHECK(channels.size() == 31);

    std::map<std::string, int> groupCounts;
    for (const auto &ch : channels) {
        ++groupCounts[ch.group];
        if (!ch.streamUrl.empty()) {
            CHECK(ch.streamUrl.compare(0, 8, "https://") == 0);
            CHECK(StreamSecurity::isAllowedMediaUrl(ch.streamUrl));
        }
        if (!ch.sourcePageUrl.empty()) {
            CHECK(ch.sourcePageUrl.compare(0, 8, "https://") == 0);
            CHECK(StreamSecurity::isAllowedMediaUrl(ch.sourcePageUrl));
        }
        for (const auto &fb : ch.fallbackUrls) {
            CHECK(fb.compare(0, 8, "https://") == 0);
            CHECK(StreamSecurity::isAllowedMediaUrl(fb));
        }
    }

    CHECK(groupCounts["Ulusal"] == 10);
    CHECK(groupCounts["Haber"] == 12);
    CHECK(groupCounts["Spor"] == 2);
    CHECK(groupCounts["Belgesel"] == 2);
    CHECK(groupCounts["Çocuk"] == 4);
    CHECK(groupCounts["Kültür"] == 1);

    // Spot check order & Phase 2.3 channels
    CHECK(channels[0].name == "TRT 1");
    CHECK(channels[10].name == "TRT Haber");
    CHECK(channels[20].name == "Ülke TV");
    CHECK(channels[21].name == "Kanal 24");
    CHECK(channels[25].name == "DMAX");
    CHECK(channels[27].name == "Cartoon Network Türkiye");
    CHECK(channels[27].streamUrl.empty()); // Option A: no raw IP stream
    CHECK(channels[28].name == "MinikaGO");
    CHECK(channels[29].name == "MinikaÇOCUK");
    CHECK(channels[30].name == "TRT 2");

    // Default startup state selects Haber and starts TRT Haber
    PortableState freshState;
    CHECK(freshState.selectedCategory == "Haber");
    CHECK(freshState.hlsBitrateCap == true);
    const int startupIdx = ChannelCatalog::findStartupChannelIndex(channels, freshState);
    CHECK(startupIdx == 10);
    CHECK(channels[startupIdx].name == "TRT Haber");
}

void testStreamResolverTurkishBroadcasters()
{
    // 1. Demirören JSON API (CNN Türk)
    {
        const std::string json =
            R"({"Media":{"Link":{"ServiceUrl":"https://live.duhnet.tv","SecurePath":"/S2/HLS_LIVE/cnnturknp/playlist.m3u8?st=abc"}}})";
        const auto res = StreamResolver::extractStreamFromHtml(
            json, "https://www.cnnturk.com/action/media/62d6814670380e2cdc7c124c", "application/json");
        CHECK(res.ok);
        CHECK(res.streamUrl == "https://live.duhnet.tv/S2/HLS_LIVE/cnnturknp/playlist.m3u8?st=abc");
    }

    // 2. Turkuvaz 3-step chain (ATV / MinikaGO / MinikaÇOCUK): HTML -> getvideo -> securevideotoken -> signed HLS
    {
        const std::string pageHtml =
            R"(<div class="tmdvpcontainer" data-websiteid="11111111-2222-3333-4444-555555555555" data-videoid="aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"></div>)";
        const auto hop1 = StreamResolver::extractStreamFromHtml(
            pageHtml, "https://www.minikago.com.tr/webtv/canli-yayin", "text/html");
        CHECK(!hop1.ok);
        CHECK(hop1.secondaryPageUrl
              == "https://videojs.tmgrup.com.tr/getvideo/11111111-2222-3333-4444-555555555555/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");

        const std::string getVideoJson =
            R"({"video":{"VideoUrl":"https://trkvz-live.ercdn.net/minikago/minikago.m3u8"}})";
        const auto hop2 = StreamResolver::extractStreamFromHtml(
            getVideoJson, hop1.secondaryPageUrl, "application/json");
        CHECK(!hop2.ok);
        CHECK(hop2.secondaryPageUrl
              == "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fminikago%2Fminikago.m3u8");

        const std::string tokenJson =
            R"({"Success":true,"Url":"https://trkvz-live.ercdn.net/minikago/minikago.m3u8?st=signed123&e=999999"})";
        const auto hop3 = StreamResolver::extractStreamFromHtml(
            tokenJson, hop2.secondaryPageUrl, "application/json");
        CHECK(hop3.ok);
        CHECK(hop3.streamUrl == "https://trkvz-live.ercdn.net/minikago/minikago.m3u8?st=signed123&e=999999");
    }

    // 3. YouTube /@.../live mobile Safari hlsManifestUrl extraction
    {
        const std::string ytHtml =
            R"({"streamingData":{"hlsManifestUrl":"https:\/\/manifest.googlevideo.com\/api\/manifest\/hls_variant\/expire\/123\/file\/index.m3u8"}})";
        const auto res = StreamResolver::extractStreamFromHtml(
            ytHtml, "https://m.youtube.com/@trthaber/live", "text/html");
        CHECK(res.ok);
        CHECK(res.streamUrl == "https://manifest.googlevideo.com/api/manifest/hls_variant/expire/123/file/index.m3u8");
    }

    // 4. Ad/tracker filtering + DRM detection
    {
        const std::string adHtml =
            R"(<script>var a = "https://doubleclick.net/ads/preroll.m3u8"; var b = "https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3";</script>)";
        const auto res = StreamResolver::extractStreamFromHtml(
            adHtml, "https://www.ntv.com.tr/canli-yayin/ntv", "text/html");
        CHECK(res.ok);
        CHECK(res.streamUrl == "https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3");

        const std::string drmHtml = R"({"licenseUrl":"https://drm.example.org/widevine","drm":true})";
        const auto drmRes = StreamResolver::extractStreamFromHtml(
            drmHtml, "https://example.org/live", "text/html");
        CHECK(!drmRes.ok);
        CHECK(drmRes.drmDetected);
    }

    // 5. End-to-end session failover: 404 -> 403 -> 200 working backup
    {
        ChannelEntry ch;
        ch.id = "Test.tr";
        ch.name = "Test TV";
        ch.group = "Haber";
        ch.sourcePageUrl = "https://primary.example.org/canli";
        ch.fallbackUrls = {
            "https://blocked.example.org/canli",
            "https://www.youtube.com/@testtv/live",
        };

        auto session = StreamResolver::createSession(ch, false);
        int calls = 0;
        HttpFetcher mockFetcher = [&](const HttpRequest &req) {
            ++calls;
            HttpResponse r;
            r.finalUrl = req.url;
            if (req.url.find("primary.example.org") != std::string::npos) {
                r.ok = false;
                r.httpStatus = 404;
                return r;
            }
            if (req.url.find("blocked.example.org") != std::string::npos) {
                r.ok = false;
                r.httpStatus = 403;
                return r;
            }
            if (req.url.find("m.youtube.com/@testtv/live") != std::string::npos) {
                CHECK(req.cookie == "SOCS=CAI");
                CHECK(req.userAgent.find("iPhone") != std::string::npos);
                r.ok = true;
                r.httpStatus = 200;
                r.contentType = "text/html";
                r.body = R"({"hlsManifestUrl":"https://manifest.googlevideo.com/api/manifest/hls_variant/ok/index.m3u8"})";
                return r;
            }
            return r;
        };

        const auto outcome = StreamResolver::resolveNext(session, mockFetcher);
        CHECK(calls == 3);
        CHECK(outcome.ok);
        CHECK(outcome.streamUrl == "https://manifest.googlevideo.com/api/manifest/hls_variant/ok/index.m3u8");
        CHECK(outcome.httpReferer.empty());
        CHECK(outcome.httpUserAgent.find("iPhone") != std::string::npos);
    }
}

void testPortableStatePersistence()
{
    PortableState s;
    s.selectedCategory = "Ulusal";
    s.lastChannelId = "NTV.tr";
    s.volume = 65;
    s.muted = true;
    s.hlsBitrateCap = false;

    const std::string json = ChannelCatalog::serializeStateJson(s);
    const PortableState parsed = ChannelCatalog::parseStateJson(json);
    CHECK(parsed.selectedCategory == "Ulusal");
    CHECK(parsed.lastChannelId == "NTV.tr");
    CHECK(parsed.volume == 65);
    CHECK(parsed.muted == true);
    CHECK(parsed.hlsBitrateCap == false);

    // Corrupt/invalid category falls back cleanly to Haber
    const PortableState bad = ChannelCatalog::parseStateJson(R"({"selectedCategory":"Unknown","volume":999})");
    CHECK(bad.selectedCategory == "Haber");
    CHECK(bad.volume == 100);
}

} // namespace

int main()
{
    testSecurityAllowlist();
    testTurkish31ChannelsAndOptionA();
    testStreamResolverTurkishBroadcasters();
    testPortableStatePersistence();

    if (g_failed == 0) {
        std::cout << "ALL Win7CoreTest CHECKS PASSED\n";
        return 0;
    }
    std::cerr << g_failed << " CHECK(S) FAILED\n";
    return 1;
}
