#include "StreamSecurity.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace zethropol::tvwin7 {

namespace {

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexVal(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return 0;
}

bool startsWithNoCase(const std::string &s, const char *prefix)
{
    std::size_t i = 0;
    while (prefix[i] != '\0') {
        if (i >= s.size()) {
            return false;
        }
        if (std::tolower(static_cast<unsigned char>(s[i]))
            != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
        ++i;
    }
    return true;
}

bool isYoutubeHostLower(const std::string &h)
{
    return h == "youtube.com"
           || (h.size() > 12 && h.compare(h.size() - 12, 12, ".youtube.com") == 0)
           || h == "youtube-nocookie.com"
           || (h.size() > 21 && h.compare(h.size() - 21, 21, ".youtube-nocookie.com") == 0);
}

} // namespace

std::string StreamSecurity::toLowerAscii(const std::string &in)
{
    std::string out = in;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string StreamSecurity::trimAscii(const std::string &in)
{
    std::size_t start = 0;
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start]))) {
        ++start;
    }
    std::size_t end = in.size();
    while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }
    return in.substr(start, end - start);
}

ParsedUrl StreamSecurity::parseHttpUrl(const std::string &rawUrl)
{
    ParsedUrl out;
    const std::string trimmed = trimAscii(rawUrl);
    if (trimmed.empty() || trimmed.size() > kMaxUrlLength) {
        return out;
    }
    for (unsigned char c : trimmed) {
        if (c <= 0x20 || c == 0x7F) {
            return out;
        }
    }

    const std::size_t schemeEnd = trimmed.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0) {
        return out;
    }
    const std::string scheme = toLowerAscii(trimmed.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") {
        return out;
    }

    const std::size_t authStart = schemeEnd + 3;
    std::size_t authEnd = trimmed.find_first_of("/?#", authStart);
    if (authEnd == std::string::npos) {
        authEnd = trimmed.size();
    }
    const std::string authority = trimmed.substr(authStart, authEnd - authStart);
    if (authority.empty()) {
        return out;
    }
    // Reject embedded credentials (user:pass@host) or backslashes.
    if (authority.find('@') != std::string::npos || authority.find('\\') != std::string::npos) {
        return out;
    }

    std::string host;
    int port = (scheme == "https") ? 443 : 80;
    bool explicitPort = false;

    if (authority.front() == '[') {
        const std::size_t closeBracket = authority.find(']');
        if (closeBracket == std::string::npos || closeBracket == 1) {
            return out;
        }
        host = authority.substr(1, closeBracket - 1);
        if (closeBracket + 1 < authority.size()) {
            if (authority[closeBracket + 1] != ':') {
                return out;
            }
            const std::string portStr = authority.substr(closeBracket + 2);
            if (portStr.empty()) {
                return out;
            }
            long parsedPort = 0;
            for (char c : portStr) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return out;
                }
                parsedPort = parsedPort * 10 + (c - '0');
                if (parsedPort > 65535) {
                    return out;
                }
            }
            if (parsedPort <= 0) {
                return out;
            }
            port = static_cast<int>(parsedPort);
            explicitPort = true;
        }
    } else {
        const std::size_t colonPos = authority.rfind(':');
        if (colonPos != std::string::npos) {
            host = authority.substr(0, colonPos);
            const std::string portStr = authority.substr(colonPos + 1);
            if (portStr.empty()) {
                return out;
            }
            long parsedPort = 0;
            for (char c : portStr) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return out;
                }
                parsedPort = parsedPort * 10 + (c - '0');
                if (parsedPort > 65535) {
                    return out;
                }
            }
            if (parsedPort <= 0) {
                return out;
            }
            port = static_cast<int>(parsedPort);
            explicitPort = true;
        } else {
            host = authority;
        }
    }

    if (host.empty() || host.find('/') != std::string::npos || host.find('\\') != std::string::npos) {
        return out;
    }
    for (unsigned char c : host) {
        if (c <= 0x20 || c >= 0x7F || c == ':' || c == '?' || c == '#' || c == '@') {
            return out;
        }
    }

    // Strip fragment (#...)
    std::string remainder;
    if (authEnd < trimmed.size()) {
        std::size_t hashPos = trimmed.find('#', authEnd);
        remainder = (hashPos == std::string::npos)
            ? trimmed.substr(authEnd)
            : trimmed.substr(authEnd, hashPos - authEnd);
    }

    std::string path = "/";
    std::string query;
    if (!remainder.empty()) {
        if (remainder[0] == '?') {
            query = remainder.substr(1);
        } else {
            const std::size_t qPos = remainder.find('?');
            if (qPos == std::string::npos) {
                path = remainder;
            } else {
                path = remainder.substr(0, qPos);
                query = remainder.substr(qPos + 1);
            }
            if (path.empty()) {
                path = "/";
            }
        }
    }

    out.valid = true;
    out.scheme = scheme;
    out.host = host;
    out.port = port;
    out.explicitPort = explicitPort;
    out.path = path;
    out.query = query;
    out.pathAndQuery = query.empty() ? path : (path + "?" + query);

    out.origin = scheme + "://" + host;
    if (explicitPort) {
        out.origin += ":" + std::to_string(port);
    }
    out.origin += "/";
    return out;
}

bool StreamSecurity::isAllowedMediaUrl(const std::string &rawUrl, std::string *reasonOut)
{
    auto reject = [&](const std::string &reason) {
        if (reasonOut) {
            *reasonOut = reason;
        }
        return false;
    };

    const std::string trimmed = trimAscii(rawUrl);
    if (trimmed.empty()) {
        return reject("Boş yayın adresi.");
    }
    if (trimmed.size() > kMaxUrlLength) {
        return reject("Yayın adresi izin verilen maksimum uzunluğu aşıyor.");
    }
    if (trimmed[0] == '-' || trimmed[0] == '/' || trimmed[0] == '.') {
        return reject("Yerel yol veya komut bayrağı biçimindeki adresler yasaktır.");
    }
    for (unsigned char c : trimmed) {
        if (c <= 0x20 || c == 0x7F) {
            return reject("Yayın adresi geçersiz veya kontrol karakteri içeriyor.");
        }
    }

    const std::string lower = toLowerAscii(trimmed);
    if (lower.find("%00") != std::string::npos
        || lower.find("%0a") != std::string::npos
        || lower.find("%0d") != std::string::npos) {
        return reject("Yayın adresi kodlanmış kontrol karakteri içeriyor.");
    }

    if (!startsWithNoCase(trimmed, "http://") && !startsWithNoCase(trimmed, "https://")) {
        return reject("Yalnızca http:// ve https:// yayın şemalarına izin verilir.");
    }

    const ParsedUrl parsed = parseHttpUrl(trimmed);
    if (!parsed.valid) {
        return reject("Geçersiz URL biçimi veya gömülü kimlik bilgisi.");
    }
    return true;
}

std::string StreamSecurity::unwrapResolverUrl(const std::string &rawInput,
                                              bool *wasResolveScheme,
                                              std::string *reasonOut)
{
    if (wasResolveScheme) {
        *wasResolveScheme = false;
    }
    std::string candidate = trimAscii(rawInput);
    if (startsWithNoCase(candidate, "resolve://")) {
        if (wasResolveScheme) {
            *wasResolveScheme = true;
        }
        candidate = trimAscii(candidate.substr(10));
    }
    if (!isAllowedMediaUrl(candidate, reasonOut)) {
        return {};
    }
    return candidate;
}

std::string StreamSecurity::extractSafeOriginReferer(const std::string &pageUrl)
{
    if (!isAllowedMediaUrl(pageUrl)) {
        return {};
    }
    const ParsedUrl parsed = parseHttpUrl(pageUrl);
    if (!parsed.valid || parsed.host.empty() || isYoutubeHostLower(toLowerAscii(parsed.host))) {
        return {};
    }
    return isAllowedMediaUrl(parsed.origin) ? parsed.origin : std::string();
}

std::string StreamSecurity::resolveRelativeUrl(const std::string &candidate, const ParsedUrl &base)
{
    const std::string cleaned = trimAscii(candidate);
    if (cleaned.empty()) {
        return {};
    }
    std::string resolved;
    if (cleaned.size() >= 2 && cleaned[0] == '/' && cleaned[1] == '/') {
        const std::string scheme = (base.valid && !base.scheme.empty()) ? base.scheme : "https";
        resolved = scheme + ":" + cleaned;
    } else if (cleaned.find("://") != std::string::npos) {
        resolved = cleaned;
    } else {
        // Reject scheme-like prefixes (javascript:, data:, etc.)
        const std::size_t colonPos = cleaned.find(':');
        const std::size_t slashPos = cleaned.find('/');
        if (colonPos != std::string::npos && (slashPos == std::string::npos || colonPos < slashPos)) {
            return {};
        }
        if (!base.valid) {
            return {};
        }
        const std::string baseNoSlash = base.origin.substr(0, base.origin.size() - 1);
        if (cleaned[0] == '/') {
            resolved = baseNoSlash + cleaned;
        } else if (cleaned[0] == '?') {
            resolved = baseNoSlash + base.path + cleaned;
        } else {
            const std::size_t lastSlash = base.path.rfind('/');
            const std::string dir = (lastSlash == std::string::npos) ? "/" : base.path.substr(0, lastSlash + 1);
            resolved = baseNoSlash + dir + cleaned;
        }
    }
    return isAllowedMediaUrl(resolved) ? resolved : std::string();
}

std::string StreamSecurity::sanitizeUserAgent(const std::string &raw, const std::string &fallback)
{
    std::string out;
    out.reserve(std::min<std::size_t>(raw.size(), 240));
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        out.push_back(static_cast<char>(c));
        if (out.size() >= 240) {
            break;
        }
    }
    out = trimAscii(out);
    return out.empty() ? fallback : out;
}

std::string StreamSecurity::percentDecode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && isHexDigit(in[i + 1]) && isHexDigit(in[i + 2])) {
            const char decoded = static_cast<char>((hexVal(in[i + 1]) << 4) | hexVal(in[i + 2]));
            out.push_back(decoded);
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string StreamSecurity::percentEncode(const std::string &in)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

} // namespace zethropol::tvwin7
