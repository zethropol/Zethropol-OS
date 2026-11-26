#include "StreamSecurity.h"

#include <QFileInfo>
#include <QUrl>

namespace zethropol::tv {

bool StreamSecurity::isAllowedMediaUrl(const QString &rawUrl, QString *reasonOut)
{
    auto reject = [&](const QString &reason) {
        if (reasonOut) {
            *reasonOut = reason;
        }
        return false;
    };

    const QString trimmed = rawUrl.trimmed();
    if (trimmed.isEmpty()) {
        return reject(QStringLiteral("Boş yayın adresi."));
    }
    if (trimmed.size() > kMaxUrlLength) {
        return reject(QStringLiteral("Yayın adresi izin verilen maksimum uzunluğu aşıyor."));
    }

    // Reject control chars, newlines, NULs, whitespace or mpv option-style prefixes.
    if (trimmed.startsWith(QLatin1Char('-')) || trimmed.startsWith(QLatin1Char('/'))
        || trimmed.startsWith(QLatin1Char('.'))) {
        return reject(QStringLiteral("Yerel yol veya komut bayrağı biçimindeki adresler yasaktır."));
    }
    for (const QChar ch : trimmed) {
        const ushort code = ch.unicode();
        if (code <= 0x20 || code == 0x7F) {
            return reject(QStringLiteral("Yayın adresi geçersiz veya kontrol karakteri içeriyor."));
        }
    }

    // Reject percent-encoded control/NUL injection attempts.
    const QString lower = trimmed.toLower();
    if (lower.contains(QStringLiteral("%00")) || lower.contains(QStringLiteral("%0a"))
        || lower.contains(QStringLiteral("%0d"))) {
        return reject(QStringLiteral("Yayın adresi kodlanmış kontrol karakteri içeriyor."));
    }

    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative()) {
        return reject(QStringLiteral("Geçersiz URL biçimi."));
    }

    const QString scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return reject(QStringLiteral("Yalnızca http:// ve https:// yayın şemalarına izin verilir (reddedilen: %1).")
                          .arg(scheme.isEmpty() ? QStringLiteral("yok") : scheme));
    }

    // Require explicit authority separator (`http://` or `https://`).
    if (!lower.startsWith(QStringLiteral("http://"))
        && !lower.startsWith(QStringLiteral("https://"))) {
        return reject(QStringLiteral("URL açık http:// veya https:// önekiyle başlamalıdır."));
    }

    if (!url.userInfo().isEmpty()) {
        return reject(QStringLiteral("URL içinde gömülü kimlik bilgisi desteklenmez."));
    }

    const QString host = url.host();
    if (host.isEmpty() || host.contains(QLatin1Char('/')) || host.contains(QLatin1Char('\\'))) {
        return reject(QStringLiteral("Geçerli bir sunucu adı (host) bulunamadı."));
    }

    if (url.port() == 0 || url.port() > 65535) {
        return reject(QStringLiteral("Geçersiz port numarası."));
    }

    return true;
}

bool StreamSecurity::isAllowedLogoUrl(const QString &rawUrl)
{
    if (rawUrl.trimmed().isEmpty()) {
        return false;
    }
    return isAllowedMediaUrl(rawUrl, nullptr);
}

QString StreamSecurity::unwrapResolverUrl(const QString &rawInput,
                                          bool *wasResolveScheme,
                                          QString *reasonOut)
{
    if (wasResolveScheme) {
        *wasResolveScheme = false;
    }
    QString candidate = rawInput.trimmed();
    if (candidate.startsWith(QStringLiteral("resolve://"), Qt::CaseInsensitive)) {
        if (wasResolveScheme) {
            *wasResolveScheme = true;
        }
        candidate = candidate.mid(10).trimmed();
    }
    if (!isAllowedMediaUrl(candidate, reasonOut)) {
        return QString();
    }
    return candidate;
}

QString StreamSecurity::resolveSafeLocalImportPath(const QString &pathOrFileUrl,
                                                   qint64 maxFileBytes,
                                                   QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut) {
            *errorOut = msg;
        }
        return QString();
    };

    const QString trimmed = pathOrFileUrl.trimmed();
    if (trimmed.isEmpty()) {
        return fail(QStringLiteral("Dosya yolu boş."));
    }

    QString localPath = trimmed;
    if (trimmed.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive)) {
        const QUrl url(trimmed);
        if (!url.isValid() || !url.isLocalFile()) {
            return fail(QStringLiteral("Geçersiz yerel dosya bağlantısı."));
        }
        localPath = url.toLocalFile();
    } else if (trimmed.contains(QStringLiteral("://"))) {
        return fail(QStringLiteral("Yerel dosya içe aktarma için uzak şema kullanılamaz."));
    }

    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile() || !info.isReadable()) {
        return fail(QStringLiteral("Dosya bulunamadı veya okunamıyor."));
    }
    if (info.size() <= 0) {
        return fail(QStringLiteral("Dosya boş."));
    }
    if (info.size() > maxFileBytes) {
        return fail(QStringLiteral("Dosya boyutu izin verilen sınırı (%1 MiB) aşıyor.")
                        .arg(maxFileBytes / (1024 * 1024)));
    }

    return info.canonicalFilePath();
}

QString StreamSecurity::sanitizeDisplayText(const QString &raw, int maxLength)
{
    QString out;
    out.reserve(qMin(raw.size(), maxLength));
    bool lastWasSpace = false;

    for (const QChar ch : raw) {
        const ushort code = ch.unicode();
        if (code < 0x20 || code == 0x7F) {
            if (ch == QLatin1Char('\t') || ch == QLatin1Char('\n') || ch == QLatin1Char('\r')) {
                if (!lastWasSpace && !out.isEmpty()) {
                    out.append(QLatin1Char(' '));
                    lastWasSpace = true;
                }
            }
            continue;
        }
        if (ch.isSpace()) {
            if (!lastWasSpace && !out.isEmpty()) {
                out.append(QLatin1Char(' '));
                lastWasSpace = true;
            }
            continue;
        }
        out.append(ch);
        lastWasSpace = false;
        if (out.size() >= maxLength) {
            break;
        }
    }

    return out.trimmed();
}

} // namespace zethropol::tv
