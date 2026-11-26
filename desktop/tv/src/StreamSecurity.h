#pragma once

#include <QString>
#include <QUrl>

#include <cstdint>

namespace zethropol::tv {

class StreamSecurity
{
public:
    static constexpr int kNetworkTimeoutMs = 15000;
    static constexpr qint64 kMaxM3uBytes = 4 * 1024 * 1024;          // 4 MiB
    static constexpr qint64 kMaxXmltvBytes = 16 * 1024 * 1024;       // 16 MiB
    static constexpr qint64 kMaxResolverPageBytes = 2 * 1024 * 1024; // 2 MiB
    static constexpr int kMaxRedirects = 3;
    static constexpr int kMaxChannels = 5000;
    static constexpr int kMaxEpgPrograms = 50000;
    static constexpr int kMaxUrlLength = 2048;

    /// Strictly permits only http:// and https:// URLs with a valid non-empty host.
    /// Rejects file://, exec://, lua://, edl://, av://, fd://, memory://, data:,
    /// javascript:, mpv://, resolve://, local/relative paths, control characters, and CLI flags.
    static bool isAllowedMediaUrl(const QString &rawUrl, QString *reasonOut = nullptr);

    /// Unwraps an optional `resolve://` prefix and validates the underlying target URL
    /// strictly via `isAllowedMediaUrl()`. Returns empty string on rejection.
    static QString unwrapResolverUrl(const QString &rawInput,
                                     bool *wasResolveScheme = nullptr,
                                     QString *reasonOut = nullptr);

    /// Validates remote channel logo URLs under the same strict http/https rules.
    static bool isAllowedLogoUrl(const QString &rawUrl);

    /// Validates an incoming local filesystem path (or file:// URL from FileDialog)
    /// strictly for reading user-selected M3U/XMLTV playlist files — never for media playback.
    static QString resolveSafeLocalImportPath(const QString &pathOrFileUrl,
                                              qint64 maxFileBytes,
                                              QString *errorOut = nullptr);

    /// Strips control characters and clamps untrusted text length for safe UI rendering.
    static QString sanitizeDisplayText(const QString &raw, int maxLength = 160);
};

} // namespace zethropol::tv
