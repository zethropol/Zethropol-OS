#include "StreamResolver.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace zethropol::tvwin7 {

namespace {

bool containsNoCase(const std::string &haystackLower, const char *needleLower)
{
    return haystackLower.find(needleLower) != std::string::npos;
}

bool endsWithNoCase(const std::string &sLower, const char *suffixLower)
{
    const std::size_t n = std::char_traits<char>::length(suffixLower);
    return sLower.size() >= n && sLower.compare(sLower.size() - n, n, suffixLower) == 0;
}

bool isYoutubeHost(const std::string &host)
{
    const std::string h = StreamSecurity::toLowerAscii(host);
    return h == "youtube.com" || endsWithNoCase(h, ".youtube.com")
           || h == "youtube-nocookie.com" || endsWithNoCase(h, ".youtube-nocookie.com");
}

bool isAdOrTrackerUrl(const std::string &url)
{
    const std::string lower = StreamSecurity::toLowerAscii(url);
    static const char *const kAdTokens[] = {
        "doubleclick.net",
        "googlesyndication",
        "googleadservices",
        "imasdk.googleapis.com",
        "adservice.",
        "adsystem.",
        "adserver.",
        "adbreak",
        "taboola.",
        "outbrain.",
        "adnxs.com",
        "pubmatic.com",
        "rubiconproject.com",
        "criteo.",
        "mgid.com",
        "medyanet",
        "virgul.com",
        "reklam",
        "/ads/",
        "/ad/",
        "vast",
        "vmap",
        "preroll",
        "midroll",
        "postroll",
        "bumper",
        "tracker",
        "pixel.",
        "telemetry",
    };
    for (const char *tok : kAdTokens) {
        if (lower.find(tok) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool isSocialOrWidgetUrl(const std::string &url)
{
    const std::string lower = StreamSecurity::toLowerAscii(url);
    static const char *const kWidgetTokens[] = {
        "facebook.com",
        "fbcdn.net",
        "twitter.com",
        "platform.twitter",
        "instagram.com",
        "googletagmanager.com",
        "google.com/recaptcha",
        "google.com/maps",
        "recaptcha.net",
        "hcaptcha.com",
        "whatsapp.com",
        "t.me/",
        "disqus.com",
        "cookielaw.org",
        "onetrust.com",
    };
    for (const char *tok : kWidgetTokens) {
        if (lower.find(tok) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void replaceAllNoCase(std::string &s, const char *fromLower, const char *to)
{
    const std::size_t fromLen = std::char_traits<char>::length(fromLower);
    const std::size_t toLen = std::char_traits<char>::length(to);
    if (fromLen == 0) {
        return;
    }
    std::string lower = StreamSecurity::toLowerAscii(s);
    std::size_t pos = 0;
    while ((pos = lower.find(fromLower, pos)) != std::string::npos) {
        s.replace(pos, fromLen, to);
        lower.replace(pos, fromLen, to);
        pos += toLen;
    }
}

std::string unescapeCandidateUrl(std::string raw)
{
    raw = StreamSecurity::trimAscii(raw);
    replaceAllNoCase(raw, "\\/", "/");
    replaceAllNoCase(raw, "\\u002f", "/");
    replaceAllNoCase(raw, "\\u0026", "&");
    replaceAllNoCase(raw, "\\u003d", "=");
    replaceAllNoCase(raw, "\\u003f", "?");
    replaceAllNoCase(raw, "\\u003a", ":");
    replaceAllNoCase(raw, "&#x2f;", "/");
    replaceAllNoCase(raw, "&#47;", "/");
    replaceAllNoCase(raw, "&#x3a;", ":");
    replaceAllNoCase(raw, "&#x3f;", "?");
    replaceAllNoCase(raw, "&#x3d;", "=");
    replaceAllNoCase(raw, "&#x26;", "&");
    replaceAllNoCase(raw, "&amp;", "&");

    while (!raw.empty()) {
        const char last = raw.back();
        if (last == '\\' || last == ';' || last == ',' || last == ')'
            || last == ']' || last == '}' || last == '\'' || last == '"') {
            raw.pop_back();
        } else {
            break;
        }
    }
    return StreamSecurity::trimAscii(raw);
}

bool isUrlTerminator(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    return uc <= 0x20 || c == '"' || c == '\'' || c == '<' || c == '>' || c == ')' || c == ']' || c == '}';
}

bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

bool isUuid36(const std::string &s)
{
    if (s.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool isSameSiteOrSubdomain(const std::string &hostA, const std::string &hostB)
{
    const std::string a = StreamSecurity::toLowerAscii(hostA);
    const std::string b = StreamSecurity::toLowerAscii(hostB);
    if (a.empty() || b.empty()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    auto splitDots = [](const std::string &h) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start < h.size()) {
            const std::size_t dot = h.find('.', start);
            if (dot == std::string::npos) {
                parts.push_back(h.substr(start));
                break;
            }
            if (dot > start) {
                parts.push_back(h.substr(start, dot - start));
            }
            start = dot + 1;
        }
        return parts;
    };
    const auto pa = splitDots(a);
    const auto pb = splitDots(b);
    if (pa.size() < 2 || pb.size() < 2) {
        return false;
    }
    auto registrable = [](const std::vector<std::string> &p) {
        const std::size_t n = p.size();
        if (n >= 3 && p[n - 1].size() == 2
            && (p[n - 2] == "com" || p[n - 2] == "org" || p[n - 2] == "net" || p[n - 2] == "gov")) {
            return p[n - 3] + "." + p[n - 2] + "." + p[n - 1];
        }
        return p[n - 2] + "." + p[n - 1];
    };
    return registrable(pa) == registrable(pb);
}

int scoreCandidateUrl(const std::string &url, int sourcePriority)
{
    int score = sourcePriority;
    const std::string lower = StreamSecurity::toLowerAscii(url);
    if (lower.compare(0, 8, "https://") == 0) {
        score += 20;
    }
    if (lower.find("master.m3u8") != std::string::npos) {
        score += 40;
    } else if (lower.find("playlist.m3u8") != std::string::npos
               || lower.find("index.m3u8") != std::string::npos
               || lower.find("hls_variant") != std::string::npos) {
        score += 25;
    } else if (lower.find(".m3u8") != std::string::npos) {
        score += 15;
    }
    if (lower.find("live") != std::string::npos || lower.find("hls") != std::string::npos
        || lower.find("canli") != std::string::npos || lower.find("daioncdn.net") != std::string::npos
        || lower.find("medya.trt.com.tr") != std::string::npos
        || lower.find("googlevideo.com") != std::string::npos) {
        score += 10;
    }
    if (lower.find("chunklist") != std::string::npos || lower.find("audio_only") != std::string::npos
        || lower.find("_audio") != std::string::npos || lower.find("subtitle") != std::string::npos
        || lower.find("fragman") != std::string::npos || lower.find("teaser") != std::string::npos
        || lower.find("preview") != std::string::npos) {
        score -= 15;
    }
    if (lower.find("-hls-drm") != std::string::npos || lower.find("/drm/") != std::string::npos) {
        score -= 80;
    }
    return score;
}

bool detectDrmIndicators(const std::string &textLower)
{
    static const char *const kMarkers[] = {
        "com.widevine.alpha",
        "com.microsoft.playready",
        "com.apple.fps",
        "widevine",
        "playready",
        "fairplay",
        "\"isdrm\":true",
        "\"isdrm\": true",
        "\"drm\":true",
        "\"drm\": true",
        "\"drm_info\"",
        "licenseserver",
        "license_url",
        "licenseurl",
        "eme_drm",
        "\"status\":\"login_required\"",
        "\"status\":\"unplayable\"",
        "drm protected",
        "drm korumalı",
    };
    for (const char *m : kMarkers) {
        if (textLower.find(m) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Linear-time scanner: finds all values for a JSON/JS key (quoted or unquoted) like `"key": "value"` or `key: 'value'`
std::vector<std::string> findKeyStringValues(const std::string &text,
                                             const std::string &lower,
                                             const std::string &keyLower)
{
    std::vector<std::string> results;
    std::size_t pos = 0;
    while ((pos = lower.find(keyLower, pos)) != std::string::npos) {
        const std::size_t keyEnd = pos + keyLower.size();
        // Check word boundary before key
        bool quotedKey = false;
        if (pos > 0) {
            const char prev = lower[pos - 1];
            if (prev == '"' || prev == '\'') {
                quotedKey = true;
            } else if (isIdentChar(prev)) {
                pos = keyEnd;
                continue;
            }
        }
        std::size_t cur = keyEnd;
        if (quotedKey) {
            if (cur >= lower.size() || (lower[cur] != '"' && lower[cur] != '\'')) {
                pos = keyEnd;
                continue;
            }
            ++cur;
        } else {
            if (cur < lower.size() && isIdentChar(lower[cur])) {
                pos = keyEnd;
                continue;
            }
        }
        while (cur < text.size() && std::isspace(static_cast<unsigned char>(text[cur]))) {
            ++cur;
        }
        if (cur >= text.size() || text[cur] != ':') {
            pos = keyEnd;
            continue;
        }
        ++cur;
        while (cur < text.size() && std::isspace(static_cast<unsigned char>(text[cur]))) {
            ++cur;
        }
        if (cur >= text.size() || (text[cur] != '"' && text[cur] != '\'')) {
            pos = keyEnd;
            continue;
        }
        const char quote = text[cur++];
        const std::size_t valStart = cur;
        while (cur < text.size()) {
            if (text[cur] == '\\' && cur + 1 < text.size()) {
                cur += 2;
                continue;
            }
            if (text[cur] == quote) {
                break;
            }
            ++cur;
        }
        if (cur < text.size() && cur > valStart && (cur - valStart) <= StreamSecurity::kMaxUrlLength) {
            results.push_back(text.substr(valStart, cur - valStart));
        }
        pos = (cur > keyEnd) ? cur : keyEnd;
    }
    return results;
}

// Linear-time scanner: finds HTML attribute values like `attrName="value"` or `attrName='value'`
std::vector<std::string> findHtmlAttrValues(const std::string &text,
                                            const std::string &lower,
                                            const std::string &attrLower)
{
    std::vector<std::string> results;
    std::size_t pos = 0;
    while ((pos = lower.find(attrLower, pos)) != std::string::npos) {
        if (pos > 0 && isIdentChar(lower[pos - 1])) {
            pos += attrLower.size();
            continue;
        }
        std::size_t cur = pos + attrLower.size();
        while (cur < text.size() && std::isspace(static_cast<unsigned char>(text[cur]))) {
            ++cur;
        }
        if (cur >= text.size() || text[cur] != '=') {
            pos += attrLower.size();
            continue;
        }
        ++cur;
        while (cur < text.size() && std::isspace(static_cast<unsigned char>(text[cur]))) {
            ++cur;
        }
        if (cur >= text.size() || (text[cur] != '"' && text[cur] != '\'')) {
            pos += attrLower.size();
            continue;
        }
        const char quote = text[cur++];
        const std::size_t valStart = cur;
        while (cur < text.size() && text[cur] != quote) {
            ++cur;
        }
        if (cur < text.size() && cur > valStart && (cur - valStart) <= StreamSecurity::kMaxUrlLength) {
            results.push_back(text.substr(valStart, cur - valStart));
        }
        pos = (cur > pos + attrLower.size()) ? cur : (pos + attrLower.size());
    }
    return results;
}

// Linear-time scanner: finds all substrings containing `.m3u8` preceded by http(s):// or //
std::vector<std::string> findGenericM3u8Urls(const std::string &text, const std::string &lower)
{
    std::vector<std::string> results;
    std::size_t pos = 0;
    while ((pos = lower.find(".m3u8", pos)) != std::string::npos) {
        // Walk backwards to find URL start delimiter
        std::size_t start = pos;
        while (start > 0 && (pos - start) < StreamSecurity::kMaxUrlLength) {
            const char c = text[start - 1];
            if (isUrlTerminator(c) || c == '(' || c == '[' || c == '{' || c == '`') {
                break;
            }
            --start;
        }
        // Walk forwards to find URL end delimiter
        std::size_t end = pos + 5;
        while (end < text.size() && (end - start) < StreamSecurity::kMaxUrlLength) {
            if (isUrlTerminator(text[end])) {
                break;
            }
            ++end;
        }
        if (end > start) {
            std::string cand = text.substr(start, end - start);
            const std::string candLower = StreamSecurity::toLowerAscii(cand);
            // Strip leading junk before http://, https://, or //
            std::size_t schemePos = candLower.find("https://");
            if (schemePos == std::string::npos) {
                schemePos = candLower.find("http://");
            }
            if (schemePos == std::string::npos) {
                schemePos = candLower.find("https:\\/\\/");
            }
            if (schemePos == std::string::npos) {
                schemePos = candLower.find("http:\\/\\/");
            }
            if (schemePos == std::string::npos) {
                schemePos = candLower.find("//");
            }
            if (schemePos != std::string::npos) {
                results.push_back(cand.substr(schemePos));
            }
        }
        pos = end;
    }
    return results;
}

std::vector<std::string> findPercentEncodedM3u8Urls(const std::string &text, const std::string &lower)
{
    std::vector<std::string> results;
    for (const char *prefix : {"https%3a%2f%2f", "http%3a%2f%2f"}) {
        std::size_t pos = 0;
        while ((pos = lower.find(prefix, pos)) != std::string::npos) {
            std::size_t end = pos;
            while (end < text.size() && (end - pos) < StreamSecurity::kMaxUrlLength) {
                const char c = text[end];
                if (isUrlTerminator(c) || c == '&') {
                    break;
                }
                ++end;
            }
            const std::string encoded = text.substr(pos, end - pos);
            const std::string decoded = StreamSecurity::percentDecode(encoded);
            if (StreamSecurity::toLowerAscii(decoded).find(".m3u8") != std::string::npos) {
                results.push_back(decoded);
            }
            pos = end;
        }
    }
    return results;
}

std::string extractYoutubeLiveTarget(const std::string &text,
                                     const std::string &lower,
                                     const ParsedUrl &baseUrl,
                                     const std::string &baseUrlStr)
{
    const bool alreadyOnYt = baseUrl.valid && isYoutubeHost(baseUrl.host);

    // 1. youtube(-nocookie).com/embed/live_stream?channel=UC...
    {
        std::size_t pos = 0;
        while ((pos = lower.find("embed", pos)) != std::string::npos) {
            const std::size_t lsPos = lower.find("live_stream?", pos);
            if (lsPos != std::string::npos && lsPos - pos <= 8) {
                const std::size_t chPos = lower.find("channel=uc", lsPos);
                if (chPos != std::string::npos && chPos - lsPos < 256) {
                    const std::size_t idStart = chPos + 8;
                    std::size_t idEnd = idStart;
                    while (idEnd < text.size() && isIdentChar(text[idEnd])) {
                        ++idEnd;
                    }
                    if (idEnd - idStart >= 12) {
                        const std::string target = "https://www.youtube.com/channel/" + text.substr(idStart, idEnd - idStart) + "/live";
                        if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrlStr) {
                            return target;
                        }
                    }
                }
            }
            pos += 5;
        }
    }

    // 2. youtube(-nocookie).com/embed/<11-char-id>
    {
        for (const char *marker : {"youtube.com/embed/", "youtube.com\\/embed\\/", "youtube-nocookie.com/embed/", "youtube-nocookie.com\\/embed\\/"}) {
            const std::size_t mLen = std::char_traits<char>::length(marker);
            std::size_t pos = 0;
            while ((pos = lower.find(marker, pos)) != std::string::npos) {
                const std::size_t idStart = pos + mLen;
                std::size_t idEnd = idStart;
                while (idEnd < text.size() && isIdentChar(text[idEnd])) {
                    ++idEnd;
                }
                if (idEnd - idStart == 11) {
                    const std::string vid = text.substr(idStart, 11);
                    if (StreamSecurity::toLowerAscii(vid) != "videoseries") {
                        const std::string target = "https://www.youtube.com/watch?v=" + vid;
                        if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrlStr) {
                            return target;
                        }
                    }
                }
                pos = idStart;
            }
        }
    }

    // 3. Direct youtube.com/@handle/live or youtube.com/channel/UC.../live or youtube.com/watch?v=<11>
    if (!alreadyOnYt) {
        for (const char *marker : {"youtube.com/@", "youtube.com\\/@", "youtube.com/channel/uc", "youtube.com\\/channel\\/uc"}) {
            const std::size_t mLen = std::char_traits<char>::length(marker);
            std::size_t pos = 0;
            while ((pos = lower.find(marker, pos)) != std::string::npos) {
                std::size_t end = pos + mLen;
                while (end < text.size() && !isUrlTerminator(text[end]) && (end - pos) < 160) {
                    ++end;
                }
                const std::string chunk = unescapeCandidateUrl(text.substr(pos, end - pos));
                const std::string chunkLower = StreamSecurity::toLowerAscii(chunk);
                const std::size_t livePos = chunkLower.find("/live");
                if (livePos != std::string::npos) {
                    const std::string pathPart = chunk.substr(11, livePos - 11); // after "youtube.com"
                    const std::string target = "https://www.youtube.com" + pathPart + "/live";
                    if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrlStr) {
                        return target;
                    }
                }
                pos = end;
            }
        }
        for (const char *marker : {"youtube.com/watch?", "youtube.com\\/watch?"}) {
            const std::size_t mLen = std::char_traits<char>::length(marker);
            std::size_t pos = 0;
            while ((pos = lower.find(marker, pos)) != std::string::npos) {
                const std::size_t vPos = lower.find("v=", pos + mLen);
                if (vPos != std::string::npos && vPos - (pos + mLen) < 120) {
                    const std::size_t idStart = vPos + 2;
                    std::size_t idEnd = idStart;
                    while (idEnd < text.size() && isIdentChar(text[idEnd])) {
                        ++idEnd;
                    }
                    if (idEnd - idStart == 11) {
                        const std::string target = "https://www.youtube.com/watch?v=" + text.substr(idStart, 11);
                        if (StreamSecurity::isAllowedMediaUrl(target) && target != baseUrlStr) {
                            return target;
                        }
                    }
                }
                pos += mLen;
            }
        }
    }

    return {};
}

std::string extractBroadcasterPlayerIframe(const std::string &text,
                                           const std::string &lower,
                                           const ParsedUrl &baseUrl,
                                           const std::string &baseUrlStr)
{
    std::string bestUrl;
    int bestScore = -1;
    std::size_t pos = 0;
    while ((pos = lower.find("<iframe", pos)) != std::string::npos) {
        const std::size_t tagEnd = lower.find('>', pos);
        if (tagEnd == std::string::npos) {
            break;
        }
        const std::string tag = text.substr(pos, tagEnd - pos + 1);
        const std::string tagLower = lower.substr(pos, tagEnd - pos + 1);
        for (const char *attr : {"src", "data-src"}) {
            const auto vals = findHtmlAttrValues(tag, tagLower, attr);
            for (const std::string &raw : vals) {
                const std::string cleaned = unescapeCandidateUrl(raw);
                const std::string resolved = StreamSecurity::resolveRelativeUrl(cleaned, baseUrl);
                if (resolved.empty() || resolved == baseUrlStr) {
                    continue;
                }
                if (isAdOrTrackerUrl(resolved) || isSocialOrWidgetUrl(resolved)) {
                    continue;
                }
                const ParsedUrl parsed = StreamSecurity::parseHttpUrl(resolved);
                const std::string rLower = StreamSecurity::toLowerAscii(resolved);
                const bool sameSite = baseUrl.valid && isSameSiteOrSubdomain(parsed.host, baseUrl.host);
                const bool looksLikePlayer = rLower.find("player") != std::string::npos
                                             || rLower.find("embed") != std::string::npos
                                             || rLower.find("canli") != std::string::npos
                                             || rLower.find("live") != std::string::npos
                                             || rLower.find("yayin") != std::string::npos
                                             || rLower.find("stream") != std::string::npos;
                if (!sameSite && !looksLikePlayer) {
                    continue;
                }
                int score = 0;
                if (sameSite) score += 40;
                if (looksLikePlayer) score += 30;
                if (rLower.compare(0, 8, "https://") == 0) score += 10;
                if (score > bestScore) {
                    bestScore = score;
                    bestUrl = resolved;
                }
            }
        }
        pos = tagEnd + 1;
    }
    return bestUrl;
}

struct ScoredCandidate
{
    std::string url;
    int score = 0;
};

} // namespace

bool StreamResolver::isDirectStreamUrl(const std::string &url)
{
    if (!StreamSecurity::isAllowedMediaUrl(url)) {
        return false;
    }
    const ParsedUrl parsed = StreamSecurity::parseHttpUrl(url);
    if (!parsed.valid) {
        return false;
    }
    const std::string pathLower = StreamSecurity::toLowerAscii(parsed.path);
    const std::string queryLower = StreamSecurity::toLowerAscii(parsed.query);
    static const char *const kExts[] = {
        ".m3u8", ".m3u", ".mpd", ".ts", ".mp4", ".webm", ".mkv", ".flv", ".aac", ".mp3",
    };
    for (const char *ext : kExts) {
        if (endsWithNoCase(pathLower, ext)) {
            return true;
        }
    }
    if (queryLower.find(".m3u8") != std::string::npos
        || queryLower.find("format=m3u8") != std::string::npos
        || queryLower.find("type=m3u8") != std::string::npos) {
        return true;
    }
    return false;
}

StreamExtractResult StreamResolver::extractStreamFromHtml(const std::string &rawBody,
                                                          const std::string &baseUrlStr,
                                                          const std::string &contentType)
{
    StreamExtractResult res;
    if (rawBody.empty()) {
        res.errorMessage = "Yayın sayfası boş yanıt döndürdü.";
        return res;
    }
    if (rawBody.size() > StreamSecurity::kMaxResolverPageBytes) {
        res.errorMessage = "Yayın sayfası 2 MiB güvenlik sınırını aşıyor.";
        return res;
    }

    const std::string ctLower = StreamSecurity::toLowerAscii(contentType);
    const std::string head = StreamSecurity::trimAscii(rawBody.substr(0, std::min<std::size_t>(64, rawBody.size())));
    if (ctLower.find("mpegurl") != std::string::npos || head.compare(0, 7, "#EXTM3U") == 0) {
        if (StreamSecurity::isAllowedMediaUrl(baseUrlStr)) {
            res.ok = true;
            res.streamUrl = baseUrlStr;
            return res;
        }
    }

    const ParsedUrl baseUrl = StreamSecurity::parseHttpUrl(baseUrlStr);
    const std::string lower = StreamSecurity::toLowerAscii(rawBody);

    std::vector<ScoredCandidate> candidates;
    std::set<std::string> seen;

    auto consider = [&](const std::string &rawMatch, int basePriority) {
        const std::string cleaned = unescapeCandidateUrl(rawMatch);
        const std::string resolvedStr = StreamSecurity::resolveRelativeUrl(cleaned, baseUrl);
        if (resolvedStr.empty() || isAdOrTrackerUrl(resolvedStr) || seen.count(resolvedStr)) {
            return;
        }
        seen.insert(resolvedStr);
        candidates.push_back({resolvedStr, scoreCandidateUrl(resolvedStr, basePriority)});
    };

    // Priority 0: Turkuvaz getvideo JSON response -> raw trkvz-live.ercdn.net requires securevideotoken
    if (baseUrl.valid && containsNoCase(StreamSecurity::toLowerAscii(baseUrl.host), "videojs.tmgrup.com.tr")) {
        const auto videoUrls = findKeyStringValues(rawBody, lower, "videourl");
        for (const std::string &rawVal : videoUrls) {
            const std::string rawVideoUrl = unescapeCandidateUrl(rawVal);
            if (StreamSecurity::isAllowedMediaUrl(rawVideoUrl)
                && StreamSecurity::toLowerAscii(rawVideoUrl).find(".m3u8") != std::string::npos
                && rawVideoUrl.find("st=") == std::string::npos) {
                const std::string tokenApi = "https://securevideotoken.tmgrup.com.tr/webtv/secure?url="
                                             + StreamSecurity::percentEncode(rawVideoUrl);
                if (StreamSecurity::isAllowedMediaUrl(tokenApi)) {
                    res.secondaryPageUrl = tokenApi;
                    res.errorMessage = "Resmi yayın belirteci alınıyor...";
                    return res;
                }
            }
        }
    }

    // Priority 1a: Demirören official media JSON API ("ServiceUrl"/"DefaultServiceUrl" + "SecurePath")
    {
        auto serviceUrls = findKeyStringValues(rawBody, lower, "serviceurl");
        if (serviceUrls.empty()) {
            serviceUrls = findKeyStringValues(rawBody, lower, "defaultserviceurl");
        }
        const auto securePaths = findKeyStringValues(rawBody, lower, "securepath");
        if (!serviceUrls.empty() && !securePaths.empty()) {
            std::string svc = unescapeCandidateUrl(serviceUrls.front());
            std::string path = unescapeCandidateUrl(securePaths.front());
            if (StreamSecurity::toLowerAscii(path).find(".m3u8") != std::string::npos) {
                while (!svc.empty() && svc.back() == '/') {
                    svc.pop_back();
                }
                if (!path.empty() && path.front() != '/') {
                    path.insert(path.begin(), '/');
                }
                consider(svc + path, 115);
            }
        }
    }

    // Priority 1b: YouTube / player hlsManifestUrl field
    {
        const auto manifests = findKeyStringValues(rawBody, lower, "hlsmanifesturl");
        for (const std::string &val : manifests) {
            const std::string cand = unescapeCandidateUrl(val);
            const std::string cLower = StreamSecurity::toLowerAscii(cand);
            if (cLower.find(".m3u8") != std::string::npos
                || cLower.find("hls_variant") != std::string::npos
                || cLower.find("hls_playlist") != std::string::npos) {
                consider(cand, 110);
            }
        }
    }

    // Priority 1c: Structured JSON & unquoted JS player config fields
    static const char *const kConfigKeys[] = {
        "hlsurl", "hls_url", "hls", "streamurl", "stream_url", "liveurl", "live_url",
        "livestreamurl", "live_stream_url", "broadcasturl", "videourl", "video_url",
        "mediaurl", "playlisturl", "m3u8", "m3u8url", "manifesturl", "contenturl",
        "defaulturl", "hdurl", "source", "file", "src", "url",
    };
    for (const char *key : kConfigKeys) {
        const auto vals = findKeyStringValues(rawBody, lower, key);
        for (const std::string &v : vals) {
            if (StreamSecurity::toLowerAscii(v).find(".m3u8") != std::string::npos) {
                consider(v, 100);
            }
        }
    }

    // Priority 2a: HTML data-* attributes
    static const char *const kDataAttrs[] = {
        "data-hls", "data-stream", "data-live-url", "data-liveurl",
        "data-video-url", "data-videourl", "data-url", "data-source", "data-src", "data-m3u8",
    };
    for (const char *attr : kDataAttrs) {
        const auto vals = findHtmlAttrValues(rawBody, lower, attr);
        for (const std::string &v : vals) {
            if (StreamSecurity::toLowerAscii(v).find(".m3u8") != std::string::npos) {
                consider(v, 90);
            }
        }
    }

    // Priority 2b: HTML <source> / <video> tags with .m3u8 src
    for (const char *tagName : {"<source", "<video"}) {
        std::size_t pos = 0;
        while ((pos = lower.find(tagName, pos)) != std::string::npos) {
            const std::size_t end = lower.find('>', pos);
            if (end == std::string::npos) {
                break;
            }
            const auto srcs = findHtmlAttrValues(rawBody.substr(pos, end - pos + 1),
                                                 lower.substr(pos, end - pos + 1),
                                                 "src");
            for (const std::string &v : srcs) {
                if (StreamSecurity::toLowerAscii(v).find(".m3u8") != std::string::npos) {
                    consider(v, 85);
                }
            }
            pos = end + 1;
        }
    }

    // Priority 2c: Percent-encoded https%3A%2F%2F...m3u8
    for (const std::string &decoded : findPercentEncodedM3u8Urls(rawBody, lower)) {
        consider(decoded, 70);
    }

    // Priority 3: Generic .m3u8 URLs
    for (const std::string &rawM3u8 : findGenericM3u8Urls(rawBody, lower)) {
        consider(rawM3u8, 60);
    }

    if (!candidates.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const ScoredCandidate &a, const ScoredCandidate &b) {
                             return a.score > b.score;
                         });
        res.ok = true;
        res.streamUrl = candidates.front().url;
        return res;
    }

    const bool hasDrm = detectDrmIndicators(lower);

    // Secondary hop 1: Turkuvaz Media container (ATV / A Haber / MinikaGO / MinikaÇOCUK)
    std::string secondary;
    if (lower.find("tmdvpcontainer") != std::string::npos || lower.find("tmgrup.com.tr") != std::string::npos) {
        const auto wsList = findHtmlAttrValues(rawBody, lower, "data-websiteid");
        const auto vidList = findHtmlAttrValues(rawBody, lower, "data-videoid");
        std::string wsId;
        std::string vidId;
        for (const std::string &w : wsList) {
            if (isUuid36(w)) {
                wsId = w;
                break;
            }
        }
        for (const std::string &v : vidList) {
            if (isUuid36(v)) {
                vidId = v;
                break;
            }
        }
        if (!wsId.empty() && !vidId.empty()) {
            const std::string apiUrl = "https://videojs.tmgrup.com.tr/getvideo/" + wsId + "/" + vidId;
            if (StreamSecurity::isAllowedMediaUrl(apiUrl) && apiUrl != baseUrlStr) {
                secondary = apiUrl;
            }
        }
    }

    // Secondary hop 2: Official YouTube Live embed or broadcaster player <iframe>
    if (secondary.empty()) {
        secondary = extractYoutubeLiveTarget(rawBody, lower, baseUrl, baseUrlStr);
    }
    if (secondary.empty()) {
        secondary = extractBroadcasterPlayerIframe(rawBody, lower, baseUrl, baseUrlStr);
    }
    if (!secondary.empty()) {
        res.secondaryPageUrl = secondary;
        res.drmDetected = hasDrm;
        res.errorMessage = "Gömülü resmi yayın oynatıcısı çözümleniyor...";
        return res;
    }

    if (hasDrm) {
        res.drmDetected = true;
        res.errorMessage = "Bu kanalın açık akışı alınamadı / DRM korumalı.";
        return res;
    }

    if (lower.find("live_stream_offline") != std::string::npos
        || (baseUrl.valid && isYoutubeHost(baseUrl.host)
            && (lower.find("\"status\":\"error\"") != std::string::npos
                || lower.find("\"status\":\"unplayable\"") != std::string::npos
                || endsWithNoCase(StreamSecurity::toLowerAscii(baseUrl.path), "/live")))) {
        res.errorMessage = "Yayıncı canlı akışı şu anda kapalı / çevrimdışı.";
        return res;
    }

    res.errorMessage = "Bu kanalın açık akışı alınamadı (geçerli HLS .m3u8 bulunamadı).";
    return res;
}

StreamResolveSession StreamResolver::createSession(const ChannelEntry &channel, bool forcePageResolve)
{
    StreamResolveSession session;
    session.channelName = channel.name;
    session.channelOriginReferer = StreamSecurity::extractSafeOriginReferer(channel.sourcePageUrl);

    std::set<std::string> seen;
    auto enqueue = [&](const std::string &raw) {
        const std::string unwrapped = StreamSecurity::unwrapResolverUrl(raw);
        if (!unwrapped.empty() && !seen.count(unwrapped)) {
            seen.insert(unwrapped);
            session.pendingCandidates.push_back(unwrapped);
        }
    };

    if (!forcePageResolve && !channel.streamUrl.empty()) {
        enqueue(channel.streamUrl);
    }
    enqueue(channel.sourcePageUrl);
    for (const std::string &fb : channel.fallbackUrls) {
        enqueue(fb);
    }
    return session;
}

ResolveOutcome StreamResolver::resolveNext(StreamResolveSession &session,
                                           const HttpFetcher &fetcher,
                                           const std::atomic<bool> *cancelFlag,
                                           const std::function<void(const std::string &)> &statusCallback)
{
    auto isCanceled = [&]() { return cancelFlag && cancelFlag->load(); };

    while (!session.pendingCandidates.empty()) {
        if (isCanceled()) {
            ResolveOutcome out;
            out.canceled = true;
            return out;
        }

        const std::string candidate = session.pendingCandidates.front();
        session.pendingCandidates.pop_front();

        if (!StreamSecurity::isAllowedMediaUrl(candidate)) {
            continue;
        }

        // Stage 1: Direct HLS/media URL passes immediately (remaining candidates stay in session for playback failover)
        if (isDirectStreamUrl(candidate)) {
            ResolveOutcome out;
            out.ok = true;
            out.fromWebPage = false;
            out.streamUrl = candidate;
            out.httpReferer = session.channelOriginReferer;
            return out;
        }

        // Stage 2: Fetch broadcaster live page / API with up to kMaxSecondaryHops
        std::set<std::string> visited;
        std::string currentUrl = candidate;
        std::string currentReferer = session.channelOriginReferer.empty()
            ? StreamSecurity::extractSafeOriginReferer(candidate)
            : session.channelOriginReferer;
        bool sawDrmHint = false;

        for (int hop = 0; hop <= kMaxSecondaryHops; ++hop) {
            if (isCanceled()) {
                ResolveOutcome out;
                out.canceled = true;
                return out;
            }

            if (visited.count(currentUrl)) {
                break;
            }
            visited.insert(currentUrl);

            ParsedUrl parsed = StreamSecurity::parseHttpUrl(currentUrl);
            if (!parsed.valid) {
                break;
            }

            const bool ytTarget = isYoutubeHost(parsed.host);
            std::string fetchUrl = currentUrl;
            const std::string hostLower = StreamSecurity::toLowerAscii(parsed.host);
            if (ytTarget && (hostLower == "www.youtube.com" || hostLower == "youtube.com")) {
                // Mobile Safari HTML exposes hlsManifestUrl directly in ytInitialPlayerResponse
                fetchUrl = parsed.scheme + "://m.youtube.com" + parsed.pathAndQuery;
                if (!StreamSecurity::isAllowedMediaUrl(fetchUrl)) {
                    break;
                }
                visited.insert(fetchUrl);
            }

            if (statusCallback) {
                if (hop == 0) {
                    statusCallback(session.channelName + " için resmi akış çözümleniyor...");
                } else {
                    statusCallback(session.channelName + " için gömülü resmi oynatıcı çözümleniyor...");
                }
            }

            HttpRequest req;
            req.url = fetchUrl;
            req.userAgent = ytTarget ? kMobileSafariUa : kDesktopBrowserUa;
            req.cancelFlag = cancelFlag;
            if (ytTarget) {
                req.cookie = "SOCS=CAI";
            } else if (!currentReferer.empty() && StreamSecurity::isAllowedMediaUrl(currentReferer)) {
                req.referer = currentReferer;
                const ParsedUrl refP = StreamSecurity::parseHttpUrl(currentReferer);
                if (refP.valid && !refP.origin.empty()) {
                    req.origin = refP.origin.substr(0, refP.origin.size() - 1);
                }
            }

            const HttpResponse resp = fetcher(req);
            if (resp.canceled || isCanceled()) {
                ResolveOutcome out;
                out.canceled = true;
                return out;
            }
            if (resp.overflow) {
                session.lastFailureReason = "Yayın sayfası 2 MiB güvenlik sınırını aştı.";
                break;
            }
            if (!resp.ok) {
                if (resp.httpStatus == 401 || resp.httpStatus == 403 || resp.httpStatus == 451) {
                    session.lastFailureReason = "Yayıncı akışı erişime kapalı / DRM korumalı (HTTP "
                                                + std::to_string(resp.httpStatus) + ").";
                    session.lastFailureWasDrm = true;
                } else if (resp.httpStatus == 404) {
                    if (session.lastFailureReason.empty()) {
                        session.lastFailureReason = "Yayın kaynağı bulunamadı (HTTP 404).";
                    }
                } else if (session.lastFailureReason.empty()) {
                    session.lastFailureReason = resp.errorMessage.empty()
                        ? "Resmi yayın sayfasına erişilemedi."
                        : resp.errorMessage;
                }
                break;
            }

            const std::string effectiveUrl = resp.finalUrl.empty() ? fetchUrl : resp.finalUrl;
            const StreamExtractResult extracted = extractStreamFromHtml(resp.body, effectiveUrl, resp.contentType);
            sawDrmHint = sawDrmHint || extracted.drmDetected;

            if (extracted.ok && StreamSecurity::isAllowedMediaUrl(extracted.streamUrl)) {
                ResolveOutcome out;
                out.ok = true;
                out.fromWebPage = true;
                out.streamUrl = extracted.streamUrl;
                out.httpReferer = ytTarget
                    ? std::string()
                    : (session.channelOriginReferer.empty()
                           ? StreamSecurity::extractSafeOriginReferer(effectiveUrl)
                           : session.channelOriginReferer);
                out.httpUserAgent = ytTarget ? kMobileSafariUa : std::string();
                return out;
            }

            if (!extracted.secondaryPageUrl.empty()
                && hop < kMaxSecondaryHops
                && StreamSecurity::isAllowedMediaUrl(extracted.secondaryPageUrl)
                && !visited.count(extracted.secondaryPageUrl)) {
                currentUrl = extracted.secondaryPageUrl;
                if (currentReferer.empty()) {
                    currentReferer = StreamSecurity::extractSafeOriginReferer(effectiveUrl);
                }
                continue;
            }

            if (sawDrmHint) {
                session.lastFailureReason = "Bu kanalın açık akışı alınamadı / DRM korumalı.";
                session.lastFailureWasDrm = true;
            } else if (!extracted.errorMessage.empty() && session.lastFailureReason.empty()) {
                session.lastFailureReason = extracted.errorMessage;
            }
            break;
        }

        if (!session.pendingCandidates.empty() && statusCallback) {
            statusCallback(session.channelName + " için yedek resmi kaynak deneniyor...");
        }
    }

    ResolveOutcome fail;
    fail.ok = false;
    fail.drmDetected = session.lastFailureWasDrm;
    fail.errorMessage = session.lastFailureReason.empty()
        ? "Bu kanalın açık akışı şu anda alınamıyor."
        : session.lastFailureReason;
    return fail;
}

} // namespace zethropol::tvwin7
