#pragma once

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

namespace zethropol::tv {

struct StreamExtractResult
{
    bool ok = false;
    bool drmDetected = false;
    QString streamUrl;
    QString secondaryPageUrl;
    QString errorMessage;
};

struct YoutubeDependencyStatus
{
    bool hasYtDlp = false;
    bool hasDeno = false;
    bool hasFfmpeg = false;
    QString ytDlpPath;
    QString denoPath;
    QString ffmpegPath;

    bool allAvailable() const { return hasYtDlp && hasDeno && hasFfmpeg; }
};

/// Two-stage official live stream resolver:
/// Stage 1: Direct HLS (.m3u8) / media URLs pass through immediately after StreamSecurity validation.
/// Stage 2: Official live broadcast web pages (`resolve://...` or channel `sourcePageUrl`) are
///          fetched over Qt6::Network with strict timeouts and byte limits to extract open HLS
///          (.m3u8) URLs, with a safe shell-free `yt-dlp` (`--js-runtimes deno`) fallback strictly
///          for validated official YouTube Live/Channel URLs.
/// Every emitted stream URL is guaranteed to pass StreamSecurity::isAllowedMediaUrl().
class StreamResolver : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool resolving READ resolving NOTIFY resolvingChanged)
    Q_PROPERTY(bool hasPendingFallbacks READ hasPendingFallbacks NOTIFY fallbacksChanged)
    Q_PROPERTY(bool lastResolvedWasYoutube READ lastResolvedWasYoutube NOTIFY lastResolvedUrlChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString lastResolvedUrl READ lastResolvedUrl NOTIFY lastResolvedUrlChanged)

public:
    static constexpr qint64 kCacheTtlSecs = 300; // 5 minutes
    static constexpr int kMaxSecondaryHops = 3;
    static constexpr int kDefaultYtDlpTimeoutMs = 18000;
    static constexpr qint64 kMaxYtDlpOutputBytes = 4 * 1024 * 1024;

    static constexpr const char *kYoutubeMissingDepsMessage =
        "YouTube tabanlı kanalların oynatılması için sistemde yt-dlp, deno ve ffmpeg paketlerinin kurulu olması gerekir.";
    static constexpr const char *kYoutubeResolverOutdatedMessage =
        "YouTube yayın çözücüsü güncelliğini yitirmiş olabilir; sistem güncellemelerini (yt-dlp) denetleyin.";

    explicit StreamResolver(QObject *parent = nullptr);
    ~StreamResolver() override;

    /// Returns true if a validated http/https URL points directly to a playable stream/playlist.
    static bool isDirectStreamUrl(const QString &url);

    /// Returns true if a validated http/https URL targets an official YouTube host.
    static bool isYoutubeUrl(const QString &url);

    /// Checks whether yt-dlp, deno, and ffmpeg executables are present on the system
    /// (or in `searchDirsOverride` when provided for deterministic tests).
    static YoutubeDependencyStatus checkYoutubeDependencies(
        const QStringList &searchDirsOverride = QStringList());

    /// Returns the user-facing Turkish warning message for a YouTube resolution failure
    /// based on whether yt-dlp/deno/ffmpeg are installed.
    static QString youtubeFailureMessage(const YoutubeDependencyStatus &deps);

    /// Pure deterministic extractor for HTML / JSON broadcaster live pages (unit-testable offline).
    /// Supports Demirören ServiceUrl+SecurePath JSON, Turkuvaz tmdvpcontainer/securevideotoken,
    /// YouTube Live hlsManifestUrl, HTML data-* / JS configs, filters ad/tracker/DRM URLs.
    static StreamExtractResult extractStreamFromHtml(const QByteArray &rawBody,
                                                     const QUrl &baseUrl,
                                                     const QString &contentType = QString());

    /// Pure deterministic extractor for `yt-dlp -J` JSON metadata output (unit-testable offline).
    /// Prefers single-URL master HLS (`hls_variant` / `.m3u8`) manifests with both audio & video.
    static StreamExtractResult extractStreamFromYtDlpJson(const QByteArray &rawJson,
                                                          QString *userAgentOut = nullptr);

    /// Deterministic test seams (empty override uses system PATH + /usr/bin).
    void setYoutubeToolSearchDirsOverride(const QStringList &dirs);
    void setYtDlpTimeoutMs(int timeoutMs);

    bool resolving() const { return m_resolving; }
    bool hasPendingFallbacks() const { return !m_pendingFallbacks.isEmpty(); }
    bool lastResolvedWasYoutube() const { return m_lastResolvedWasYoutube; }
    QString statusMessage() const { return m_statusMessage; }
    QString lastError() const { return m_lastError; }
    QString lastResolvedUrl() const { return m_lastResolvedUrl; }

    /// Returns the appropriate Turkish YouTube diagnostic message for the current host environment.
    Q_INVOKABLE QString youtubeDiagnosticMessage() const;

    /// Resolves either a direct stream URL or an official broadcaster page (`resolve://` or `sourcePageUrl`)
    /// with an optional ordered backup URL list (`fallbackUrls`) for automatic failover.
    Q_INVOKABLE void resolveChannel(const QString &streamUrl,
                                    const QString &sourcePageUrl = QString(),
                                    const QString &channelName = QString(),
                                    bool forcePageResolve = false,
                                    const QStringList &fallbackUrls = QStringList());

    /// Advances to the next validated backup URL when playback of the current URL fails.
    Q_INVOKABLE bool tryNextFallback(const QString &channelName = QString());

    /// Convenience wrapper to resolve a single URL (direct stream, http/https page, or resolve://...).
    Q_INVOKABLE void resolveUrl(const QString &inputUrl,
                                const QString &channelName = QString(),
                                bool bypassCache = false);

    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearCache();

signals:
    void resolvingChanged();
    void fallbacksChanged();
    void statusMessageChanged();
    void lastErrorChanged();
    void lastResolvedUrlChanged();

    void streamResolved(const QString &resolvedUrl, const QString &channelName, bool fromWebPage);
    void streamResolvedWithHeaders(const QString &resolvedUrl,
                                   const QString &channelName,
                                   bool fromWebPage,
                                   const QString &httpReferer,
                                   const QString &httpUserAgent);
    void resolveFailed(const QString &errorMessage, const QString &channelName, bool drmDetected);

private:
    struct CacheEntry
    {
        QString streamUrl;
        QString httpReferer;
        QString httpUserAgent;
        bool fromYoutube = false;
        QDateTime expiresAtUtc;
    };

    void startNextFallback(bool bypassCache);
    void handleCandidateFailure(const QString &errorMessage,
                                const QString &channelName,
                                bool drmDetected,
                                quint64 requestId);
    void startPageFetch(const QString &pageUrl,
                        const QString &cacheKey,
                        const QString &channelName,
                        int redirectDepth,
                        int hopDepth,
                        bool sawDrmHint,
                        const QString &refererUrl,
                        quint64 requestId);
    void startYtDlpResolve(const QString &youtubeUrl,
                           const QString &cacheKey,
                           const QString &channelName,
                           quint64 requestId);
    void stopActiveYtDlpProcess();
    void finishSuccess(const QString &resolvedUrl,
                       const QString &channelName,
                       bool fromWebPage,
                       const QString &httpReferer = QString(),
                       const QString &httpUserAgent = QString(),
                       bool fromYoutube = false);
    void finishFailure(const QString &errorMessage, const QString &channelName, bool drmDetected);
    void setResolving(bool resolving);
    void setStatusMessage(const QString &msg);
    void setLastError(const QString &err);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_activeReply;
    QPointer<QProcess> m_activeYtDlpProcess;
    QTimer m_ytDlpTimeoutTimer;
    QByteArray m_ytDlpStdout;
    QByteArray m_ytDlpStderr;
    bool m_ytDlpOverflow = false;
    QByteArray m_buffer;
    bool m_overflow = false;
    quint64 m_requestSeq = 0;
    QSet<QString> m_visitedUrls;

    QStringList m_pendingFallbacks;
    QString m_activeChannelName;
    QString m_channelOriginReferer;
    QString m_lastFailureReason;
    bool m_lastFailureWasDrm = false;
    bool m_sawYoutubeCandidate = false;
    bool m_sawYoutubeMissingDeps = false;
    bool m_sawYoutubeResolverFailure = false;
    bool m_lastResolvedWasYoutube = false;

    QStringList m_youtubeToolSearchDirsOverride;
    int m_ytDlpTimeoutMs = kDefaultYtDlpTimeoutMs;

    bool m_resolving = false;
    QString m_statusMessage;
    QString m_lastError;
    QString m_lastResolvedUrl;

    QHash<QString, CacheEntry> m_cache;
};

} // namespace zethropol::tv
