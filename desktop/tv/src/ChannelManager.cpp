#include "ChannelManager.h"
#include "StreamResolver.h"
#include "StreamSecurity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <iterator>

namespace zethropol::tv {

namespace {

QString makeStableChannelId(const QString &tvgId, const QString &name, const QString &streamUrl)
{
    const QByteArray material = (tvgId + QLatin1Char('|') + name + QLatin1Char('|') + streamUrl).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(16));
}

QString extractM3uAttribute(const QString &extinfHeader, const QString &attrName)
{
    static const QHash<QString, QRegularExpression> kPatterns = {
        {QStringLiteral("tvg-id"), QRegularExpression(QStringLiteral(R"re(tvg-id="([^"]*)")re"), QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("tvg-name"), QRegularExpression(QStringLiteral(R"re(tvg-name="([^"]*)")re"), QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("tvg-logo"), QRegularExpression(QStringLiteral(R"re(tvg-logo="([^"]*)")re"), QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("group-title"), QRegularExpression(QStringLiteral(R"re(group-title="([^"]*)")re"), QRegularExpression::CaseInsensitiveOption)},
    };
    const auto it = kPatterns.find(attrName);
    if (it == kPatterns.end()) {
        return QString();
    }
    const QRegularExpressionMatch match = it->match(extinfHeader);
    return match.hasMatch() ? match.captured(1) : QString();
}

constexpr int kStateSchemaVersion = 6;

bool isLegacyPhase1Default(const ChannelEntry &ch)
{
    return ch.tvgId.startsWith(QStringLiteral("zethropol.demo."));
}

bool isLegacyPhase2Default(const ChannelEntry &ch)
{
    struct LegacySeed {
        const char *tvgId;
        const char *streamUrl;
        const char *sourcePageUrl;
    };
    static const LegacySeed kPhase2Seeds[] = {
        {"TRT1.tr", "https://tv-trt1.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-1"},
        {"TRTCocuk.tr", "https://tv-trtcocuk.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-cocuk"},
        {"TRTHaber.tr", "https://tv-trthaber.medya.trt.com.tr/master.m3u8", "https://www.trthaber.com/canli-yayin"},
        {"TRTWorld.tr", "https://tv-trtworld.live.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-world"},
        {"AlJazeeraEnglish.qa", "https://live-hls-web-aje-fa.thehlive.com/AJE/index.m3u8", "https://www.aljazeera.com/live"},
        {"France24English.fr", "https://live.france24.com/hls/live/2037218/F24_EN_HI_HLS/master_5000.m3u8", "https://www.france24.com/en/live"},
        {"TRTBelgesel.tr", "https://tv-trtbelgesel.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-belgesel"},
        {"NHKWorldJapan.jp", "https://nhkwlive-xjp.akamaized.net/hls/live/2003458/nhkwlive-xjp-en/index_4M.m3u8", "https://www3.nhk.or.jp/nhkworld/en/live/"},
        {"TRTSpor.tr", "https://tv-trtspor1.medya.trt.com.tr/master.m3u8", "https://www.trtspor.com.tr/canli-yayin"},
        {"TRTSpor2.tr", "https://tv-trtspor2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-spor-yildiz"},
        {"TRT2.tr", "https://tv-trt2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-2"},
        {"TRTMuzik.tr", "https://tv-trtmuzik.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-muzik"},
    };
    for (const LegacySeed &seed : kPhase2Seeds) {
        if (ch.tvgId == QLatin1String(seed.tvgId)) {
            if (ch.streamUrl == QLatin1String(seed.streamUrl)
                || ch.sourcePageUrl == QLatin1String(seed.sourcePageUrl)) {
                return true;
            }
        }
    }
    return false;
}

bool isLegacyPhase3Default(const ChannelEntry &ch)
{
    struct LegacyV3Seed {
        const char *tvgId;
        const char *streamUrl;
        const char *sourcePageUrl;
    };
    static const LegacyV3Seed kPhase3Seeds[] = {
        {"TRT1.tr", "https://tv-trt1.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-1"},
        {"ATV.tr", "https://trkvz-live.daioncdn.net/atv/atv.m3u8", "https://www.atv.com.tr/canli-yayin"},
        {"ShowTV.tr", "https://ciner-live.daioncdn.net/showtv/showtv.m3u8", "https://www.showtv.com.tr/canli-yayin"},
        {"StarTV.tr", "https://dogus-live.daioncdn.net/startv/startv.m3u8", "https://www.startv.com.tr/canli-yayin"},
        {"KanalD.tr", "https://demiroren-live.daioncdn.net/kanald/kanald.m3u8", "https://www.kanald.com.tr/canli-yayin"},
        {"NOW.tr", "", "https://www.nowtv.com.tr/canli-yayin"},
        {"TV8.tr", "https://tv8-live.daioncdn.net/tv8/tv8.m3u8", "https://www.tv8.com.tr/canli-yayin"},
        {"Kanal7.tr", "https://kanal7-live.daioncdn.net/kanal7/kanal7.m3u8", "https://www.kanal7.com/canli-yayin"},
        {"BeyazTV.tr", "https://beyaztv-live.daioncdn.net/beyaztv/beyaztv.m3u8", "https://www.beyaztv.com.tr/canli-yayin/"},
        {"TV85.tr", "https://tv8.daioncdn.net/tv8bucuk/tv8bucuk.m3u8", "https://www.tv8bucuk.com/canli-yayin"},
        {"TRTHaber.tr", "https://tv-trthaber.medya.trt.com.tr/master.m3u8", "https://www.trthaber.com/canli-yayin"},
        {"NTV.tr", "https://dogus-live.daioncdn.net/ntv/ntv.m3u8", "https://www.ntv.com.tr/canli-yayin/ntv"},
        {"CNNTurk.tr", "https://demiroren-live.daioncdn.net/cnnturk/cnnturk.m3u8", "https://www.cnnturk.com/canli-yayin"},
        {"Haberturk.tr", "https://ciner-live.daioncdn.net/haberturktv/haberturktv.m3u8", "https://www.haberturk.com/canliyayin"},
        {"HalkTV.tr", "https://halktv-live.daioncdn.net/halktv/halktv.m3u8", "https://halktv.com.tr/canli-yayin"},
        {"SozcuTV.tr", "", "https://www.szctv.com.tr/canli-yayin"},
        {"AHaber.tr", "https://trkvz-live.daioncdn.net/ahaber/ahaber.m3u8", "https://www.ahaber.com.tr/canli-yayin"},
        {"Tele1.tr", "", "https://tele1.com.tr/canli-yayin/"},
        {"HaberGlobal.tr", "https://haberglobal-live.daioncdn.net/haberglobal/haberglobal.m3u8", "https://haberglobal.com.tr/canli-yayin"},
        {"TGRTHaber.tr", "https://tgrthaber-live.daioncdn.net/tgrthaber/tgrthaber.m3u8", "https://www.tgrthaber.com/canli-yayin"},
        {"TRTSpor.tr", "https://tv-trtspor1.medya.trt.com.tr/master.m3u8", "https://www.trtspor.com.tr/canli-yayin"},
        {"TRTSpor2.tr", "https://tv-trtspor2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-spor-yildiz"},
        {"TRTBelgesel.tr", "https://tv-trtbelgesel.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-belgesel"},
        {"TRTCocuk.tr", "https://tv-trtcocuk.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-cocuk"},
        {"TRT2.tr", "https://tv-trt2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-2"},
    };
    for (const LegacyV3Seed &seed : kPhase3Seeds) {
        if (ch.tvgId == QLatin1String(seed.tvgId)) {
            const bool streamMatches = (seed.streamUrl[0] != '\0' && ch.streamUrl == QLatin1String(seed.streamUrl));
            const bool pageMatches = (ch.sourcePageUrl == QLatin1String(seed.sourcePageUrl));
            if (streamMatches || pageMatches) {
                return true;
            }
        }
    }
    return false;
}

bool isLegacyPhase4Default(const ChannelEntry &ch)
{
    struct LegacyV4Seed {
        const char *tvgId;
        const char *streamUrl;
        const char *sourcePageUrl;
    };
    static const LegacyV4Seed kPhase4Seeds[] = {
        {"TRT1.tr", "https://tv-trt1.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-1"},
        {"ATV.tr", "", "https://www.atv.com.tr/canli-yayin"},
        {"ShowTV.tr", "https://ciner.daioncdn.net/showtv/showtv.m3u8?ce=3&app=4bc856ef-4c68-4a94-bc87-37dfaaa66558", "https://www.showtv.com.tr/canli-yayin"},
        {"StarTV.tr", "https://dogus.daioncdn.net/startv/startv.m3u8?app=startv_web&ce=3", "https://www.startv.com.tr/canli-yayin"},
        {"KanalD.tr", "https://demiroren.daioncdn.net/kanald/kanald.m3u8?app=kanald_web&ce=3", "https://www.kanald.com.tr/canli-yayin"},
        {"NOW.tr", "", "https://www.nowtv.com.tr/canli-yayin"},
        {"TV8.tr", "https://tv8.daioncdn.net/tv8/tv8.m3u8?app=7ddc255a-ef47-4e81-ab14-c0e5f2949788&ce=3", "https://www.tv8.com.tr/canli-yayin"},
        {"Kanal7.tr", "https://kanal7-live.daioncdn.net/kanal7/kanal7.m3u8", "https://www.kanal7.com/canli-izle"},
        {"BeyazTV.tr", "https://beyaztv-live.daioncdn.net/beyaztv/beyaztv.m3u8", "https://www.beyaztv.com.tr/canli-yayin/"},
        {"TV85.tr", "https://tv8.daioncdn.net/tv8bucuk/tv8bucuk.m3u8?app=tv8bucuk_web&ce=3", "https://www.tv8bucuk.com/tv8-5-canli-yayin"},
        {"TRTHaber.tr", "https://tv-trthaber.medya.trt.com.tr/master.m3u8", "https://www.tabii.com/tr/watch/live/trthaber"},
        {"NTV.tr", "https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3", "https://www.ntv.com.tr/canli-yayin/ntv"},
        {"CNNTurk.tr", "", "https://www.cnnturk.com/action/media/62d6814670380e2cdc7c124c"},
        {"Haberturk.tr", "https://ciner.daioncdn.net/haberturktv/haberturktv.m3u8?app=c98ab0b0-50cc-495b-bb37-778e91f5ff5b&ce=3", "https://www.haberturk.com/canliyayin"},
        {"HalkTV.tr", "https://halktv-live.daioncdn.net/halktv/halktv.m3u8", "https://halktv.com.tr/canli-yayin"},
        {"SozcuTV.tr", "", "https://www.youtube.com/@Sozcutelevizyonu/live"},
        {"AHaber.tr", "https://trkvz-live.ercdn.net/ahaberhd/ahaberhd.m3u8", "https://www.ahaber.com.tr/video/canli-yayin"},
        {"Tele1.tr", "", "https://www.tele1.com.tr/canli-yayin/"},
        {"HaberGlobal.tr", "https://tv.ensonhaber.com/haberglobal/haberglobal.m3u8", "https://haberglobal.com/canli-yayin"},
        {"TGRTHaber.tr", "https://tgrthaber-live.daioncdn.net/tgrthaber/tgrthaber.m3u8", "https://www.tgrthaber.com/canli-yayin"},
        {"TRTSpor.tr", "https://tv-trtspor1.medya.trt.com.tr/master.m3u8", "https://www.trtspor.com.tr/canli-yayin-izle/trt-spor"},
        {"TRTSpor2.tr", "https://tv-trtspor2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-spor-yildiz"},
        {"TRTBelgesel.tr", "https://tv-trtbelgesel.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-belgesel"},
        {"TRTCocuk.tr", "https://tv-trtcocuk.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-cocuk"},
        {"TRT2.tr", "https://tv-trt2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-2"},
    };
    for (const LegacyV4Seed &seed : kPhase4Seeds) {
        if (ch.tvgId == QLatin1String(seed.tvgId)) {
            const bool streamMatches = (seed.streamUrl[0] != '\0' && ch.streamUrl == QLatin1String(seed.streamUrl));
            const bool pageMatches = (ch.sourcePageUrl == QLatin1String(seed.sourcePageUrl));
            if (streamMatches || pageMatches) {
                return true;
            }
        }
    }
    return false;
}

bool isLegacyPhase5Default(const ChannelEntry &ch)
{
    struct LegacyV5Seed {
        const char *tvgId;
        const char *streamUrl;
        const char *sourcePageUrl;
    };
    static const LegacyV5Seed kPhase5Seeds[] = {
        {"TRT1.tr", "https://tv-trt1.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-1"},
        {"ATV.tr", "", "https://www.atv.com.tr/canli-yayin"},
        {"ShowTV.tr", "https://ciner.daioncdn.net/showtv/showtv.m3u8?ce=3&app=4bc856ef-4c68-4a94-bc87-37dfaaa66558", "https://www.showtv.com.tr/canli-yayin"},
        {"StarTV.tr", "https://dogus.daioncdn.net/startv/startv.m3u8?app=startv_web&ce=3", "https://www.startv.com.tr/canli-yayin"},
        {"KanalD.tr", "https://demiroren.daioncdn.net/kanald/kanald.m3u8?app=kanald_web&ce=3", "https://www.kanald.com.tr/canli-yayin"},
        {"NOW.tr", "", "https://www.nowtv.com.tr/canli-yayin"},
        {"TV8.tr", "https://tv8.daioncdn.net/tv8/tv8.m3u8?app=7ddc255a-ef47-4e81-ab14-c0e5f2949788&ce=3", "https://www.tv8.com.tr/canli-yayin"},
        {"Kanal7.tr", "https://kanal7-live.daioncdn.net/kanal7/kanal7.m3u8", "https://www.kanal7.com/canli-izle"},
        {"BeyazTV.tr", "https://beyaztv-live.daioncdn.net/beyaztv/beyaztv.m3u8", "https://www.beyaztv.com.tr/canli-yayin/"},
        {"TV85.tr", "https://tv8.daioncdn.net/tv8bucuk/tv8bucuk.m3u8?app=tv8bucuk_web&ce=3", "https://www.tv8bucuk.com/tv8-5-canli-yayin"},
        {"TRTHaber.tr", "https://tv-trthaber.medya.trt.com.tr/master.m3u8", "https://www.tabii.com/tr/watch/live/trthaber"},
        {"NTV.tr", "https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3", "https://www.ntv.com.tr/canli-yayin/ntv"},
        {"CNNTurk.tr", "", "https://www.cnnturk.com/action/media/62d6814670380e2cdc7c124c"},
        {"Haberturk.tr", "https://ciner.daioncdn.net/haberturktv/haberturktv.m3u8?app=c98ab0b0-50cc-495b-bb37-778e91f5ff5b&ce=3", "https://www.haberturk.com/canliyayin"},
        {"HalkTV.tr", "https://halktv-live.daioncdn.net/halktv/halktv.m3u8", "https://halktv.com.tr/canli-yayin"},
        {"SozcuTV.tr", "", "https://www.youtube.com/@Sozcutelevizyonu/live"},
        {"AHaber.tr", "https://trkvz-live.ercdn.net/ahaberhd/ahaberhd.m3u8", "https://www.ahaber.com.tr/video/canli-yayin"},
        {"Tele1.tr", "", "https://www.tele1.com.tr/canli-yayin/"},
        {"HaberGlobal.tr", "https://tv.ensonhaber.com/haberglobal/haberglobal.m3u8", "https://haberglobal.com/canli-yayin"},
        {"TGRTHaber.tr", "https://tgrthaber-live.daioncdn.net/tgrthaber/tgrthaber.m3u8", "https://www.tgrthaber.com/canli-yayin"},
        {"UlkeTV.tr", "https://livetv.radyotvonline.net/kanal7live/ulketv/playlist.m3u8", "https://www.ulketv.com/"},
        {"24TV.tr", "https://kanal24-live.lg.mncdn.com/kanal24_turkmedya/live.m3u8", "https://www.yirmidort.tv/canli-yayin"},
        {"TRTSpor.tr", "https://tv-trtspor1.medya.trt.com.tr/master.m3u8", "https://www.trtspor.com.tr/canli-yayin-izle/trt-spor"},
        {"TRTSpor2.tr", "https://tv-trtspor2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-spor-yildiz"},
        {"TRTBelgesel.tr", "https://tv-trtbelgesel.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-belgesel"},
        {"DMAX.tr", "https://dogus.daioncdn.net/dmax/dmax.m3u8?app=dmax_web&ce=3", "https://www.dmax.com.tr/canli-izle"},
        {"TRTCocuk.tr", "https://tv-trtcocuk.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-cocuk"},
        {"CartoonNetwork.tr", "", "https://www.cartoonnetwork.com.tr/canli-yayin"},
        {"MinikaGO.tr", "", "https://www.minikago.com.tr/webtv/canli-yayin"},
        {"MinikaCocuk.tr", "", "https://www.minikacocuk.com.tr/webtv/canli-yayin"},
        {"TRT2.tr", "https://tv-trt2.medya.trt.com.tr/master.m3u8", "https://www.trtizle.com/canli/tv/trt-2"},
    };
    for (const LegacyV5Seed &seed : kPhase5Seeds) {
        if (ch.tvgId == QLatin1String(seed.tvgId)) {
            const bool streamMatches = (seed.streamUrl[0] != '\0' && ch.streamUrl == QLatin1String(seed.streamUrl));
            const bool pageMatches = (ch.sourcePageUrl == QLatin1String(seed.sourcePageUrl));
            if (streamMatches || pageMatches) {
                return true;
            }
        }
    }
    return false;
}

QVector<ChannelEntry> buildLegalDefaultChannels()
{
    // Curated official open-access broadcast lineup (v6 — 41 channels, live-verified HTTPS endpoints):
    // 1-10 Ulusal, 11-20 Haber, 21-25 Spor, 26-30 Bilim ve Teknoloji,
    // 31-32 Belgesel, 33-35 Çocuk, 36-40 Müzik, 41 Kültür.
    // Every streamUrl, sourcePageUrl, and fallbackUrl passes StreamSecurity::isAllowedMediaUrl().
    struct Seed {
        const char *tvgId;
        const char *name;
        const char *group;
        const char *streamUrl;
        const char *sourcePageUrl;
        const char *fallback1;
        const char *fallback2;
        const char *fallback3;
    };
    static const Seed kSeeds[] = {
        // --- Ulusal Kanallar (1-10) ---
        {
            "TRT1.tr",
            "TRT 1",
            "Ulusal",
            "https://tv-trt1.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-1",
            "https://trt.daioncdn.net/trt-1/master.m3u8?app=clean",
            "",
            "",
        },
        {
            "ATV.tr",
            "ATV",
            "Ulusal",
            "",
            "https://www.atv.com.tr/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fatvhd%2Fatvhd.m3u8",
            "https://www.youtube.com/@atvturkiye/live",
            "",
        },
        {
            "ShowTV.tr",
            "Show TV",
            "Ulusal",
            "https://ciner.daioncdn.net/showtv/showtv.m3u8?ce=3&app=4bc856ef-4c68-4a94-bc87-37dfaaa66558",
            "https://www.showtv.com.tr/canli-yayin",
            "https://www.youtube.com/@showtv/live",
            "",
            "",
        },
        {
            "StarTV.tr",
            "Star TV",
            "Ulusal",
            "https://dogus.daioncdn.net/startv/startv.m3u8?app=startv_web&ce=3",
            "https://www.startv.com.tr/canli-yayin",
            "https://www.youtube.com/@StarTVResmi/live",
            "",
            "",
        },
        {
            "KanalD.tr",
            "Kanal D",
            "Ulusal",
            "https://demiroren.daioncdn.net/kanald/kanald.m3u8?app=kanald_web&ce=3",
            "https://www.kanald.com.tr/canli-yayin",
            "https://kdlive.duhnet.tv/S2/HLS_LIVE/kanalddainp/playlist.m3u8",
            "https://www.youtube.com/@kanald/live",
            "",
        },
        {
            "NOW.tr",
            "NOW",
            "Ulusal",
            "",
            "https://www.nowtv.com.tr/canli-yayin",
            "https://www.youtube.com/@nowtvturkiye/live",
            "",
            "",
        },
        {
            "TV8.tr",
            "TV8",
            "Ulusal",
            "https://tv8.daioncdn.net/tv8/tv8.m3u8?app=7ddc255a-ef47-4e81-ab14-c0e5f2949788&ce=3",
            "https://www.tv8.com.tr/canli-yayin",
            "",
            "",
            "",
        },
        {
            "Kanal7.tr",
            "Kanal 7",
            "Ulusal",
            "https://kanal7-live.daioncdn.net/kanal7/kanal7.m3u8",
            "https://www.kanal7.com/canli-izle",
            "https://www.youtube.com/@Kanal7/live",
            "",
            "",
        },
        {
            "BeyazTV.tr",
            "Beyaz TV",
            "Ulusal",
            "https://beyaztv-live.daioncdn.net/beyaztv/beyaztv.m3u8",
            "https://www.beyaztv.com.tr/canli-yayin/",
            "https://www.youtube.com/@beyaztv/live",
            "",
            "",
        },
        {
            "TV85.tr",
            "TV8.5",
            "Ulusal",
            "https://tv8.daioncdn.net/tv8bucuk/tv8bucuk.m3u8?app=tv8bucuk_web&ce=3",
            "https://www.tv8bucuk.com/tv8-5-canli-yayin",
            "",
            "",
            "",
        },
        // --- Haber Kanalları (11-20) ---
        {
            "TRTHaber.tr",
            "TRT Haber",
            "Haber",
            "https://tv-trthaber.medya.trt.com.tr/master.m3u8",
            "https://www.tabii.com/tr/watch/live/trthaber",
            "https://www.youtube.com/@trthaber/live",
            "",
            "",
        },
        {
            "NTV.tr",
            "NTV",
            "Haber",
            "https://dogus.daioncdn.net/ntv/ntv.m3u8?app=ntv_web&ce=3",
            "https://www.ntv.com.tr/canli-yayin/ntv",
            "https://www.youtube.com/@NTV/live",
            "",
            "",
        },
        {
            "CNNTurk.tr",
            "CNN Türk",
            "Haber",
            "",
            "https://www.cnnturk.com/action/media/62d6814670380e2cdc7c124c",
            "https://www.cnnturk.com/canli-yayin",
            "https://www.youtube.com/@cnnturk/live",
            "",
        },
        {
            "Haberturk.tr",
            "Habertürk",
            "Haber",
            "https://ciner.daioncdn.net/haberturktv/haberturktv.m3u8?app=c98ab0b0-50cc-495b-bb37-778e91f5ff5b&ce=3",
            "https://www.haberturk.com/canliyayin",
            "https://www.youtube.com/@HaberturkTV/live",
            "",
            "",
        },
        {
            "HalkTV.tr",
            "Halk TV",
            "Haber",
            "https://halktv-live.daioncdn.net/halktv/halktv.m3u8",
            "https://halktv.com.tr/canli-yayin",
            "https://www.youtube.com/@halktvcomtr/live",
            "",
            "",
        },
        {
            "AHaber.tr",
            "A Haber",
            "Haber",
            "https://trkvz-live.ercdn.net/ahaberhd/ahaberhd.m3u8",
            "https://www.ahaber.com.tr/video/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fahaberhd%2Fahaberhd.m3u8",
            "https://rnttwmjcin.turknet.ercdn.net/lcpmvefbyo/ahaber/ahaber.m3u8",
            "https://www.youtube.com/@Ahaber/live",
        },
        {
            "HaberGlobal.tr",
            "Haber Global",
            "Haber",
            "https://tv.ensonhaber.com/haberglobal/haberglobal.m3u8",
            "https://haberglobal.com/canli-yayin",
            "https://www.youtube.com/@HaberGlobal/live",
            "",
            "",
        },
        {
            "TGRTHaber.tr",
            "TGRT Haber",
            "Haber",
            "https://tgrthaber-live.daioncdn.net/tgrthaber/tgrthaber.m3u8",
            "https://www.tgrthaber.com/canli-yayin",
            "https://canli.tgrthaber.com/tgrt.m3u8",
            "https://www.youtube.com/@tgrthaber/live",
            "",
        },
        {
            "UlkeTV.tr",
            "Ülke TV",
            "Haber",
            "https://livetv.radyotvonline.net/kanal7live/ulketv/playlist.m3u8",
            "https://www.ulketv.com/",
            "https://www.youtube.com/@ulketv/live",
            "",
            "",
        },
        {
            "24TV.tr",
            "Kanal 24",
            "Haber",
            "https://kanal24-live.lg.mncdn.com/kanal24_turkmedya/live.m3u8",
            "https://www.yirmidort.tv/canli-yayin",
            "https://turkmedya-live.ercdn.net/tv24/tv24.m3u8",
            "https://www.youtube.com/channel/UCN7VYCsI4Lx1-J4_BtjoWUA/live",
            "",
        },
        // --- Spor (21-25) ---
        {
            "TRTSpor.tr",
            "TRT Spor",
            "Spor",
            "https://tv-trtspor1.medya.trt.com.tr/master.m3u8",
            "https://www.trtspor.com.tr/canli-yayin-izle/trt-spor",
            "https://trt.daioncdn.net/trtspor/master.m3u8?app=web&platform=trtspor",
            "",
            "",
        },
        {
            "TRTSpor2.tr",
            "TRT Spor Yıldız",
            "Spor",
            "https://tv-trtspor2.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-spor-yildiz",
            "https://trt.daioncdn.net/trtspor-yildiz/master.m3u8?app=web&platform=trtspor",
            "",
            "",
        },
        {
            "RedBullTV.int",
            "Red Bull TV",
            "Spor",
            "https://rbmn-live.akamaized.net/hls/live/590964/BoRB-AT/master.m3u8",
            "https://www.redbull.com/int-en/live",
            "https://3ea22335.wurl.com/master/f36d25e7e52f1ba8d7e56eb859c636563214f541/UmFrdXRlblRWLWdiX1JlZEJ1bGxUVl9ITFM/playlist.m3u8",
            "https://shls-live-enc.edgenextcdn.net/out/v1/ad35123d4ad041068bce26ae67ee3180/index.m3u8",
            "",
        },
        {
            "FIFAPlus.int",
            "FIFA+",
            "Spor",
            "https://95cedebd.wurl.com/master/f36d25e7e52f1ba8d7e56eb859c636563214f541/Vml6aW9fRklGQVBsdXNFbmdsaXNoX0hMUw/playlist.m3u8",
            "https://www.plus.fifa.com/",
            "https://a62dad94.wurl.com/master/f36d25e7e52f1ba8d7e56eb859c636563214f541/UmFrdXRlblRWLWV1X0ZJRkFQbHVzRW5nbGlzaF9ITFM/playlist.m3u8",
            "https://d2w9q46ikgrcwx.cloudfront.net/v1/master/3722c60a815c199d9c0ef36c5b73da68a62b09d1/cc-of5cbk3sav3w5/v1/sysdata_s_p_a_fifa_7/samsungheadend_us/latest/main/hls/playlist.m3u8",
            "",
        },
        {
            "beINSPORTSTurkiye.tr",
            "beIN SPORTS Türkiye",
            "Spor",
            "",
            "https://www.youtube.com/@beINSPORTSTurkiye/live",
            "https://www.youtube.com/@beINSPORTSTurkiye/videos",
            "",
            "",
        },
        // --- Bilim ve Teknoloji (26-30) ---
        {
            "NASATV.us",
            "NASA TV",
            "Bilim ve Teknoloji",
            "https://ntv1.akamaized.net/hls/live/2014075/NASA-NTV1-HLS/master.m3u8",
            "https://www.youtube.com/@NASA/live",
            "https://ntv2.akamaized.net/hls/live/2013923/NASA-NTV2-HLS/master.m3u8",
            "https://plus.nasa.gov/",
            "",
        },
        {
            "DWPlanetA.de",
            "DW Planet A",
            "Bilim ve Teknoloji",
            "",
            "https://www.youtube.com/@DWPlanetA/live",
            "https://www.youtube.com/@DWPlanetA/videos",
            "https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/index.m3u8",
            "",
        },
        {
            "TeknoSeyir.tr",
            "TeknoSeyir",
            "Bilim ve Teknoloji",
            "",
            "https://www.youtube.com/@teknoseyir/live",
            "https://www.youtube.com/@teknoseyir/streams",
            "https://www.youtube.com/@teknoseyir/videos",
            "",
        },
        {
            "HardwarePlus.tr",
            "Hardware Plus (HWP)",
            "Bilim ve Teknoloji",
            "",
            "https://www.youtube.com/@EnYeniTeknoloji/live",
            "https://www.youtube.com/@EnYeniTeknoloji/videos",
            "",
            "",
        },
        {
            "DonanimHaber.tr",
            "DonanımHaber",
            "Bilim ve Teknoloji",
            "",
            "https://www.youtube.com/@donanimhabertv/live",
            "https://www.youtube.com/@donanimhabertv/videos",
            "",
            "",
        },
        // --- Belgesel (31-32) ---
        {
            "TRTBelgesel.tr",
            "TRT Belgesel",
            "Belgesel",
            "https://tv-trtbelgesel.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-belgesel",
            "",
            "",
            "",
        },
        {
            "DMAX.tr",
            "DMAX",
            "Belgesel",
            "https://dogus.daioncdn.net/dmax/dmax.m3u8?app=dmax_web&ce=3",
            "https://www.dmax.com.tr/canli-izle",
            "",
            "",
            "",
        },
        // --- Çocuk (33-35) ---
        {
            "TRTCocuk.tr",
            "TRT Çocuk",
            "Çocuk",
            "https://tv-trtcocuk.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-cocuk",
            "",
            "",
            "",
        },
        {
            "MinikaGO.tr",
            "MinikaGO",
            "Çocuk",
            "",
            "https://www.minikago.com.tr/webtv/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fminikago%2Fminikago.m3u8",
            "https://rnttwmjcin.turknet.ercdn.net/lcpmvefbyo/minikago/minikago.m3u8",
            "",
        },
        {
            "MinikaCocuk.tr",
            "MinikaÇOCUK",
            "Çocuk",
            "",
            "https://www.minikacocuk.com.tr/webtv/canli-yayin",
            "https://securevideotoken.tmgrup.com.tr/webtv/secure?url=https%3A%2F%2Ftrkvz-live.ercdn.net%2Fminikagococuk%2Fminikagococuk.m3u8",
            "https://rnttwmjcin.turknet.ercdn.net/lcpmvefbyo/minikago_cocuk/minikago_cocuk.m3u8",
            "",
        },
        // --- Müzik (36-40) ---
        {
            "PowerTurkTV.tr",
            "PowerTürk TV",
            "Müzik",
            "https://livetv.powerapp.com.tr/powerturkTV/powerturkhd.smil/playlist.m3u8",
            "https://www.powerapp.com.tr/",
            "https://livetv.powerapp.com.tr/powerturkTV/powerturkhd.smil/master.m3u8",
            "https://live.artidijitalmedya.com/artidijital_powerturktv/powerturktv/playlist.m3u8",
            "",
        },
        {
            "PowerTV.tr",
            "Power TV",
            "Müzik",
            "https://livetv.powerapp.com.tr/powerTV/powerhd.smil/playlist.m3u8",
            "https://www.powerapp.com.tr/",
            "https://livetv.powerapp.com.tr/powerTV/powerhd.smil/master.m3u8",
            "",
            "",
        },
        {
            "KralPopTV.tr",
            "Kral Pop TV",
            "Müzik",
            "https://dogus.daioncdn.net/kralpoptv/kralpoptv.m3u8?app=kralpoptv_web&ce=3",
            "https://www.kralmuzik.com.tr/tv/kral-pop-tv",
            "https://dogus-live.daioncdn.net/kralpoptv/playlist.m3u8",
            "https://dygedge.radyotvonline.net/kralpop/playlist.m3u8",
            "https://www.youtube.com/@KralMuzik/live",
        },
        {
            "Number1TurkTV.tr",
            "Number1 Türk TV",
            "Müzik",
            "https://number1.radyotvonline.net/hls/play/number1_turk_tv.m3u8",
            "https://www.numberone.com.tr/tv/number1-turk-tv",
            "https://b01c02nl.mediatriple.net/videoonlylive/mtkgeuihrlfwlive/broadcast_5c9e187770143.smil/playlist.m3u8",
            "",
            "",
        },
        {
            "Number1TV.tr",
            "Number1 TV",
            "Müzik",
            "https://b01c02nl.mediatriple.net/videoonlylive/mtkgeuihrlfwlive/broadcast_5c9e17cd59e8b.smil/playlist.m3u8",
            "https://www.numberone.com.tr/tv/number1-tv",
            "",
            "",
            "",
        },
        // --- Kültür (41) ---
        {
            "TRT2.tr",
            "TRT 2",
            "Kültür",
            "https://tv-trt2.medya.trt.com.tr/master.m3u8",
            "https://www.trtizle.com/canli/tv/trt-2",
            "",
            "",
            "",
        },
    };

    QVector<ChannelEntry> out;
    out.reserve(static_cast<qsizetype>(std::size(kSeeds)));
    int num = 1;
    for (const Seed &seed : kSeeds) {
        const QString rawStream = QString::fromUtf8(seed.streamUrl).trimmed();
        const QString rawPage = QString::fromUtf8(seed.sourcePageUrl).trimmed();
        const QString validStream = StreamSecurity::isAllowedMediaUrl(rawStream) ? rawStream : QString();
        const QString validPage = StreamSecurity::isAllowedMediaUrl(rawPage) ? rawPage : QString();
        if (validStream.isEmpty() && validPage.isEmpty()) {
            continue;
        }
        ChannelEntry ch;
        ch.number = num++;
        ch.tvgId = QString::fromUtf8(seed.tvgId);
        ch.name = QString::fromUtf8(seed.name);
        ch.group = QString::fromUtf8(seed.group);
        ch.streamUrl = validStream;
        ch.sourcePageUrl = validPage;
        for (const char *fbRaw : {seed.fallback1, seed.fallback2, seed.fallback3}) {
            const QString fb = QString::fromUtf8(fbRaw).trimmed();
            if (!fb.isEmpty() && StreamSecurity::isAllowedMediaUrl(fb) && !ch.fallbackUrls.contains(fb)) {
                ch.fallbackUrls.append(fb);
            }
        }
        ch.id = makeStableChannelId(ch.tvgId, ch.name,
                                    ch.streamUrl.isEmpty() ? ch.sourcePageUrl : ch.streamUrl);
        out.append(ch);
    }
    return out;
}

} // namespace

ChannelManager::ChannelManager(const QString &configDirOverride, QObject *parent)
    : QAbstractListModel(parent)
{
    if (!configDirOverride.isEmpty()) {
        m_configDir = configDirOverride;
    } else {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        m_configDir = base + QStringLiteral("/zethropol/tv");
    }

    loadPersistedState();
    if (m_allChannels.isEmpty()) {
        loadDefaultChannels();
    } else {
        rebuildCategories();
        rebuildFilter();
    }
}

ChannelManager::~ChannelManager()
{
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
    }
}

int ChannelManager::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_filteredIndices.size();
}

QVariant ChannelManager::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_filteredIndices.size()) {
        return {};
    }
    const ChannelEntry &ch = m_allChannels[m_filteredIndices[index.row()]];
    switch (role) {
    case ChannelIdRole:
        return ch.id;
    case NumberRole:
        return ch.number;
    case NameRole:
        return ch.name;
    case GroupRole:
        return ch.group;
    case LogoUrlRole:
        return ch.logoUrl;
    case StreamUrlRole:
        return ch.streamUrl;
    case SourcePageUrlRole:
        return ch.sourcePageUrl;
    case FallbackUrlsRole:
        return ch.fallbackUrls;
    case TvgIdRole:
        return ch.tvgId;
    case FavoriteRole:
        return ch.favorite;
    case ActiveRole:
        return ch.id == m_currentChannelId;
    default:
        return {};
    }
}

QHash<int, QByteArray> ChannelManager::roleNames() const
{
    return {
        {ChannelIdRole, "channelId"},
        {NumberRole, "channelNumber"},
        {NameRole, "channelName"},
        {GroupRole, "channelGroup"},
        {LogoUrlRole, "logoUrl"},
        {StreamUrlRole, "streamUrl"},
        {SourcePageUrlRole, "sourcePageUrl"},
        {FallbackUrlsRole, "fallbackUrls"},
        {TvgIdRole, "tvgId"},
        {FavoriteRole, "favorite"},
        {ActiveRole, "active"},
    };
}

M3uParseResult ChannelManager::parseM3uContent(const QByteArray &rawBytes)
{
    M3uParseResult result;
    if (rawBytes.isEmpty()) {
        result.errorMessage = QStringLiteral("M3U içeriği boş.");
        return result;
    }
    if (rawBytes.size() > StreamSecurity::kMaxM3uBytes) {
        result.errorMessage = QStringLiteral("M3U içeriği 4 MiB güvenlik sınırını aşıyor.");
        return result;
    }

    const QString text = QString::fromUtf8(rawBytes);
    const QStringList lines = text.split(QLatin1Char('\n'));

    bool pendingMeta = false;
    QString pendingName;
    QString pendingGroup;
    QString pendingLogo;
    QString pendingTvgId;
    QSet<QString> seenUrls;

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXTM3U"), Qt::CaseInsensitive)) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXTINF:"), Qt::CaseInsensitive)) {
            pendingMeta = true;
            const int commaPos = line.indexOf(QLatin1Char(','));
            const QString header = (commaPos >= 0) ? line.left(commaPos) : line;
            const QString titleAfterComma = (commaPos >= 0) ? line.mid(commaPos + 1) : QString();

            const QString tvgName = extractM3uAttribute(header, QStringLiteral("tvg-name"));
            pendingName = StreamSecurity::sanitizeDisplayText(
                titleAfterComma.trimmed().isEmpty() ? tvgName : titleAfterComma, 120);
            pendingGroup = StreamSecurity::sanitizeDisplayText(
                extractM3uAttribute(header, QStringLiteral("group-title")), 64);
            if (pendingGroup.isEmpty()) {
                pendingGroup = QStringLiteral("Genel");
            }
            const QString rawLogo = extractM3uAttribute(header, QStringLiteral("tvg-logo")).trimmed();
            pendingLogo = StreamSecurity::isAllowedLogoUrl(rawLogo) ? rawLogo : QString();
            pendingTvgId = StreamSecurity::sanitizeDisplayText(
                extractM3uAttribute(header, QStringLiteral("tvg-id")), 128);
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) {
            // Ignore unknown/untrusted M3U directives (including #EXTVLCOPT / #KODIPROP injection).
            continue;
        }

        // Media or resolver URL line — strictly enforce http/https allowlist (optional resolve://).
        bool wasResolveScheme = false;
        const QString validatedUrl = StreamSecurity::unwrapResolverUrl(line, &wasResolveScheme);
        if (validatedUrl.isEmpty()) {
            ++result.rejectedCount;
            pendingMeta = false;
            pendingName.clear();
            pendingGroup.clear();
            pendingLogo.clear();
            pendingTvgId.clear();
            continue;
        }

        const QString dedupKey = (wasResolveScheme ? QStringLiteral("resolve://") : QString()) + validatedUrl;
        if (seenUrls.contains(dedupKey)) {
            pendingMeta = false;
            continue;
        }
        if (result.channels.size() >= StreamSecurity::kMaxChannels) {
            break;
        }
        seenUrls.insert(dedupKey);

        ChannelEntry ch;
        ch.number = result.channels.size() + 1;
        if (wasResolveScheme) {
            ch.sourcePageUrl = validatedUrl;
        } else {
            ch.streamUrl = validatedUrl;
        }
        if (pendingMeta && !pendingName.isEmpty()) {
            ch.name = pendingName;
            ch.group = pendingGroup.isEmpty() ? QStringLiteral("Genel") : pendingGroup;
            ch.logoUrl = pendingLogo;
            ch.tvgId = pendingTvgId;
        } else {
            const QUrl u(validatedUrl);
            ch.name = StreamSecurity::sanitizeDisplayText(
                u.fileName().isEmpty() ? u.host() : u.fileName(), 120);
            ch.group = QStringLiteral("Genel");
        }
        ch.id = makeStableChannelId(ch.tvgId, ch.name, ch.streamUrl.isEmpty() ? ch.sourcePageUrl : ch.streamUrl);
        result.channels.append(ch);

        pendingMeta = false;
        pendingName.clear();
        pendingGroup.clear();
        pendingLogo.clear();
        pendingTvgId.clear();
    }

    return result;
}

void ChannelManager::setSearchQuery(const QString &query)
{
    const QString cleaned = StreamSecurity::sanitizeDisplayText(query, 80);
    if (m_searchQuery == cleaned) {
        return;
    }
    m_searchQuery = cleaned;
    emit searchQueryChanged();
    rebuildFilter();
}

void ChannelManager::setSelectedCategory(const QString &category)
{
    const QString next = category.trimmed().isEmpty() ? QStringLiteral("Tümü") : category.trimmed();
    if (m_selectedCategory == next) {
        return;
    }
    m_selectedCategory = next;
    emit selectedCategoryChanged();
    rebuildFilter();
}

int ChannelManager::findAllIndexById(const QString &id) const
{
    if (id.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_allChannels.size(); ++i) {
        if (m_allChannels[i].id == id) {
            return i;
        }
    }
    return -1;
}

QString ChannelManager::currentChannelId() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].id : QString();
}

int ChannelManager::currentChannelNumber() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].number : 0;
}

QString ChannelManager::currentChannelName() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].name : QString();
}

QString ChannelManager::currentChannelGroup() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].group : QString();
}

QString ChannelManager::currentChannelStreamUrl() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].streamUrl : QString();
}

QString ChannelManager::currentChannelSourcePageUrl() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].sourcePageUrl : QString();
}

QStringList ChannelManager::currentChannelFallbackUrls() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].fallbackUrls : QStringList();
}

QString ChannelManager::currentChannelLogoUrl() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].logoUrl : QString();
}

QString ChannelManager::currentChannelTvgId() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].tvgId : QString();
}

bool ChannelManager::currentChannelFavorite() const
{
    const int idx = findAllIndexById(m_currentChannelId);
    return (idx >= 0) ? m_allChannels[idx].favorite : false;
}

bool ChannelManager::selectChannel(int filteredIndex)
{
    if (filteredIndex < 0 || filteredIndex >= m_filteredIndices.size()) {
        return false;
    }
    const ChannelEntry &ch = m_allChannels[m_filteredIndices[filteredIndex]];
    return selectChannelById(ch.id);
}

bool ChannelManager::selectChannelById(const QString &channelId)
{
    const int allIdx = findAllIndexById(channelId);
    if (allIdx < 0) {
        return false;
    }
    const ChannelEntry &ch = m_allChannels[allIdx];
    const QString previousId = m_currentChannelId;
    m_currentChannelId = ch.id;

    if (!m_filteredIndices.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_filteredIndices.size() - 1, 0), {ActiveRole});
    }
    if (previousId != m_currentChannelId) {
        emit currentChannelChanged();
        savePersistedState();
    }
    emit channelSelected(ch.streamUrl, ch.name, ch.tvgId);
    return true;
}

bool ChannelManager::nextChannel()
{
    if (m_filteredIndices.isEmpty()) {
        return false;
    }
    int curFiltered = -1;
    for (int i = 0; i < m_filteredIndices.size(); ++i) {
        if (m_allChannels[m_filteredIndices[i]].id == m_currentChannelId) {
            curFiltered = i;
            break;
        }
    }
    const int nextIdx = (curFiltered + 1) % m_filteredIndices.size();
    return selectChannel(nextIdx);
}

bool ChannelManager::previousChannel()
{
    if (m_filteredIndices.isEmpty()) {
        return false;
    }
    int curFiltered = 0;
    for (int i = 0; i < m_filteredIndices.size(); ++i) {
        if (m_allChannels[m_filteredIndices[i]].id == m_currentChannelId) {
            curFiltered = i;
            break;
        }
    }
    const int prevIdx = (curFiltered - 1 + m_filteredIndices.size()) % m_filteredIndices.size();
    return selectChannel(prevIdx);
}

void ChannelManager::toggleFavorite(const QString &channelId)
{
    const int allIdx = findAllIndexById(channelId);
    if (allIdx < 0) {
        return;
    }
    const bool nextFav = !m_allChannels[allIdx].favorite;
    m_allChannels[allIdx].favorite = nextFav;
    if (nextFav) {
        m_favoriteIds.insert(channelId);
    } else {
        m_favoriteIds.remove(channelId);
    }
    savePersistedState();

    if (channelId == m_currentChannelId) {
        emit currentChannelChanged();
    }
    if (m_selectedCategory == QStringLiteral("Favoriler")) {
        rebuildFilter();
    } else {
        for (int row = 0; row < m_filteredIndices.size(); ++row) {
            if (m_filteredIndices[row] == allIdx) {
                emit dataChanged(index(row, 0), index(row, 0), {FavoriteRole});
                break;
            }
        }
    }
}

bool ChannelManager::importM3uFromFile(const QString &pathOrFileUrl, bool replaceExisting)
{
    QString err;
    const QString safePath = StreamSecurity::resolveSafeLocalImportPath(
        pathOrFileUrl, StreamSecurity::kMaxM3uBytes, &err);
    if (safePath.isEmpty()) {
        setStatusMessage(QStringLiteral("M3U dosyası reddedildi: %1").arg(err));
        return false;
    }

    QFile file(safePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusMessage(QStringLiteral("M3U dosyası açılamadı."));
        return false;
    }
    const QByteArray bytes = file.read(StreamSecurity::kMaxM3uBytes + 1);
    if (bytes.size() > StreamSecurity::kMaxM3uBytes) {
        setStatusMessage(QStringLiteral("M3U dosyası 4 MiB güvenlik sınırını aşıyor."));
        return false;
    }

    const M3uParseResult parsed = parseM3uContent(bytes);
    if (!parsed.errorMessage.isEmpty()) {
        setStatusMessage(parsed.errorMessage);
        return false;
    }
    applyParsedChannels(parsed, replaceExisting, QFileInfo(safePath).fileName());
    return !parsed.channels.isEmpty();
}

void ChannelManager::importM3uFromUrl(const QString &url, bool replaceExisting)
{
    QString reason;
    if (!StreamSecurity::isAllowedMediaUrl(url, &reason)) {
        setStatusMessage(QStringLiteral("Güvenlik engeli: %1").arg(reason));
        return;
    }
    startRemoteM3uFetch(url.trimmed(), replaceExisting, 0);
}

bool ChannelManager::addSingleChannel(const QString &name,
                                      const QString &urlOrPage,
                                      const QString &group,
                                      const QString &logoUrl)
{
    bool wasResolveScheme = false;
    QString reason;
    const QString validated = StreamSecurity::unwrapResolverUrl(urlOrPage, &wasResolveScheme, &reason);
    if (validated.isEmpty()) {
        setStatusMessage(QStringLiteral("Güvenlik engeli: %1").arg(reason));
        return false;
    }

    if (m_allChannels.size() >= StreamSecurity::kMaxChannels) {
        setStatusMessage(QStringLiteral("Maksimum kanal sınırına (%1) ulaşıldı.").arg(StreamSecurity::kMaxChannels));
        return false;
    }

    const bool isDirect = !wasResolveScheme && StreamResolver::isDirectStreamUrl(validated);
    for (const ChannelEntry &existing : m_allChannels) {
        if ((isDirect && existing.streamUrl == validated)
            || (!isDirect && existing.sourcePageUrl == validated)) {
            selectChannelById(existing.id);
            setStatusMessage(QStringLiteral("Kanal zaten listede: %1").arg(existing.name));
            return true;
        }
    }

    ChannelEntry ch;
    ch.number = m_allChannels.size() + 1;
    const QString cleanedName = StreamSecurity::sanitizeDisplayText(name, 120);
    if (!cleanedName.isEmpty()) {
        ch.name = cleanedName;
    } else {
        const QUrl parsed(validated);
        ch.name = StreamSecurity::sanitizeDisplayText(parsed.host(), 120);
    }
    const QString cleanedGroup = StreamSecurity::sanitizeDisplayText(group, 64);
    ch.group = cleanedGroup.isEmpty() ? QStringLiteral("Özel") : cleanedGroup;
    ch.logoUrl = StreamSecurity::isAllowedLogoUrl(logoUrl) ? logoUrl.trimmed() : QString();

    if (isDirect) {
        ch.streamUrl = validated;
    } else {
        ch.sourcePageUrl = validated;
    }
    ch.id = makeStableChannelId(ch.tvgId, ch.name, validated);
    ch.favorite = m_favoriteIds.contains(ch.id);

    m_allChannels.append(ch);
    emit totalChannelCountChanged();
    rebuildCategories();
    rebuildFilter();
    savePersistedState();

    setStatusMessage(isDirect
                         ? QStringLiteral("Doğrudan yayın eklendi: %1").arg(ch.name)
                         : QStringLiteral("Resmi yayın sayfası eklendi: %1").arg(ch.name));
    selectChannelById(ch.id);
    return true;
}

void ChannelManager::startRemoteM3uFetch(const QString &url, bool replaceExisting, int redirectDepth)
{
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }

    setLoading(true);
    setStatusMessage(QStringLiteral("M3U kanal listesi indiriliyor..."));
    m_downloadBuffer.clear();
    m_downloadOverflow = false;

    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(StreamSecurity::kNetworkTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Zethropol-TV/0.1"));

    QNetworkReply *reply = m_network.get(req);
    m_activeReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_activeReply != reply) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (m_downloadBuffer.size() + chunk.size() > StreamSecurity::kMaxM3uBytes) {
            m_downloadOverflow = true;
            reply->abort();
            return;
        }
        m_downloadBuffer.append(chunk);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, replaceExisting, redirectDepth]() {
        reply->deleteLater();
        if (m_activeReply != reply) {
            return;
        }
        m_activeReply = nullptr;

        if (m_downloadOverflow) {
            setLoading(false);
            setStatusMessage(QStringLiteral("İndirme durduruldu: M3U boyutu 4 MiB sınırını aştı."));
            return;
        }

        const QVariant redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirectTarget.isValid()) {
            if (redirectDepth >= StreamSecurity::kMaxRedirects) {
                setLoading(false);
                setStatusMessage(QStringLiteral("Çok fazla yönlendirme nedeniyle M3U indirme reddedildi."));
                return;
            }
            const QUrl resolved = reply->url().resolved(redirectTarget.toUrl());
            QString reason;
            if (!StreamSecurity::isAllowedMediaUrl(resolved.toString(), &reason)) {
                setLoading(false);
                setStatusMessage(QStringLiteral("Güvensiz yönlendirme engellendi: %1").arg(reason));
                return;
            }
            startRemoteM3uFetch(resolved.toString(), replaceExisting, redirectDepth + 1);
            return;
        }

        setLoading(false);
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(QStringLiteral("M3U indirilemedi: %1").arg(reply->errorString()));
            return;
        }

        const M3uParseResult parsed = parseM3uContent(m_downloadBuffer);
        if (!parsed.errorMessage.isEmpty()) {
            setStatusMessage(parsed.errorMessage);
            return;
        }
        applyParsedChannels(parsed, replaceExisting, reply->url().host());
    });
}

void ChannelManager::applyParsedChannels(const M3uParseResult &parsed,
                                         bool replaceExisting,
                                         const QString &sourceLabel)
{
    m_lastRejectedUrlCount = parsed.rejectedCount;
    emit lastRejectedUrlCountChanged();

    if (parsed.channels.isEmpty()) {
        setStatusMessage(parsed.rejectedCount > 0
                             ? QStringLiteral("Geçerli kanal bulunamadı (%1 güvensiz URL reddedildi).").arg(parsed.rejectedCount)
                             : QStringLiteral("Kaynakta geçerli http/https kanalı bulunamadı."));
        return;
    }

    if (replaceExisting) {
        m_allChannels.clear();
    }

    QSet<QString> existingUrls;
    for (const ChannelEntry &existing : m_allChannels) {
        if (!existing.streamUrl.isEmpty()) {
            existingUrls.insert(existing.streamUrl);
        }
        if (!existing.sourcePageUrl.isEmpty()) {
            existingUrls.insert(QStringLiteral("page:") + existing.sourcePageUrl);
        }
    }

    int added = 0;
    for (ChannelEntry ch : parsed.channels) {
        const QString key = ch.streamUrl.isEmpty()
            ? (QStringLiteral("page:") + ch.sourcePageUrl)
            : ch.streamUrl;
        if (existingUrls.contains(key)) {
            continue;
        }
        if (m_allChannels.size() >= StreamSecurity::kMaxChannels) {
            break;
        }
        existingUrls.insert(key);
        ch.number = m_allChannels.size() + 1;
        ch.favorite = m_favoriteIds.contains(ch.id);
        m_allChannels.append(ch);
        ++added;
    }

    emit totalChannelCountChanged();
    rebuildCategories();
    rebuildFilter();
    savePersistedState();

    QString summary = QStringLiteral("%1 kaynağından %2 kanal eklendi.").arg(sourceLabel).arg(added);
    if (parsed.rejectedCount > 0) {
        summary += QStringLiteral(" (%1 güvensiz adres engellendi)").arg(parsed.rejectedCount);
    }
    setStatusMessage(summary);
}

void ChannelManager::loadDefaultChannels()
{
    m_allChannels = buildLegalDefaultChannels();
    for (ChannelEntry &ch : m_allChannels) {
        ch.favorite = m_favoriteIds.contains(ch.id);
    }
    emit totalChannelCountChanged();
    rebuildCategories();
    rebuildFilter();
    setStatusMessage(QStringLiteral("41 kanallı resmi açık yayın listesi yüklendi."));
}

void ChannelManager::resetToDefaultChannels()
{
    m_currentChannelId.clear();
    loadDefaultChannels();
    emit currentChannelChanged();
    savePersistedState();
}

QString ChannelManager::stateFilePath() const
{
    return m_configDir + QStringLiteral("/state.json");
}

void ChannelManager::loadPersistedState()
{
    QFile file(stateFilePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray bytes = file.read(StreamSecurity::kMaxM3uBytes);
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) {
        return;
    }

    const QJsonObject root = doc.object();
    const int schemaVersion = root.value(QStringLiteral("version")).toInt(1);
    const QString lastId = root.value(QStringLiteral("lastChannelId")).toString().trimmed();

    const QJsonArray favs = root.value(QStringLiteral("favorites")).toArray();
    for (const QJsonValue &val : favs) {
        const QString id = val.toString().trimmed();
        if (!id.isEmpty()) {
            m_favoriteIds.insert(id);
        }
    }

    const QJsonArray chArr = root.value(QStringLiteral("channels")).toArray();
    QSet<QString> seenKeys;
    bool hasLegacyDefaults = chArr.isEmpty();
    QVector<ChannelEntry> customChannels;
    QSet<QString> favoriteTvgIds;
    QSet<QString> legacyIdsToDrop;
    QString remappedLastId = lastId;
    QString lastSelectedTvgId;

    for (const QJsonValue &val : chArr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        const QString rawStream = obj.value(QStringLiteral("streamUrl")).toString().trimmed();
        const QString rawPage = obj.value(QStringLiteral("sourcePageUrl")).toString().trimmed();

        const QString validStream = StreamSecurity::isAllowedMediaUrl(rawStream) ? rawStream : QString();
        const QString validPage = StreamSecurity::isAllowedMediaUrl(rawPage) ? rawPage : QString();
        if (validStream.isEmpty() && validPage.isEmpty()) {
            continue;
        }

        const QString dedupKey = validStream.isEmpty()
            ? (QStringLiteral("page:") + validPage)
            : validStream;
        if (seenKeys.contains(dedupKey)) {
            continue;
        }
        if (m_allChannels.size() >= StreamSecurity::kMaxChannels) {
            break;
        }
        seenKeys.insert(dedupKey);

        ChannelEntry ch;
        ch.number = m_allChannels.size() + 1;
        ch.name = StreamSecurity::sanitizeDisplayText(obj.value(QStringLiteral("name")).toString(), 120);
        if (ch.name.isEmpty()) {
            ch.name = QStringLiteral("Kanal %1").arg(ch.number);
        }
        ch.group = StreamSecurity::sanitizeDisplayText(obj.value(QStringLiteral("group")).toString(), 64);
        if (ch.group.isEmpty()) {
            ch.group = QStringLiteral("Genel");
        }
        const QString logo = obj.value(QStringLiteral("logoUrl")).toString().trimmed();
        ch.logoUrl = StreamSecurity::isAllowedLogoUrl(logo) ? logo : QString();
        ch.streamUrl = validStream;
        ch.sourcePageUrl = validPage;
        const QJsonArray fbArr = obj.value(QStringLiteral("fallbackUrls")).toArray();
        for (const QJsonValue &fbVal : fbArr) {
            const QString fb = fbVal.toString().trimmed();
            if (StreamSecurity::isAllowedMediaUrl(fb) && !ch.fallbackUrls.contains(fb)) {
                ch.fallbackUrls.append(fb);
            }
        }
        ch.tvgId = StreamSecurity::sanitizeDisplayText(obj.value(QStringLiteral("tvgId")).toString(), 128);

        const QString persistedId = obj.value(QStringLiteral("id")).toString().trimmed();
        ch.id = makeStableChannelId(ch.tvgId, ch.name,
                                    ch.streamUrl.isEmpty() ? ch.sourcePageUrl : ch.streamUrl);

        const bool wasFavorited = m_favoriteIds.contains(ch.id)
                                  || (!persistedId.isEmpty() && m_favoriteIds.contains(persistedId));
        if (wasFavorited) {
            m_favoriteIds.insert(ch.id);
            if (!persistedId.isEmpty() && persistedId != ch.id) {
                m_favoriteIds.remove(persistedId);
            }
            if (!ch.tvgId.isEmpty()) {
                favoriteTvgIds.insert(ch.tvgId);
            }
        }
        ch.favorite = wasFavorited;

        if (!lastId.isEmpty() && (lastId == ch.id || (!persistedId.isEmpty() && lastId == persistedId))) {
            remappedLastId = ch.id;
            lastSelectedTvgId = ch.tvgId;
        }

        const bool isLegacy = isLegacyPhase1Default(ch)
                              || isLegacyPhase2Default(ch)
                              || isLegacyPhase3Default(ch)
                              || isLegacyPhase4Default(ch)
                              || isLegacyPhase5Default(ch);
        if (isLegacy) {
            hasLegacyDefaults = true;
            legacyIdsToDrop.insert(ch.id);
            if (!persistedId.isEmpty()) {
                legacyIdsToDrop.insert(persistedId);
            }
        } else {
            customChannels.append(ch);
        }
        m_allChannels.append(ch);
    }

    // Automatically upgrade Phase 1 (v1) and Phase 2 (v2, including foreign channels)
    // state.json files to the Turkish-only 25-channel v3 default lineup while preserving
    // any user-added custom channels and favorites.
    if (schemaVersion < kStateSchemaVersion) {
        if (hasLegacyDefaults) {
            QVector<ChannelEntry> upgraded = buildLegalDefaultChannels();
            QSet<QString> upgradedKeys;
            QSet<QString> keptIds;

            for (ChannelEntry &ch : upgraded) {
                if (!ch.streamUrl.isEmpty()) {
                    upgradedKeys.insert(ch.streamUrl);
                }
                if (!ch.sourcePageUrl.isEmpty()) {
                    upgradedKeys.insert(QStringLiteral("page:") + ch.sourcePageUrl);
                }
                const bool favMatch = m_favoriteIds.contains(ch.id)
                                      || (!ch.tvgId.isEmpty() && favoriteTvgIds.contains(ch.tvgId));
                if (favMatch) {
                    ch.favorite = true;
                    m_favoriteIds.insert(ch.id);
                    keptIds.insert(ch.id);
                }
            }

            // Drop favorite IDs that belonged strictly to removed legacy demo/foreign channels.
            for (const QString &oldLegacyId : legacyIdsToDrop) {
                if (!keptIds.contains(oldLegacyId)) {
                    m_favoriteIds.remove(oldLegacyId);
                }
            }

            // Append user-added custom channels after the 25 Turkish defaults.
            for (ChannelEntry custom : customChannels) {
                if (upgraded.size() >= StreamSecurity::kMaxChannels) {
                    break;
                }
                const QString streamKey = custom.streamUrl;
                const QString pageKey = custom.sourcePageUrl.isEmpty()
                    ? QString()
                    : (QStringLiteral("page:") + custom.sourcePageUrl);
                if ((!streamKey.isEmpty() && upgradedKeys.contains(streamKey))
                    || (!pageKey.isEmpty() && upgradedKeys.contains(pageKey))) {
                    continue;
                }
                if (!streamKey.isEmpty()) {
                    upgradedKeys.insert(streamKey);
                }
                if (!pageKey.isEmpty()) {
                    upgradedKeys.insert(pageKey);
                }
                custom.number = upgraded.size() + 1;
                custom.favorite = m_favoriteIds.contains(custom.id);
                upgraded.append(custom);
            }

            m_allChannels = upgraded;
            m_currentChannelId.clear();
            if (!remappedLastId.isEmpty() && findAllIndexById(remappedLastId) >= 0) {
                m_currentChannelId = remappedLastId;
            } else if (!lastSelectedTvgId.isEmpty()) {
                for (const ChannelEntry &ch : m_allChannels) {
                    if (ch.tvgId == lastSelectedTvgId) {
                        m_currentChannelId = ch.id;
                        break;
                    }
                }
            }
            setStatusMessage(QStringLiteral("41 kanallı varsayılan yayın listesine yükseltildi."));
            savePersistedState();
            return;
        }

        // User replaced the entire list with custom channels in v1/v2: preserve as-is and bump schema to v3.
        if (!remappedLastId.isEmpty() && findAllIndexById(remappedLastId) >= 0) {
            m_currentChannelId = remappedLastId;
        }
        savePersistedState();
        return;
    }

    if (!remappedLastId.isEmpty() && findAllIndexById(remappedLastId) >= 0) {
        m_currentChannelId = remappedLastId;
    }
}

void ChannelManager::savePersistedState() const
{
    QDir dir(m_configDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return;
    }

    QJsonArray favArr;
    for (const QString &id : m_favoriteIds) {
        favArr.append(id);
    }

    QJsonArray chArr;
    for (const ChannelEntry &ch : m_allChannels) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), ch.id);
        obj.insert(QStringLiteral("name"), ch.name);
        obj.insert(QStringLiteral("group"), ch.group);
        obj.insert(QStringLiteral("logoUrl"), ch.logoUrl);
        obj.insert(QStringLiteral("streamUrl"), ch.streamUrl);
        obj.insert(QStringLiteral("sourcePageUrl"), ch.sourcePageUrl);
        QJsonArray fbArr;
        for (const QString &fb : ch.fallbackUrls) {
            fbArr.append(fb);
        }
        obj.insert(QStringLiteral("fallbackUrls"), fbArr);
        obj.insert(QStringLiteral("tvgId"), ch.tvgId);
        chArr.append(obj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kStateSchemaVersion);
    root.insert(QStringLiteral("favorites"), favArr);
    root.insert(QStringLiteral("lastChannelId"), m_currentChannelId);
    root.insert(QStringLiteral("channels"), chArr);

    QSaveFile saveFile(stateFilePath());
    if (!saveFile.open(QIODevice::WriteOnly)) {
        return;
    }
    saveFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    saveFile.commit();
}

void ChannelManager::rebuildCategories()
{
    QStringList cats;
    cats << QStringLiteral("Tümü") << QStringLiteral("Favoriler");

    QSet<QString> seen;
    for (const ChannelEntry &ch : m_allChannels) {
        const QString grp = ch.group.isEmpty() ? QStringLiteral("Genel") : ch.group;
        if (!seen.contains(grp) && grp != QStringLiteral("Tümü") && grp != QStringLiteral("Favoriler")) {
            seen.insert(grp);
            cats.append(grp);
        }
    }

    if (m_categories != cats) {
        m_categories = cats;
        emit categoriesChanged();
    }
    if (!m_categories.contains(m_selectedCategory)) {
        m_selectedCategory = QStringLiteral("Tümü");
        emit selectedCategoryChanged();
    }
}

void ChannelManager::rebuildFilter()
{
    beginResetModel();
    m_filteredIndices.clear();

    const QString needle = m_searchQuery.toLower();
    for (int i = 0; i < m_allChannels.size(); ++i) {
        const ChannelEntry &ch = m_allChannels[i];
        if (m_selectedCategory == QStringLiteral("Favoriler")) {
            if (!ch.favorite) {
                continue;
            }
        } else if (m_selectedCategory != QStringLiteral("Tümü")) {
            if (ch.group != m_selectedCategory) {
                continue;
            }
        }

        if (!needle.isEmpty()) {
            const bool matchName = ch.name.toLower().contains(needle);
            const bool matchGroup = ch.group.toLower().contains(needle);
            const bool matchNum = QString::number(ch.number).contains(needle);
            if (!matchName && !matchGroup && !matchNum) {
                continue;
            }
        }
        m_filteredIndices.append(i);
    }
    endResetModel();
    emit filteredChannelCountChanged();
}

void ChannelManager::setLoading(bool loading)
{
    if (m_loading != loading) {
        m_loading = loading;
        emit loadingChanged();
    }
}

void ChannelManager::setStatusMessage(const QString &msg)
{
    if (m_statusMessage != msg) {
        m_statusMessage = msg;
        emit statusMessageChanged();
    }
}

} // namespace zethropol::tv
