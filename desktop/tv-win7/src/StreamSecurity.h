#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace zethropol::tvwin7 {

struct ParsedUrl
{
    bool valid = false;
    std::string scheme;       // lowercase "http" or "https"
    std::string host;         // host without port
    int port = 0;             // explicit port, or default 80 / 443
    bool explicitPort = false;
    std::string path;         // starts with '/'
    std::string query;        // without leading '?'
    std::string pathAndQuery; // path + optional ?query
    std::string origin;       // scheme://host[:port]/
};

class StreamSecurity
{
public:
    static constexpr std::size_t kMaxUrlLength = 4096;
    static constexpr std::size_t kMaxResolverPageBytes = 2 * 1024 * 1024; // 2 MiB
    static constexpr int kMaxRedirects = 5;
    static constexpr int kNetworkTimeoutMs = 8000;

    static bool isAllowedMediaUrl(const std::string &rawUrl, std::string *reasonOut = nullptr);
    static ParsedUrl parseHttpUrl(const std::string &rawUrl);
    static std::string unwrapResolverUrl(const std::string &rawInput,
                                         bool *wasResolveScheme = nullptr,
                                         std::string *reasonOut = nullptr);
    static std::string extractSafeOriginReferer(const std::string &pageUrl);
    static std::string resolveRelativeUrl(const std::string &candidate, const ParsedUrl &base);
    static std::string sanitizeUserAgent(const std::string &raw, const std::string &fallback);
    static std::string percentDecode(const std::string &in);
    static std::string percentEncode(const std::string &in);
    static std::string toLowerAscii(const std::string &in);
    static std::string trimAscii(const std::string &in);
};

} // namespace zethropol::tvwin7
