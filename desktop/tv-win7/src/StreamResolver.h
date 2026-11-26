#pragma once

#include "ChannelCatalog.h"
#include "StreamSecurity.h"

#include <atomic>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace zethropol::tvwin7 {

struct HttpRequest
{
    std::string url;
    std::string userAgent;
    std::string referer;
    std::string origin;
    std::string cookie;
    int timeoutMs = StreamSecurity::kNetworkTimeoutMs;
    std::size_t maxBytes = StreamSecurity::kMaxResolverPageBytes;
    const std::atomic<bool> *cancelFlag = nullptr;
};

struct HttpResponse
{
    bool ok = false;
    bool canceled = false;
    bool overflow = false;
    int httpStatus = 0;
    std::string finalUrl;
    std::string contentType;
    std::string body;
    std::string errorMessage;
};

using HttpFetcher = std::function<HttpResponse(const HttpRequest &)>;

struct StreamExtractResult
{
    bool ok = false;
    bool drmDetected = false;
    std::string streamUrl;
    std::string secondaryPageUrl;
    std::string errorMessage;
};

struct ResolveOutcome
{
    bool ok = false;
    bool canceled = false;
    bool drmDetected = false;
    bool fromWebPage = false;
    std::string streamUrl;
    std::string httpReferer;
    std::string httpUserAgent;
    std::string errorMessage;
};

struct StreamResolveSession
{
    std::string channelName;
    std::string channelOriginReferer;
    std::deque<std::string> pendingCandidates;
    std::string lastFailureReason;
    bool lastFailureWasDrm = false;
};

class StreamResolver
{
public:
    static constexpr int kMaxSecondaryHops = 2;
    static constexpr const char *kDesktopBrowserUa =
        "Mozilla/5.0 (Windows NT 6.1) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/109.0.0.0 Safari/537.36";
    static constexpr const char *kMobileSafariUa =
        "Mozilla/5.0 (iPhone; CPU iPhone OS 17_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Mobile/15E148 Safari/604.1";

    static bool isDirectStreamUrl(const std::string &url);
    static StreamExtractResult extractStreamFromHtml(const std::string &rawBody,
                                                     const std::string &baseUrlStr,
                                                     const std::string &contentType = {});

    static StreamResolveSession createSession(const ChannelEntry &channel, bool forcePageResolve = false);
    static ResolveOutcome resolveNext(StreamResolveSession &session,
                                      const HttpFetcher &fetcher,
                                      const std::atomic<bool> *cancelFlag = nullptr,
                                      const std::function<void(const std::string &)> &statusCallback = nullptr);
};

} // namespace zethropol::tvwin7
