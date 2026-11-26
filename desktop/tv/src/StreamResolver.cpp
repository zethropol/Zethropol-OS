#include "StreamResolver.h"
#include "StreamSecurity.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace zethropol::tv {

namespace {

constexpr const char *kDesktopBrowserUa =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
constexpr const char *kMobileSafariUa =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Mobile/15E148 Safari/604.1";

bool isYoutubeHost(const QString &host)
{
    const QString h = host.toLower();
    return h == QLatin1String("youtube.com") || h.endsWith(QStringLiteral(".youtube.com"))
           || h == QLatin1String("youtube-nocookie.com")
           || h.endsWith(QStringLiteral(".youtube-nocookie.com"));
}

QString extractSafeOriginReferer(const QString &pageUrl)
{
    if (!StreamSecurity::isAllowedMediaUrl(pageUrl)) {
        return QString();
    }
    const QUrl u(pageUrl.trimmed(), QUrl::StrictMode);
    if (!u.isValid() || u.host().isEmpty() || isYoutubeHost(u.host())) {
        return QString();
    }
    QUrl origin;
    origin.setScheme(u.scheme());
    origin.setHost(u.host());
    if (u.port() > 0) {
        origin.setPort(u.port());
    }
    origin.setPath(QStringLiteral("/"));
    const QString str = origin.toString();
    return StreamSecurity::isAllowedMediaUrl(str) ? str : QString();
}

bool isAdOrTrackerUrl(const QString &url)
{
    const QString lower = url.toLower();
    static const QStringList kAdTokens = {
        QStringLiteral("doubleclick.net"),
        QStringLiteral("googlesyndication"),
        QStringLiteral("googleadservices"),
        QStringLiteral("imasdk.googleapis.com"),
        QStringLiteral("adservice."),
        QStringLiteral("adsystem."),
        QStringLiteral("adserver."),
        QStringLiteral("adbreak"),
        QStringLiteral("taboola."),
        QStringLiteral("outbrain."),
        QStringLiteral("adnxs.com"),
        QStringLiteral("pubmatic.com"),
        QStringLiteral("rubiconproject.com"),
        QStringLiteral("criteo."),
        QStringLiteral("mgid.com"),
        QStringLiteral("medyanet"),
        QStringLiteral("virgul.com"),
        QStringLiteral("reklam"),
        QStringLiteral("/ads/"),
        QStringLiteral("/ad/"),
        QStringLiteral("vast"),
        QStringLiteral("vmap"),
        QStringLiteral("preroll"),
        QStringLiteral("midroll"),
        QStringLiteral("postroll"),
        QStringLiteral("bumper"),
        QStringLiteral("tracker"),
        QStringLiteral("pixel."),
        QStringLiteral("telemetry"),
    };
    for (const QString &tok : kAdTokens) {
        if (lower.contains(tok)) {
            return true;
        }
    }
    return false;
}

bool isSocialOrWidgetUrl(const QString &url)
{
    const QString lower = url.toLower();
    static const QStringList kWidgetTokens = {
        QStringLiteral("facebook.com"),
        QStringLiteral("fbcdn.net"),
        QStringLiteral("twitter.com"),
        QStringLiteral("platform.twitter"),
        QStringLiteral("instagram.com"),
        QStringLiteral("googletagmanager.com"),
        QStringLiteral("google.com/recaptcha"),
        QStringLiteral("google.com/maps"),
        QStringLiteral("recaptcha.net"),
        QStringLiteral("hcaptcha.com"),
        QStringLiteral("whatsapp.com"),
        QStringLiteral("t.me/"),
        QStringLiteral("disqus.com"),
        QStringLiteral("cookielaw.org"),
        QStringLiteral("onetrust.com"),
    };
    for (const QString &tok : kWidgetTokens) {
        if (lower.contains(tok)) {
            return true;
        }
    }
    return false;
}

QString unescapeCandidateUrl(QString raw)
{
    raw = raw.trimmed();
    raw.replace(QStringLiteral("\\/"), QStringLiteral("/"));
    raw.replace(QStringLiteral("\\u002F"), QStringLiteral("/"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("\\u0026"), QStringLiteral("&"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("\\u003D"), QStringLiteral("="), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("\\u003F"), QStringLiteral("?"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("\\u003A"), QStringLiteral(":"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("&#x2F;"), QStringLiteral("/"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("&#47;"), QStringLiteral("/"));
    raw.replace(QStringLiteral("&#x3A;"), QStringLiteral(":"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("&#x3F;"), QStringLiteral("?"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("&#x3D;"), QStringLiteral("="), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("&#x26;"), QStringLiteral("&"), Qt::CaseInsensitive);
    raw.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);

    // Strip any trailing punctuation left by inline JS/HTML delimiters.
    while (!raw.isEmpty() && (raw.endsWith(QLatin1Char('\\')) || raw.endsWith(QLatin1Char(';'))
                              || raw.endsWith(QLatin1Char(',')) || raw.endsWith(QLatin1Char(')'))
                              || raw.endsWith(QLatin1Char(']')) || raw.endsWith(QLatin1Char('}'))
                              || raw.endsWith(QLatin1Char('\'')) || raw.endsWith(QLatin1Char('"')))) {
        raw.chop(1);
    }
    return raw.trimmed();
}

QString resolveSafeCandidateUrl(const QString &cleaned, const QUrl &baseUrl)
{
    if (cleaned.isEmpty()) {
        return QString();
    }
    QString resolvedStr = cleaned;
    if (cleaned.startsWith(QStringLiteral("//"))) {
        const QString scheme = (baseUrl.isValid() && !baseUrl.scheme().isEmpty())
            ? baseUrl.scheme()
            : QStringLiteral("https");
        resolvedStr = scheme + QLatin1Char(':') + cleaned;
    } else if (!cleaned.contains(QStringLiteral("://"))) {
        // Allow only safe relative URLs without explicit forbidden schemes (e.g. `javascript:`, `data:`).
        const int colonIdx = cleaned.indexOf(QLatin1Char(':'));
        const int slashIdx = cleaned.indexOf(QLatin1Char('/'));
        if (colonIdx >= 0 && (slashIdx < 0 || colonIdx < slashIdx)) {
            return QString();
        }
        if (baseUrl.isValid()) {
            resolvedStr = baseUrl.resolved(QUrl(cleaned)).toString();
        }
    }
    return StreamSecurity::isAllowedMediaUrl(resolvedStr) ? resolvedStr : QString();
}

bool isSameSiteOrSubdomain(const QString &hostA, const QString &hostB)
{
    const QString a = hostA.toLower();
    const QString b = hostB.toLower();
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    const QStringList partsA = a.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const QStringList partsB = b.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (partsA.size() < 2 || partsB.size() < 2) {
        return false;
    }
    auto registrableSuffix = [](const QStringList &parts) {
        const int n = parts.size();
        if (n >= 3 && parts[n - 1].size() == 2
            && (parts[n - 2] == QLatin1String("com") || parts[n - 2] == QLatin1String("org")
                || parts[n - 2] == QLatin1String("net") || parts[n - 2] == QLatin1String("gov"))) {
            return parts[n - 3] + QLatin1Char('.') + parts[n - 2] + QLatin1Char('.') + parts[n - 1];
        }
        return parts[n - 2] + QLatin1Char('.') + parts[n - 1];
    };
    return registrableSuffix(partsA) == registrableSuffix(partsB);
}

QString extractYoutubeLiveTarget(const QString &text, const QUrl &baseUrl)
{
    const QString baseHost = baseUrl.host().toLower();
    const bool alreadyOnYoutube = baseHost.endsWith(QStringLiteral("youtube.com"))
                                  || baseHost.endsWith(QStringLiteral("youtube-nocookie.com"));

    // 1. Official live_stream embed by channel ID: youtube.com/embed/live_stream?channel=UC...
    static const QRegularExpression kYtLiveChannelEmbed(
        QStringLiteral(R"re((?:youtube\.com|youtube-nocookie\.com)(?:\\?/)embed(?:\\?/)live_stream\?[^"'<>\s]*\bchannel=(UC[A-Za-z0-9_-]{10,}))re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        const auto m = kYtLiveChannelEmbed.match(text);
        if (m.hasMatch()) {
            const QString target = QStringLiteral("https://www.youtube.com/channel/%1/live").arg(m.captured(1));
            if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrl.toString()) {
                return target;
            }
        }
    }

    // 2. Official YouTube iframe video embed: youtube(-nocookie).com/embed/<11-char-id>
    static const QRegularExpression kYtVideoEmbed(
        QStringLiteral(R"re((?:youtube\.com|youtube-nocookie\.com)(?:\\?/)embed(?:\\?/)([A-Za-z0-9_-]{11})(?![A-Za-z0-9_-]))re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kYtVideoEmbed.globalMatch(text);
        while (it.hasNext()) {
            const QString videoId = it.next().captured(1);
            if (videoId.compare(QStringLiteral("videoseries"), Qt::CaseInsensitive) == 0) {
                continue;
            }
            const QString target = QStringLiteral("https://www.youtube.com/watch?v=%1").arg(videoId);
            if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrl.toString()) {
                return target;
            }
        }
    }

    // 3. Direct official YouTube channel /live reference on a broadcaster's live page
    if (!alreadyOnYoutube) {
        static const QRegularExpression kYtChannelLive(
            QStringLiteral(R"re(https?:\\?/\\?/(?:www\.|m\.)?youtube\.com(?:\\?/)(channel(?:\\?/)UC[A-Za-z0-9_-]{10,}|@[A-Za-z0-9._-]+)(?:\\?/)live\b)re"),
            QRegularExpression::CaseInsensitiveOption);
        const auto m = kYtChannelLive.match(text);
        if (m.hasMatch()) {
            QString pathPart = unescapeCandidateUrl(m.captured(1));
            const QString target = QStringLiteral("https://www.youtube.com/%1/live").arg(pathPart);
            if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrl.toString()) {
                return target;
            }
        }

        // 4. Direct youtube.com/watch?v=<id> link inside broadcaster live player config
        static const QRegularExpression kYtWatchLink(
            QStringLiteral(R"re(https?:\\?/\\?/(?:www\.|m\.)?youtube\.com(?:\\?/)watch\?[^"'<>\s]*\bv=([A-Za-z0-9_-]{11})(?![A-Za-z0-9_-]))re"),
            QRegularExpression::CaseInsensitiveOption);
        const auto wm = kYtWatchLink.match(text);
        if (wm.hasMatch()) {
            const QString target = QStringLiteral("https://www.youtube.com/watch?v=%1").arg(wm.captured(1));
            if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrl.toString()) {
                return target;
            }
        }
    }

    return QString();
}

QString extractBroadcasterPlayerIframe(const QString &text, const QUrl &baseUrl)
{
    static const QRegularExpression kIframeRegex(
        QStringLiteral(R"re(<iframe\b[^>]*\b(?:src|data-src)\s*=\s*["']([^"'<>\s]+)["'])re"),
        QRegularExpression::CaseInsensitiveOption);

    QString bestUrl;
    int bestScore = -1;
    auto it = kIframeRegex.globalMatch(text);
    while (it.hasNext()) {
        const QString cleaned = unescapeCandidateUrl(it.next().captured(1));
        const QString resolved = resolveSafeCandidateUrl(cleaned, baseUrl);
        if (resolved.isEmpty() || resolved == baseUrl.toString()) {
            continue;
        }
        if (isAdOrTrackerUrl(resolved) || isSocialOrWidgetUrl(resolved)) {
            continue;
        }

        const QUrl parsed(resolved, QUrl::StrictMode);
        const QString lower = resolved.toLower();
        const bool sameSite = isSameSiteOrSubdomain(parsed.host(), baseUrl.host());
        const bool looksLikePlayer = lower.contains(QStringLiteral("player"))
                                     || lower.contains(QStringLiteral("embed"))
                                     || lower.contains(QStringLiteral("canli"))
                                     || lower.contains(QStringLiteral("live"))
                                     || lower.contains(QStringLiteral("yayin"))
                                     || lower.contains(QStringLiteral("stream"));
        if (!sameSite && !looksLikePlayer) {
            continue;
        }

        int score = 0;
        if (sameSite) {
            score += 40;
        }
        if (looksLikePlayer) {
            score += 30;
        }
        if (lower.startsWith(QStringLiteral("https://"))) {
            score += 10;
        }
        if (score > bestScore) {
            bestScore = score;
            bestUrl = resolved;
        }
    }
    return bestUrl;
}

int scoreCandidateUrl(const QString &url, int sourcePriority)
{
    int score = sourcePriority;
    const QString lower = url.toLower();
    if (lower.startsWith(QStringLiteral("https://"))) {
        score += 20;
    }
    if (lower.contains(QStringLiteral("master.m3u8"))) {
        score += 40;
    } else if (lower.contains(QStringLiteral("playlist.m3u8"))
               || lower.contains(QStringLiteral("index.m3u8"))
               || lower.contains(QStringLiteral("hls_variant"))) {
        score += 25;
    } else if (lower.contains(QStringLiteral(".m3u8"))) {
        score += 15;
    }
    if (lower.contains(QStringLiteral("live")) || lower.contains(QStringLiteral("hls"))
        || lower.contains(QStringLiteral("canli")) || lower.contains(QStringLiteral("daioncdn.net"))
        || lower.contains(QStringLiteral("medya.trt.com.tr"))
        || lower.contains(QStringLiteral("googlevideo.com"))) {
        score += 10;
    }
    if (lower.contains(QStringLiteral("chunklist")) || lower.contains(QStringLiteral("audio_only"))
        || lower.contains(QStringLiteral("_audio")) || lower.contains(QStringLiteral("subtitle"))
        || lower.contains(QStringLiteral("fragman")) || lower.contains(QStringLiteral("teaser"))
        || lower.contains(QStringLiteral("preview"))) {
        score -= 15;
    }
    if (lower.contains(QStringLiteral("-hls-drm")) || lower.contains(QStringLiteral("/drm/"))) {
        score -= 80;
    }
    return score;
}

bool detectDrmIndicators(const QString &textLower)
{
    static const QStringList kStrongDrmMarkers = {
        QStringLiteral("com.widevine.alpha"),
        QStringLiteral("com.microsoft.playready"),
        QStringLiteral("com.apple.fps"),
        QStringLiteral("widevine"),
        QStringLiteral("playready"),
        QStringLiteral("fairplay"),
        QStringLiteral("\"isdrm\":true"),
        QStringLiteral("\"isdrm\": true"),
        QStringLiteral("\"drm\":true"),
        QStringLiteral("\"drm\": true"),
        QStringLiteral("\"drm_info\""),
        QStringLiteral("licenseserver"),
        QStringLiteral("license_url"),
        QStringLiteral("licenseurl"),
        QStringLiteral("eme_drm"),
        QStringLiteral("\"status\":\"login_required\""),
        QStringLiteral("\"status\":\"unplayable\""),
        QStringLiteral("drm protected"),
        QStringLiteral("drm korumalı"),
    };
    for (const QString &marker : kStrongDrmMarkers) {
        if (textLower.contains(marker)) {
            return true;
        }
    }
    return false;
}

struct ScoredCandidate
{
    QString url;
    int score = 0;
};

} // namespace

StreamResolver::StreamResolver(QObject *parent)
    : QObject(parent)
{
}

StreamResolver::~StreamResolver()
{
    cancel();
}

bool StreamResolver::isDirectStreamUrl(const QString &url)
{
    if (!StreamSecurity::isAllowedMediaUrl(url)) {
        return false;
    }
    const QUrl parsed(url.trimmed(), QUrl::StrictMode);
    const QString pathLower = parsed.path().toLower();
    const QString queryLower = parsed.query().toLower();

    static const QStringList kDirectExtensions = {
        QStringLiteral(".m3u8"),
        QStringLiteral(".m3u"),
        QStringLiteral(".mpd"),
        QStringLiteral(".ts"),
        QStringLiteral(".mp4"),
        QStringLiteral(".webm"),
        QStringLiteral(".mkv"),
        QStringLiteral(".flv"),
        QStringLiteral(".aac"),
        QStringLiteral(".mp3"),
    };
    for (const QString &ext : kDirectExtensions) {
        if (pathLower.endsWith(ext)) {
            return true;
        }
    }
    if (queryLower.contains(QStringLiteral(".m3u8"))
        || queryLower.contains(QStringLiteral("format=m3u8"))
        || queryLower.contains(QStringLiteral("type=m3u8"))) {
        return true;
    }
    return false;
}

StreamExtractResult StreamResolver::extractStreamFromHtml(const QByteArray &rawBody,
                                                          const QUrl &baseUrl,
                                                          const QString &contentType)
{
    StreamExtractResult res;
    if (rawBody.isEmpty()) {
        res.errorMessage = QStringLiteral("Yayın sayfası boş yanıt döndürdü.");
        return res;
    }
    if (rawBody.size() > StreamSecurity::kMaxResolverPageBytes) {
        res.errorMessage = QStringLiteral("Yayın sayfası 2 MiB güvenlik sınırını aşıyor.");
        return res;
    }

    const QString ctLower = contentType.toLower();
    const QByteArray trimmedStart = rawBody.left(64).trimmed();
    if (ctLower.contains(QStringLiteral("mpegurl")) || trimmedStart.startsWith("#EXTM3U")) {
        const QString finalUrl = baseUrl.toString();
        if (StreamSecurity::isAllowedMediaUrl(finalUrl)) {
            res.ok = true;
            res.streamUrl = finalUrl;
            return res;
        }
    }

    const QString text = QString::fromUtf8(rawBody);
    QVector<ScoredCandidate> candidates;
    QSet<QString> seen;

    auto consider = [&](const QString &rawMatch, int basePriority) {
        const QString cleaned = unescapeCandidateUrl(rawMatch);
        const QString resolvedStr = resolveSafeCandidateUrl(cleaned, baseUrl);
        if (resolvedStr.isEmpty()) {
            return;
        }
        if (isAdOrTrackerUrl(resolvedStr)) {
            return;
        }
        if (seen.contains(resolvedStr)) {
            return;
        }
        seen.insert(resolvedStr);
        candidates.append({resolvedStr, scoreCandidateUrl(resolvedStr, basePriority)});
    };

    // Priority 0: Turkuvaz getvideo JSON response -> raw trkvz-live.ercdn.net/atvhd/ requires securevideotoken
    if (baseUrl.host().toLower().contains(QStringLiteral("videojs.tmgrup.com.tr"))) {
        static const QRegularExpression kTurkuvazVideoUrl(
            QStringLiteral(R"re(["']VideoUrl["']\s*:\s*["'](https?:\\?/\\?/[^"'<>\s]+\.m3u8(?:[^"'<>\s]*)?)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        const auto m = kTurkuvazVideoUrl.match(text);
        if (m.hasMatch()) {
            const QString rawVideoUrl = unescapeCandidateUrl(m.captured(1));
            if (StreamSecurity::isAllowedMediaUrl(rawVideoUrl)
                && !rawVideoUrl.contains(QStringLiteral("st="))) {
                const QString tokenApi = QStringLiteral("https://securevideotoken.tmgrup.com.tr/webtv/secure?url=")
                                         + QString::fromLatin1(QUrl::toPercentEncoding(rawVideoUrl));
                if (StreamSecurity::isAllowedMediaUrl(tokenApi)) {
                    res.secondaryPageUrl = tokenApi;
                    res.errorMessage = QStringLiteral("Resmi yayın belirteci alınıyor...");
                    return res;
                }
            }
        }
    }

    // Priority 1a: Demirören official media JSON API ("ServiceUrl"/"DefaultServiceUrl" + "SecurePath")
    {
        static const QRegularExpression kServiceUrlRegex(
            QStringLiteral(R"re(["'](?:ServiceUrl|DefaultServiceUrl)["']\s*:\s*["'](https?:\\?/\\?/[^"'<>\s]+)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression kSecurePathRegex(
            QStringLiteral(R"re(["']SecurePath["']\s*:\s*["']([^"'<>\s]+\.m3u8(?:[^"'<>\s]*)?)["'])re"),
            QRegularExpression::CaseInsensitiveOption);
        const auto sm = kServiceUrlRegex.match(text);
        const auto pm = kSecurePathRegex.match(text);
        if (sm.hasMatch() && pm.hasMatch()) {
            QString svc = unescapeCandidateUrl(sm.captured(1));
            QString path = unescapeCandidateUrl(pm.captured(1));
            while (svc.endsWith(QLatin1Char('/'))) {
                svc.chop(1);
            }
            if (!path.startsWith(QLatin1Char('/'))) {
                path.prepend(QLatin1Char('/'));
            }
            consider(svc + path, 115);
        }
    }

    // Priority 1b: Official YouTube / player hlsManifestUrl field (supports .m3u8 & hls_variant manifests)
    static const QRegularExpression kHlsManifestRegex(
        QStringLiteral(R"re((?:["']hlsManifestUrl["']|\bhlsManifestUrl)\s*:\s*["']([^"'<>\s]+)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kHlsManifestRegex.globalMatch(text);
        while (it.hasNext()) {
            const QString candidate = unescapeCandidateUrl(it.next().captured(1));
            const QString lowerCand = candidate.toLower();
            if (lowerCand.contains(QStringLiteral(".m3u8"))
                || lowerCand.contains(QStringLiteral("hls_variant"))
                || lowerCand.contains(QStringLiteral("hls_playlist"))) {
                consider(candidate, 110);
            }
        }
    }

    // Priority 1b: Structured JSON & unquoted JS player config fields
    static const QRegularExpression kJsonHlsRegex(
        QStringLiteral(R"re((?:["'](?:hlsManifestUrl|hlsUrl|hls_url|hls|streamUrl|stream_url|liveUrl|live_url|liveStreamUrl|live_stream_url|broadcastUrl|videoUrl|video_url|mediaUrl|playlistUrl|m3u8|m3u8Url|manifestUrl|contentUrl|defaultUrl|hdUrl|source|file|src|url)["']|\b(?:hlsUrl|hls_url|hls|streamUrl|stream_url|liveUrl|live_url|liveStreamUrl|broadcastUrl|videoUrl|mediaUrl|playlistUrl|m3u8Url|manifestUrl|contentUrl|defaultUrl|hdUrl|source|file|src|url))\s*:\s*["']([^"'<>\s]+\.m3u8(?:[^"'<>\s]*)?)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kJsonHlsRegex.globalMatch(text);
        while (it.hasNext()) {
            consider(it.next().captured(1), 100);
        }
    }

    // Priority 2a: HTML player container data-* attributes (used by Turkish broadcaster templates)
    static const QRegularExpression kDataAttrRegex(
        QStringLiteral(R"re(\bdata-(?:hls|stream|live-url|liveurl|video-url|videourl|url|source|src|m3u8)\s*=\s*["']([^"'<>\s]+\.m3u8(?:[^"'<>\s]*)?)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kDataAttrRegex.globalMatch(text);
        while (it.hasNext()) {
            consider(it.next().captured(1), 90);
        }
    }

    // Priority 2b: HTML <source> / <video> tags with .m3u8 src
    static const QRegularExpression kHtmlSourceRegex(
        QStringLiteral(R"re(<(?:source|video)\b[^>]*\bsrc\s*=\s*["']([^"'<>\s]+\.m3u8(?:[^"'<>\s]*)?)["'])re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kHtmlSourceRegex.globalMatch(text);
        while (it.hasNext()) {
            consider(it.next().captured(1), 85);
        }
    }

    // Priority 2c: Percent-encoded https?%3A%2F%2F...m3u8 URLs embedded in player query/config strings
    static const QRegularExpression kEncodedM3u8Regex(
        QStringLiteral(R"re((https?%3A%2F%2F[^"'<>\s&)]+\.m3u8(?:[^"'<>\s&)]*)?))re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kEncodedM3u8Regex.globalMatch(text);
        while (it.hasNext()) {
            const QString decoded = QUrl::fromPercentEncoding(it.next().captured(1).toUtf8());
            consider(decoded, 70);
        }
    }

    // Priority 3: General http/https or protocol-relative .m3u8 URLs in static markup/config
    static const QRegularExpression kGenericM3u8Regex(
        QStringLiteral(R"re(((?:https?:)?\\?/\\?/[A-Za-z0-9.-]+(?::\d+)?(?:\\?/)[^"'<>\s\\)]+\.m3u8(?:\?[^"'<>\s)]*)?))re"),
        QRegularExpression::CaseInsensitiveOption);
    {
        auto it = kGenericM3u8Regex.globalMatch(text);
        while (it.hasNext()) {
            consider(it.next().captured(1), 60);
        }
    }

    if (!candidates.isEmpty()) {
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const ScoredCandidate &a, const ScoredCandidate &b) {
                             return a.score > b.score;
                         });
        res.ok = true;
        res.streamUrl = candidates.first().url;
        return res;
    }

    const QString lower = text.toLower();
    const bool hasDrm = detectDrmIndicators(lower);

    // Secondary hop discovery:
    // 1. Turkuvaz Media container (ATV / A Haber / MinikaGO / MinikaÇOCUK) -> official videojs.tmgrup.com.tr/getvideo API
    QString secondary;
    if (lower.contains(QStringLiteral("tmdvpcontainer")) || lower.contains(QStringLiteral("tmgrup.com.tr"))) {
        static const QRegularExpression kTmdWebsiteRegex(
            QStringLiteral(R"re(\bdata-websiteid\s*=\s*["']([0-9a-fA-F-]{36})["'])re"));
        static const QRegularExpression kTmdVideoRegex(
            QStringLiteral(R"re(\bdata-videoid\s*=\s*["']([0-9a-fA-F-]{36})["'])re"));
        const auto ws = kTmdWebsiteRegex.match(text);
        const auto vid = kTmdVideoRegex.match(text);
        if (ws.hasMatch() && vid.hasMatch()) {
            const QString apiUrl = QStringLiteral("https://videojs.tmgrup.com.tr/getvideo/%1/%2")
                                       .arg(ws.captured(1), vid.captured(1));
            if (StreamSecurity::isAllowedMediaUrl(apiUrl) && apiUrl != baseUrl.toString()) {
                secondary = apiUrl;
            }
        }
    }
    // 2. Official YouTube Live embed or broadcaster player <iframe>
    if (secondary.isEmpty()) {
        secondary = extractYoutubeLiveTarget(text, baseUrl);
    }
    if (secondary.isEmpty()) {
        secondary = extractBroadcasterPlayerIframe(text, baseUrl);
    }
    if (!secondary.isEmpty()) {
        res.secondaryPageUrl = secondary;
        res.drmDetected = hasDrm;
        res.errorMessage = QStringLiteral("Gömülü resmi yayın oynatıcısı çözümleniyor...");
        return res;
    }

    if (hasDrm) {
        res.drmDetected = true;
        res.errorMessage = QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı");
        return res;
    }

    if (lower.contains(QStringLiteral("live_stream_offline"))
        || (isYoutubeHost(baseUrl.host())
            && (lower.contains(QStringLiteral("\"status\":\"error\""))
                || lower.contains(QStringLiteral("\"status\":\"unplayable\""))
                || baseUrl.path().toLower().endsWith(QStringLiteral("/live"))))) {
        res.errorMessage = QStringLiteral("Yayıncı canlı akışı şu anda kapalı / çevrimdışı.");
        return res;
    }

    res.errorMessage = QStringLiteral("Bu kanalın açık akışı alınamadı (sayfada geçerli HLS .m3u8 bulunamadı).");
    return res;
}

void StreamResolver::resolveChannel(const QString &streamUrl,
                                    const QString &sourcePageUrl,
                                    const QString &channelName,
                                    bool forcePageResolve,
                                    const QStringList &fallbackUrls)
{
    cancel();

    m_activeChannelName = channelName;
    m_lastFailureReason.clear();
    m_lastFailureWasDrm = false;
    m_channelOriginReferer = extractSafeOriginReferer(sourcePageUrl);

    const QString trimmedStream = streamUrl.trimmed();
    const QString trimmedPage = sourcePageUrl.trimmed();

    QSet<QString> seen;
    auto enqueueFallback = [&](const QString &raw) {
        const QString unwrapped = StreamSecurity::unwrapResolverUrl(raw);
        if (unwrapped.isEmpty() || seen.contains(unwrapped)) {
            return;
        }
        seen.insert(unwrapped);
        m_pendingFallbacks.append(unwrapped);
    };

    if (!forcePageResolve && !trimmedStream.isEmpty()) {
        bool wasResolve = false;
        QString reason;
        const QString unwrapped = StreamSecurity::unwrapResolverUrl(trimmedStream, &wasResolve, &reason);
        if (unwrapped.isEmpty()) {
            finishFailure(QStringLiteral("Güvenlik engeli: %1").arg(reason), channelName, false);
            return;
        }
        if (!wasResolve && isDirectStreamUrl(unwrapped)) {
            seen.insert(unwrapped);
            enqueueFallback(trimmedPage);
            for (const QString &fb : fallbackUrls) {
                enqueueFallback(fb);
            }
            emit fallbacksChanged();
            finishSuccess(unwrapped, channelName, false, m_channelOriginReferer, QString());
            return;
        }
        enqueueFallback(unwrapped);
    }

    enqueueFallback(trimmedPage);
    for (const QString &fb : fallbackUrls) {
        enqueueFallback(fb);
    }
    emit fallbacksChanged();

    if (m_pendingFallbacks.isEmpty()) {
        finishFailure(QStringLiteral("Bu kanal için geçerli bir yayın veya sayfa adresi tanımlı değil."),
                      channelName, false);
        return;
    }

    startNextFallback(forcePageResolve);
}

bool StreamResolver::tryNextFallback(const QString &channelName)
{
    if (m_pendingFallbacks.isEmpty()) {
        return false;
    }
    if (!channelName.isEmpty()) {
        m_activeChannelName = channelName;
    }
    startNextFallback(true);
    return true;
}

void StreamResolver::startNextFallback(bool bypassCache)
{
    if (m_pendingFallbacks.isEmpty()) {
        const QString reason = m_lastFailureReason.isEmpty()
            ? QStringLiteral("Bu kanalın açık akışı alınamadı (tüm resmi yedekler başarısız).")
            : m_lastFailureReason;
        finishFailure(reason, m_activeChannelName, m_lastFailureWasDrm);
        return;
    }

    const QString targetUrl = m_pendingFallbacks.takeFirst();
    emit fallbacksChanged();

    if (!bypassCache) {
        const auto it = m_cache.constFind(targetUrl);
        if (it != m_cache.constEnd()) {
            if (it->expiresAtUtc > QDateTime::currentDateTimeUtc()
                && StreamSecurity::isAllowedMediaUrl(it->streamUrl)) {
                finishSuccess(it->streamUrl, m_activeChannelName, true, it->httpReferer, it->httpUserAgent);
                return;
            }
            m_cache.erase(it);
        }
    }

    const quint64 reqId = ++m_requestSeq;
    m_visitedUrls.clear();
    setLastError(QString());
    setResolving(true);
    setStatusMessage(m_activeChannelName.isEmpty()
                         ? QStringLiteral("Resmi canlı akış çözümleniyor...")
                         : QStringLiteral("%1 için resmi canlı akış çözümleniyor...").arg(m_activeChannelName));

    const QString referer = m_channelOriginReferer.isEmpty()
        ? extractSafeOriginReferer(targetUrl)
        : m_channelOriginReferer;
    startPageFetch(targetUrl, targetUrl, m_activeChannelName, 0, 0, false, referer, reqId);
}

void StreamResolver::handleCandidateFailure(const QString &errorMessage,
                                            const QString &channelName,
                                            bool drmDetected,
                                            quint64 requestId)
{
    if (m_requestSeq != requestId) {
        return;
    }
    if (drmDetected || m_lastFailureReason.isEmpty()) {
        m_lastFailureReason = errorMessage;
        m_lastFailureWasDrm = m_lastFailureWasDrm || drmDetected;
    }
    if (!m_pendingFallbacks.isEmpty()) {
        setStatusMessage(channelName.isEmpty()
                             ? QStringLiteral("Yedek resmi yayın kaynağı deneniyor...")
                             : QStringLiteral("%1 için yedek resmi kaynak deneniyor...").arg(channelName));
        startNextFallback(true);
        return;
    }
    finishFailure(m_lastFailureReason.isEmpty() ? errorMessage : m_lastFailureReason,
                  channelName,
                  m_lastFailureWasDrm || drmDetected);
}

void StreamResolver::resolveUrl(const QString &inputUrl,
                                const QString &channelName,
                                bool bypassCache)
{
    cancel();

    m_activeChannelName = channelName;
    m_lastFailureReason.clear();
    m_lastFailureWasDrm = false;

    bool wasResolve = false;
    QString reason;
    const QString targetUrl = StreamSecurity::unwrapResolverUrl(inputUrl, &wasResolve, &reason);
    if (targetUrl.isEmpty()) {
        finishFailure(QStringLiteral("Güvenlik engeli: %1").arg(reason), channelName, false);
        return;
    }

    m_channelOriginReferer = extractSafeOriginReferer(targetUrl);

    // Stage 1: Direct HLS/media stream URL passes immediately unless explicit resolve:// was requested.
    if (!wasResolve && isDirectStreamUrl(targetUrl)) {
        finishSuccess(targetUrl, channelName, false, m_channelOriginReferer, QString());
        return;
    }

    // Check short-lived in-memory cache for previously resolved live pages.
    if (!bypassCache) {
        const auto it = m_cache.constFind(targetUrl);
        if (it != m_cache.constEnd()) {
            if (it->expiresAtUtc > QDateTime::currentDateTimeUtc()
                && StreamSecurity::isAllowedMediaUrl(it->streamUrl)) {
                finishSuccess(it->streamUrl, channelName, true, it->httpReferer, it->httpUserAgent);
                return;
            }
            m_cache.erase(it);
        }
    }

    // Stage 2: Fetch broadcaster live web page purely via Qt6::Network.
    const quint64 reqId = ++m_requestSeq;
    m_visitedUrls.clear();
    setLastError(QString());
    setResolving(true);
    setStatusMessage(channelName.isEmpty()
                         ? QStringLiteral("Resmi canlı akış çözümleniyor...")
                         : QStringLiteral("%1 için resmi canlı akış çözümleniyor...").arg(channelName));
    startPageFetch(targetUrl, targetUrl, channelName, 0, 0, false, m_channelOriginReferer, reqId);
}

void StreamResolver::startPageFetch(const QString &pageUrl,
                                    const QString &cacheKey,
                                    const QString &channelName,
                                    int redirectDepth,
                                    int hopDepth,
                                    bool sawDrmHint,
                                    const QString &refererUrl,
                                    quint64 requestId)
{
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }

    m_buffer.clear();
    m_overflow = false;
    m_visitedUrls.insert(pageUrl);

    QUrl requestUrl(pageUrl, QUrl::StrictMode);
    const bool youtubeTarget = isYoutubeHost(requestUrl.host());
    if (youtubeTarget && (requestUrl.host().compare(QStringLiteral("www.youtube.com"), Qt::CaseInsensitive) == 0
                          || requestUrl.host().compare(QStringLiteral("youtube.com"), Qt::CaseInsensitive) == 0)) {
        // Mobile Safari HTML endpoint exposes hlsManifestUrl directly in ytInitialPlayerResponse.
        requestUrl.setHost(QStringLiteral("m.youtube.com"));
        if (!StreamSecurity::isAllowedMediaUrl(requestUrl.toString())) {
            handleCandidateFailure(QStringLiteral("Güvenlik engeli: Geçersiz YouTube hedefi."),
                                   channelName, false, requestId);
            return;
        }
        m_visitedUrls.insert(requestUrl.toString());
    }

    QNetworkRequest req{requestUrl};
    req.setTransferTimeout(StreamSecurity::kNetworkTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QString::fromLatin1(youtubeTarget ? kMobileSafariUa : kDesktopBrowserUa));
    req.setRawHeader("Accept", "text/html,application/xhtml+xml,application/vnd.apple.mpegurl,application/json;q=0.9,*/*;q=0.5");
    req.setRawHeader("Accept-Language", "tr-TR,tr;q=0.9,en;q=0.5");

    if (youtubeTarget) {
        req.setRawHeader("Cookie", "SOCS=CAI");
    } else {
        const QString effectiveRef = StreamSecurity::isAllowedMediaUrl(refererUrl)
            ? refererUrl
            : m_channelOriginReferer;
        if (!effectiveRef.isEmpty() && StreamSecurity::isAllowedMediaUrl(effectiveRef)) {
            req.setRawHeader("Referer", effectiveRef.toUtf8());
            const QUrl refU(effectiveRef, QUrl::StrictMode);
            if (refU.isValid() && !refU.host().isEmpty()) {
                QString originHdr = refU.scheme() + QStringLiteral("://") + refU.host();
                if (refU.port() > 0) {
                    originHdr += QLatin1Char(':') + QString::number(refU.port());
                }
                req.setRawHeader("Origin", originHdr.toUtf8());
            }
        }
    }

    QNetworkReply *reply = m_network.get(req);
    m_activeReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, requestId]() {
        if (m_activeReply != reply || m_requestSeq != requestId) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (m_buffer.size() + chunk.size() > StreamSecurity::kMaxResolverPageBytes) {
            m_overflow = true;
            reply->abort();
            return;
        }
        m_buffer.append(chunk);
    });

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cacheKey, channelName, redirectDepth, hopDepth, sawDrmHint, refererUrl, youtubeTarget, requestId]() {
                reply->deleteLater();
                if (m_activeReply != reply || m_requestSeq != requestId) {
                    return;
                }
                m_activeReply = nullptr;

                if (m_overflow) {
                    handleCandidateFailure(QStringLiteral("Yayın sayfası 2 MiB güvenlik sınırını aştı."),
                                           channelName, false, requestId);
                    return;
                }

                const QVariant redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
                if (redirectTarget.isValid()) {
                    if (redirectDepth >= StreamSecurity::kMaxRedirects) {
                        handleCandidateFailure(QStringLiteral("Çok fazla yönlendirme nedeniyle canlı akış çözümlenemedi."),
                                               channelName, false, requestId);
                        return;
                    }
                    const QUrl resolved = reply->url().resolved(redirectTarget.toUrl());
                    QString reason;
                    if (!StreamSecurity::isAllowedMediaUrl(resolved.toString(), &reason)) {
                        handleCandidateFailure(QStringLiteral("Güvensiz yönlendirme engellendi: %1").arg(reason),
                                               channelName, false, requestId);
                        return;
                    }
                    startPageFetch(resolved.toString(), cacheKey, channelName,
                                   redirectDepth + 1, hopDepth, sawDrmHint, refererUrl, requestId);
                    return;
                }

                const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (reply->error() != QNetworkReply::NoError) {
                    if (httpStatus == 401 || httpStatus == 403 || httpStatus == 451) {
                        handleCandidateFailure(
                            QStringLiteral("Yayıncı akışı erişime kapalı / DRM korumalı (HTTP %1).").arg(httpStatus),
                            channelName, true, requestId);
                        return;
                    }
                    if (httpStatus == 404) {
                        handleCandidateFailure(
                            QStringLiteral("Yayın kaynağı bulunamadı (HTTP 404)."),
                            channelName, false, requestId);
                        return;
                    }
                    handleCandidateFailure(
                        QStringLiteral("Resmi yayın sayfasına erişilemedi: %1").arg(reply->errorString()),
                        channelName, false, requestId);
                    return;
                }

                const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
                const StreamExtractResult extracted = extractStreamFromHtml(m_buffer, reply->url(), contentType);
                const bool combinedDrm = sawDrmHint || extracted.drmDetected;

                if (!extracted.ok) {
                    if (!extracted.secondaryPageUrl.isEmpty()) {
                        QString secReason;
                        if (hopDepth < kMaxSecondaryHops
                            && StreamSecurity::isAllowedMediaUrl(extracted.secondaryPageUrl, &secReason)
                            && !m_visitedUrls.contains(extracted.secondaryPageUrl)) {
                            setStatusMessage(channelName.isEmpty()
                                                 ? QStringLiteral("Gömülü resmi yayın oynatıcısı çözümleniyor...")
                                                 : QStringLiteral("%1 için gömülü resmi oynatıcı çözümleniyor...").arg(channelName));
                            const QString nextRef = m_channelOriginReferer.isEmpty()
                                ? extractSafeOriginReferer(reply->url().toString())
                                : m_channelOriginReferer;
                            startPageFetch(extracted.secondaryPageUrl, cacheKey, channelName,
                                           0, hopDepth + 1, combinedDrm, nextRef, requestId);
                            return;
                        }
                        if (combinedDrm) {
                            handleCandidateFailure(QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı"),
                                                   channelName, true, requestId);
                            return;
                        }
                        handleCandidateFailure(QStringLiteral("Gömülü yayın sayfası çözümlenemedi (yönlendirme/çerçeve sınırı aşıldı)."),
                                               channelName, false, requestId);
                        return;
                    }
                    if (combinedDrm) {
                        handleCandidateFailure(QStringLiteral("Bu kanalın açık akışı alınamadı / DRM korumalı"),
                                               channelName, true, requestId);
                        return;
                    }
                    handleCandidateFailure(extracted.errorMessage, channelName, false, requestId);
                    return;
                }

                // Final mandatory gate through StreamSecurity before emitting.
                QString secReason;
                if (!StreamSecurity::isAllowedMediaUrl(extracted.streamUrl, &secReason)) {
                    handleCandidateFailure(QStringLiteral("Güvenlik engeli: %1").arg(secReason),
                                           channelName, false, requestId);
                    return;
                }

                const QString outReferer = youtubeTarget
                    ? QString()
                    : (m_channelOriginReferer.isEmpty() ? extractSafeOriginReferer(reply->url().toString()) : m_channelOriginReferer);
                const QString outUa = youtubeTarget ? QString::fromLatin1(kMobileSafariUa) : QString();

                m_cache.insert(cacheKey,
                               {extracted.streamUrl,
                                outReferer,
                                outUa,
                                QDateTime::currentDateTimeUtc().addSecs(kCacheTtlSecs)});
                finishSuccess(extracted.streamUrl, channelName, true, outReferer, outUa);
            });
}

void StreamResolver::cancel()
{
    ++m_requestSeq;
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }
    m_buffer.clear();
    m_overflow = false;
    m_visitedUrls.clear();
    if (!m_pendingFallbacks.isEmpty()) {
        m_pendingFallbacks.clear();
        emit fallbacksChanged();
    }
    setResolving(false);
}

void StreamResolver::clearCache()
{
    m_cache.clear();
}

void StreamResolver::finishSuccess(const QString &resolvedUrl,
                                   const QString &channelName,
                                   bool fromWebPage,
                                   const QString &httpReferer,
                                   const QString &httpUserAgent)
{
    setResolving(false);
    setLastError(QString());
    if (m_lastResolvedUrl != resolvedUrl) {
        m_lastResolvedUrl = resolvedUrl;
        emit lastResolvedUrlChanged();
    }
    setStatusMessage(fromWebPage
                         ? QStringLiteral("Resmi canlı HLS akışı çözümlendi.")
                         : QStringLiteral("Doğrudan yayın akışı hazır."));
    emit streamResolved(resolvedUrl, channelName, fromWebPage);
    emit streamResolvedWithHeaders(resolvedUrl, channelName, fromWebPage, httpReferer, httpUserAgent);
}

void StreamResolver::finishFailure(const QString &errorMessage, const QString &channelName, bool drmDetected)
{
    setResolving(false);
    setLastError(errorMessage);
    setStatusMessage(errorMessage);
    emit resolveFailed(errorMessage, channelName, drmDetected);
}

void StreamResolver::setResolving(bool resolving)
{
    if (m_resolving != resolving) {
        m_resolving = resolving;
        emit resolvingChanged();
    }
}

void StreamResolver::setStatusMessage(const QString &msg)
{
    if (m_statusMessage != msg) {
        m_statusMessage = msg;
        emit statusMessageChanged();
    }
}

void StreamResolver::setLastError(const QString &err)
{
    if (m_lastError != err) {
        m_lastError = err;
        emit lastErrorChanged();
    }
}

} // namespace zethropol::tv
