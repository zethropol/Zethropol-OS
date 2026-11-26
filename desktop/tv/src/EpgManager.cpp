#include "EpgManager.h"
#include "StreamSecurity.h"

#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimeZone>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace zethropol::tv {

namespace {

QByteArray maybeDecompressXmltv(const QByteArray &rawBytes, QString *errorOut)
{
    // Detect standard gzip header (0x1f 0x8b) and decompress with a strict 16 MiB output cap
    // to prevent gzip-bomb memory exhaustion.
    if (rawBytes.size() < 2
        || static_cast<unsigned char>(rawBytes[0]) != 0x1f
        || static_cast<unsigned char>(rawBytes[1]) != 0x8b) {
        return rawBytes;
    }

    z_stream zs{};
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(rawBytes.constData()));
    zs.avail_in = static_cast<uInt>(rawBytes.size());

    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        if (errorOut) {
            *errorOut = QStringLiteral("Gzip XMLTV akışı başlatılamadı.");
        }
        return {};
    }

    QByteArray out;
    out.reserve(qMin<qint64>(rawBytes.size() * 4, StreamSecurity::kMaxXmltvBytes));
    char chunk[16384];
    int ret = Z_OK;

    while (ret == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef *>(chunk);
        zs.avail_out = static_cast<uInt>(sizeof(chunk));
        ret = inflate(&zs, Z_NO_FLUSH);
        const qint64 produced = static_cast<qint64>(sizeof(chunk) - zs.avail_out);
        if (produced > 0) {
            if (out.size() + produced > StreamSecurity::kMaxXmltvBytes) {
                inflateEnd(&zs);
                if (errorOut) {
                    *errorOut = QStringLiteral("Sıkıştırılmış XMLTV açıldığında 16 MiB güvenlik sınırını aşıyor.");
                }
                return {};
            }
            out.append(chunk, static_cast<qsizetype>(produced));
        }
    }

    inflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        if (errorOut) {
            *errorOut = QStringLiteral("Gzip XMLTV verisi bozuk veya eksik.");
        }
        return {};
    }
    return out;
}

QString formatLocalTimeRange(const QDateTime &startUtc, const QDateTime &stopUtc)
{
    const QDateTime localStart = startUtc.toLocalTime();
    const QDateTime localStop = stopUtc.toLocalTime();
    return QStringLiteral("%1 – %2")
        .arg(localStart.toString(QStringLiteral("HH:mm")),
             localStop.toString(QStringLiteral("HH:mm")));
}

} // namespace

EpgManager::EpgManager(bool loadDemoEpg, QObject *parent)
    : QObject(parent)
{
    m_refreshTimer.setInterval(30000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &EpgManager::refresh);
    m_refreshTimer.start();

    if (loadDemoEpg) {
        loadDemoSchedule();
    }
}

EpgManager::~EpgManager()
{
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
    }
}

QDateTime EpgManager::parseXmltvTimestamp(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    // Standard XMLTV: YYYYMMDDHHMMSS +HHMM (seconds and timezone optional)
    static const QRegularExpression kPattern(
        QStringLiteral(R"(^(\d{4})(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})?(?:\s*([+-])(\d{2})(\d{2}))?$)"));
    const QRegularExpressionMatch m = kPattern.match(trimmed);
    if (!m.hasMatch()) {
        return {};
    }

    const QDate date(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    const QTime time(m.captured(4).toInt(), m.captured(5).toInt(),
                     m.captured(6).isEmpty() ? 0 : m.captured(6).toInt());
    if (!date.isValid() || !time.isValid()) {
        return {};
    }

    int offsetSecs = 0;
    if (!m.captured(7).isEmpty()) {
        const int sign = (m.captured(7) == QStringLiteral("-")) ? -1 : 1;
        const int tzH = m.captured(8).toInt();
        const int tzM = m.captured(9).toInt();
        if (tzH > 14 || tzM >= 60) {
            return {};
        }
        offsetSecs = sign * (tzH * 3600 + tzM * 60);
    }

    QDateTime dt(date, time, QTimeZone::fromSecondsAheadOfUtc(offsetSecs));
    return dt.isValid() ? dt.toUTC() : QDateTime();
}

EpgParseResult EpgManager::parseXmltvContent(const QByteArray &rawBytes)
{
    EpgParseResult result;
    if (rawBytes.isEmpty()) {
        result.errorMessage = QStringLiteral("XMLTV içeriği boş.");
        return result;
    }
    if (rawBytes.size() > StreamSecurity::kMaxXmltvBytes) {
        result.errorMessage = QStringLiteral("XMLTV içeriği 16 MiB güvenlik sınırını aşıyor.");
        return result;
    }

    QString decompressError;
    const QByteArray xmlBytes = maybeDecompressXmltv(rawBytes, &decompressError);
    if (!decompressError.isEmpty()) {
        result.errorMessage = decompressError;
        return result;
    }

    // QXmlStreamReader never resolves external DTD entities (immune to XXE).
    QXmlStreamReader xml(xmlBytes);
    xml.setEntityExpansionLimit(4096);

    QString currentChannelDefId;
    bool inProgramme = false;
    EpgProgram currentProg;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringView tag = xml.name();
            if (tag == QLatin1String("channel")) {
                currentChannelDefId = StreamSecurity::sanitizeDisplayText(
                    xml.attributes().value(QLatin1String("id")).toString(), 128);
            } else if (tag == QLatin1String("display-name") && !currentChannelDefId.isEmpty()) {
                const QString displayName = StreamSecurity::sanitizeDisplayText(
                    xml.readElementText(QXmlStreamReader::SkipChildElements), 120);
                if (!displayName.isEmpty()) {
                    result.displayNameToChannelId.insert(displayName.toLower(), currentChannelDefId);
                }
            } else if (tag == QLatin1String("programme")) {
                if (result.totalPrograms >= StreamSecurity::kMaxEpgPrograms) {
                    continue;
                }
                const auto attrs = xml.attributes();
                currentProg = EpgProgram{};
                currentProg.channelId = StreamSecurity::sanitizeDisplayText(
                    attrs.value(QLatin1String("channel")).toString(), 128);
                currentProg.startUtc = parseXmltvTimestamp(attrs.value(QLatin1String("start")).toString());
                currentProg.stopUtc = parseXmltvTimestamp(attrs.value(QLatin1String("stop")).toString());
                inProgramme = !currentProg.channelId.isEmpty()
                              && currentProg.startUtc.isValid()
                              && currentProg.stopUtc.isValid()
                              && currentProg.stopUtc > currentProg.startUtc;
            } else if (inProgramme && tag == QLatin1String("title")) {
                if (currentProg.title.isEmpty()) {
                    currentProg.title = StreamSecurity::sanitizeDisplayText(
                        xml.readElementText(QXmlStreamReader::SkipChildElements), 140);
                }
            } else if (inProgramme && tag == QLatin1String("desc")) {
                if (currentProg.description.isEmpty()) {
                    currentProg.description = StreamSecurity::sanitizeDisplayText(
                        xml.readElementText(QXmlStreamReader::SkipChildElements), 320);
                }
            } else if (inProgramme && tag == QLatin1String("category")) {
                if (currentProg.category.isEmpty()) {
                    currentProg.category = StreamSecurity::sanitizeDisplayText(
                        xml.readElementText(QXmlStreamReader::SkipChildElements), 64);
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView tag = xml.name();
            if (tag == QLatin1String("channel")) {
                currentChannelDefId.clear();
            } else if (tag == QLatin1String("programme")) {
                if (inProgramme && !currentProg.title.isEmpty()) {
                    result.programsByChannel[currentProg.channelId].append(currentProg);
                    ++result.totalPrograms;
                }
                inProgramme = false;
            }
        }
    }

    if (xml.hasError() && result.totalPrograms == 0) {
        result.errorMessage = QStringLiteral("XMLTV ayrıştırma hatası: %1").arg(xml.errorString());
        return result;
    }

    for (auto it = result.programsByChannel.begin(); it != result.programsByChannel.end(); ++it) {
        std::sort(it->begin(), it->end(), [](const EpgProgram &a, const EpgProgram &b) {
            return a.startUtc < b.startUtc;
        });
    }

    return result;
}

void EpgManager::setActiveChannel(const QString &tvgId, const QString &channelName)
{
    m_activeTvgId = tvgId.trimmed();
    m_activeChannelName = channelName.trimmed();
    refresh();
}

void EpgManager::refresh()
{
    refreshForTime(QDateTime::currentDateTimeUtc());
}

QString EpgManager::resolveChannelKey(const QString &tvgId, const QString &channelName) const
{
    if (!tvgId.isEmpty()) {
        if (m_programsByChannel.contains(tvgId)) {
            return tvgId;
        }
        const QString mappedById = m_displayNameToChannelId.value(tvgId.toLower());
        if (!mappedById.isEmpty() && m_programsByChannel.contains(mappedById)) {
            return mappedById;
        }
        const int atPos = tvgId.indexOf(QLatin1Char('@'));
        if (atPos > 0) {
            const QString baseId = tvgId.left(atPos);
            if (m_programsByChannel.contains(baseId)) {
                return baseId;
            }
            const QString mappedBase = m_displayNameToChannelId.value(baseId.toLower());
            if (!mappedBase.isEmpty() && m_programsByChannel.contains(mappedBase)) {
                return mappedBase;
            }
        }
    }
    if (!channelName.isEmpty()) {
        const QString mapped = m_displayNameToChannelId.value(channelName.toLower());
        if (!mapped.isEmpty() && m_programsByChannel.contains(mapped)) {
            return mapped;
        }
        if (m_programsByChannel.contains(channelName)) {
            return channelName;
        }
    }
    return QString();
}

void EpgManager::refreshForTime(const QDateTime &nowUtc)
{
    m_hasEpgData = false;
    m_hasCurrentProgram = false;
    m_currentTitle.clear();
    m_currentDescription.clear();
    m_currentTimeRange.clear();
    m_currentProgress = 0.0;
    m_currentRemainingMinutes = 0;
    m_upcomingPrograms.clear();

    const QDateTime now = nowUtc.isValid() ? nowUtc.toUTC() : QDateTime::currentDateTimeUtc();
    const QDateTime windowEnd = now.addSecs(kTwoHourWindowSecs);

    const QString key = resolveChannelKey(m_activeTvgId, m_activeChannelName);
    if (key.isEmpty()) {
        emit epgChanged();
        return;
    }

    const QVector<EpgProgram> &programs = m_programsByChannel.value(key);
    if (programs.isEmpty()) {
        emit epgChanged();
        return;
    }

    for (const EpgProgram &prog : programs) {
        if (prog.stopUtc <= now) {
            continue;
        }

        if (prog.startUtc <= now && prog.stopUtc > now) {
            m_hasEpgData = true;
            m_hasCurrentProgram = true;
            m_currentTitle = prog.title;
            m_currentDescription = prog.description;
            m_currentTimeRange = formatLocalTimeRange(prog.startUtc, prog.stopUtc);

            const qint64 totalSecs = prog.startUtc.secsTo(prog.stopUtc);
            const qint64 elapsedSecs = prog.startUtc.secsTo(now);
            const qint64 remainingSecs = now.secsTo(prog.stopUtc);
            m_currentProgress = (totalSecs > 0)
                                    ? std::clamp(static_cast<double>(elapsedSecs) / static_cast<double>(totalSecs), 0.0, 1.0)
                                    : 0.0;
            m_currentRemainingMinutes = static_cast<int>(std::max<qint64>(1, (remainingSecs + 59) / 60));
            continue;
        }

        // Upcoming 2-hour window: startUtc > now && startUtc <= now + 2 hours.
        if (prog.startUtc > now && prog.startUtc <= windowEnd) {
            m_hasEpgData = true;
            const qint64 durationMins = std::max<qint64>(1, prog.startUtc.secsTo(prog.stopUtc) / 60);
            const qint64 startsInMins = std::max<qint64>(1, (now.secsTo(prog.startUtc) + 59) / 60);

            QVariantMap item;
            item.insert(QStringLiteral("title"), prog.title);
            item.insert(QStringLiteral("description"), prog.description);
            item.insert(QStringLiteral("category"), prog.category);
            item.insert(QStringLiteral("timeRange"), formatLocalTimeRange(prog.startUtc, prog.stopUtc));
            item.insert(QStringLiteral("startTime"), prog.startUtc.toLocalTime().toString(QStringLiteral("HH:mm")));
            item.insert(QStringLiteral("durationMinutes"), static_cast<int>(durationMins));
            item.insert(QStringLiteral("startsInMinutes"), static_cast<int>(startsInMins));
            m_upcomingPrograms.append(item);
        }
    }

    emit epgChanged();
}

bool EpgManager::loadXmltvBytes(const QByteArray &rawBytes, const QString &sourceLabel)
{
    const EpgParseResult parsed = parseXmltvContent(rawBytes);
    if (!parsed.errorMessage.isEmpty()) {
        setStatusMessage(parsed.errorMessage);
        return false;
    }

    for (auto it = parsed.programsByChannel.constBegin(); it != parsed.programsByChannel.constEnd(); ++it) {
        m_programsByChannel.insert(it.key(), it.value());
    }
    for (auto it = parsed.displayNameToChannelId.constBegin(); it != parsed.displayNameToChannelId.constEnd(); ++it) {
        m_displayNameToChannelId.insert(it.key(), it.value());
    }

    setStatusMessage(QStringLiteral("%1 üzerinden %2 EPG programı yüklendi.")
                         .arg(sourceLabel)
                         .arg(parsed.totalPrograms));
    refresh();
    return parsed.totalPrograms > 0;
}

bool EpgManager::importXmltvFromFile(const QString &pathOrFileUrl)
{
    QString err;
    const QString safePath = StreamSecurity::resolveSafeLocalImportPath(
        pathOrFileUrl, StreamSecurity::kMaxXmltvBytes, &err);
    if (safePath.isEmpty()) {
        setStatusMessage(QStringLiteral("XMLTV dosyası reddedildi: %1").arg(err));
        return false;
    }

    QFile file(safePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatusMessage(QStringLiteral("XMLTV dosyası açılamadı."));
        return false;
    }
    const QByteArray bytes = file.read(StreamSecurity::kMaxXmltvBytes + 1);
    if (bytes.size() > StreamSecurity::kMaxXmltvBytes) {
        setStatusMessage(QStringLiteral("XMLTV dosyası 16 MiB güvenlik sınırını aşıyor."));
        return false;
    }

    return loadXmltvBytes(bytes, QFileInfo(safePath).fileName());
}

void EpgManager::importXmltvFromUrl(const QString &url)
{
    QString reason;
    if (!StreamSecurity::isAllowedMediaUrl(url, &reason)) {
        setStatusMessage(QStringLiteral("Güvenlik engeli: %1").arg(reason));
        return;
    }
    startRemoteXmltvFetch(url.trimmed(), 0);
}

void EpgManager::startRemoteXmltvFetch(const QString &url, int redirectDepth)
{
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }

    setLoading(true);
    setStatusMessage(QStringLiteral("XMLTV yayın akışı indiriliyor..."));
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
        if (m_downloadBuffer.size() + chunk.size() > StreamSecurity::kMaxXmltvBytes) {
            m_downloadOverflow = true;
            reply->abort();
            return;
        }
        m_downloadBuffer.append(chunk);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, redirectDepth]() {
        reply->deleteLater();
        if (m_activeReply != reply) {
            return;
        }
        m_activeReply = nullptr;

        if (m_downloadOverflow) {
            setLoading(false);
            setStatusMessage(QStringLiteral("İndirme durduruldu: XMLTV boyutu 16 MiB sınırını aştı."));
            return;
        }

        const QVariant redirectTarget = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (redirectTarget.isValid()) {
            if (redirectDepth >= StreamSecurity::kMaxRedirects) {
                setLoading(false);
                setStatusMessage(QStringLiteral("Çok fazla yönlendirme nedeniyle XMLTV indirme reddedildi."));
                return;
            }
            const QUrl resolved = reply->url().resolved(redirectTarget.toUrl());
            QString reason;
            if (!StreamSecurity::isAllowedMediaUrl(resolved.toString(), &reason)) {
                setLoading(false);
                setStatusMessage(QStringLiteral("Güvensiz EPG yönlendirmesi engellendi: %1").arg(reason));
                return;
            }
            startRemoteXmltvFetch(resolved.toString(), redirectDepth + 1);
            return;
        }

        setLoading(false);
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(QStringLiteral("XMLTV indirilemedi: %1").arg(reply->errorString()));
            return;
        }

        loadXmltvBytes(m_downloadBuffer, reply->url().host());
    });
}

void EpgManager::loadDemoSchedule()
{
    // Built-in Turkish EPG schedule aligned with all 25 Turkish default channels so users
    // immediately see realistic live + upcoming 2-hour Turkish programming on launch
    // until an external XMLTV feed (e.g. epg_ripper_TR1.xml.gz) is imported.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime base = QDateTime(now.date(), QTime(now.time().hour(), (now.time().minute() / 30) * 30, 0), QTimeZone::utc());

    struct SlotDef {
        int startOffsetMin;
        int durationMin;
        const char *title;
        const char *desc;
        const char *cat;
    };

    auto registerChannel = [&](const QString &chId, const QString &displayName,
                               std::initializer_list<SlotDef> slotDefs) {
        m_displayNameToChannelId.insert(displayName.toLower(), chId);
        m_displayNameToChannelId.insert(chId.toLower(), chId);
        for (const SlotDef &s : slotDefs) {
            EpgProgram p;
            p.channelId = chId;
            p.startUtc = base.addSecs(s.startOffsetMin * 60);
            p.stopUtc = p.startUtc.addSecs(s.durationMin * 60);
            p.title = QString::fromUtf8(s.title);
            p.description = QString::fromUtf8(s.desc);
            p.category = QString::fromUtf8(s.cat);
            m_programsByChannel[chId].append(p);
        }
    };

    // --- Ulusal Kanallar (1-10) ---
    registerChannel(QStringLiteral("TRT1.tr"), QStringLiteral("TRT 1"), {
        {0, 45, "TRT 1 Ana Haber", "Türkiye ve dünyadan günün öne çıkan gelişmeleri.", "Haber"},
        {45, 50, "Gönül Dağı Özel", "Anadolu'nun sıcak öyküleri ve yerli yapım kuşağı.", "Dizi"},
        {95, 55, "Teşkilat", "Aksiyon ve gerilim dolu yerli dizi bölümü.", "Dizi"},
        {150, 60, "Gece Sineması", "Türk sinemasından seçkin yapımlar.", "Sinema"},
    });

    registerChannel(QStringLiteral("ATV.tr"), QStringLiteral("ATV"), {
        {0, 45, "ATV Ana Haber", "Günün sıcak başlıkları ve canlı bağlantılar.", "Haber"},
        {45, 50, "Kim Milyoner Olmak İster?", "Bilgi ve genel kültür yarışması.", "Yarışma"},
        {95, 55, "Kuruluş Osman", "Tarihi drama kuşağı özel bölümü.", "Dizi"},
        {150, 60, "Son Durak", "Gece bülteni ve kültür magazin özeti.", "Yaşam"},
    });

    registerChannel(QStringLiteral("ShowTV.tr"), QStringLiteral("Show TV"), {
        {0, 45, "Show Ana Haber", "Günün öne çıkan haberleri ve dosya başlıkları.", "Haber"},
        {45, 50, "Güldür Güldür Show", "Komedi ve sahne skeçleri kuşağı.", "Eğlence"},
        {95, 55, "Kızılcık Şerbeti", "Prime time yerli drama bölümü.", "Dizi"},
        {150, 60, "Show Sinema Kuşağı", "Seçme yerli ve yabancı sinema.", "Sinema"},
    });

    registerChannel(QStringLiteral("StarTV.tr"), QStringLiteral("Star TV"), {
        {0, 45, "Star Haber", "Türkiye gündemi, ekonomi ve yaşam haberleri.", "Haber"},
        {45, 50, "Çok Güzel Hareketler", "Sahne komedisi ve eğlence programı.", "Eğlence"},
        {95, 55, "Yalı Çapkını", "Akşam kuşağı yerli dizi bölümü.", "Dizi"},
        {150, 60, "Gece Kuşağı Sinema", "Aksiyon ve dram sinema seçkisi.", "Sinema"},
    });

    registerChannel(QStringLiteral("KanalD.tr"), QStringLiteral("Kanal D"), {
        {0, 45, "Kanal D Ana Haber", "Gündemden son dakika gelişmeleri ve özel haberler.", "Haber"},
        {45, 50, "Arka Sokaklar", "Polisiye aksiyon dizi kuşağı.", "Dizi"},
        {95, 55, "İnci Taneleri", "Yerli dram kuşağı yeni bölüm.", "Dizi"},
        {150, 60, "Kanal D Sinema", "Gecenin seçme sinema filmi.", "Sinema"},
    });

    registerChannel(QStringLiteral("NOW.tr"), QStringLiteral("NOW"), {
        {0, 45, "NOW Ana Haber", "Günün sıcak başlıkları ve analizler.", "Haber"},
        {45, 50, "En Hamarat Benim", "Yarışma ve yaşam kuşağı özel bölümü.", "Yaşam"},
        {95, 55, "Kızıl Goncalar", "Prime time yerli yapım bölümü.", "Dizi"},
        {150, 60, "NOW Gece Kuşağı", "Sinema ve kültür programı.", "Sinema"},
    });

    registerChannel(QStringLiteral("TV8.tr"), QStringLiteral("TV8"), {
        {0, 45, "MasterChef Türkiye", "Mutfakta kıyasıya lezzet ve şeflik rekabeti.", "Yarışma"},
        {45, 50, "Survivor Panorama", "Yarışma analizleri, stüdyo yorumları ve özetler.", "Yarışma"},
        {95, 55, "O Ses Türkiye Özel", "Müzik ve performans gecesi.", "Eğlence"},
        {150, 60, "TV8 Gece Yarışması", "Tekrar ve özet bölümü.", "Eğlence"},
    });

    registerChannel(QStringLiteral("Kanal7.tr"), QStringLiteral("Kanal 7"), {
        {0, 45, "Kanal 7 Haber Saati", "Yurt içi ve dünyadan güncel gelişmeler.", "Haber"},
        {45, 50, "Dünyayı Geziyorum", "Kültür, coğrafya ve gezi belgeseli.", "Gezi"},
        {95, 55, "Gelin", "Akşam kuşağı yerli aile draması.", "Dizi"},
        {150, 60, "Gündüz Gece", "Anadolu'nun renkleri ve kültür sohbetleri.", "Kültür"},
    });

    registerChannel(QStringLiteral("BeyazTV.tr"), QStringLiteral("Beyaz TV"), {
        {0, 45, "Beyaz Ana Haber", "Günün gelişmeleri ve sıcak dosyalar.", "Haber"},
        {45, 50, "Söylemezsem Olmaz", "Magazin, yaşam ve gündem değerlendirmeleri.", "Yaşam"},
        {95, 55, "Beyaz Futbol", "Haftanın maçları ve spor tartışma programı.", "Spor"},
        {150, 60, "Derin Futbol Özel", "Gece spor analizi.", "Spor"},
    });

    registerChannel(QStringLiteral("TV85.tr"), QStringLiteral("TV8.5"), {
        {0, 45, "Şampiyonlar Özeti", "Avrupa futbolundan günün öne çıkan karşılaşmaları.", "Spor"},
        {45, 50, "Yüzde Yüz Futbol", "Maç analizleri ve istatistik değerlendirmeleri.", "Spor"},
        {95, 55, "TV8.5 Sinema Kuşağı", "Aksiyon ve macera filmi.", "Sinema"},
        {150, 60, "Müzik ve Eğlence", "Gece kuşağı seçme programlar.", "Eğlence"},
    });

    // --- Haber Kanalları (11-20) ---
    registerChannel(QStringLiteral("TRTHaber.tr"), QStringLiteral("TRT Haber"), {
        {0, 40, "Canlı Haber Bülteni", "Türkiye ve dünyadan son dakika gelişmeleri.", "Haber"},
        {40, 45, "Ekonomi ve Teknoloji", "Piyasalar, teknoloji ve bilim gündemi.", "Ekonomi"},
        {85, 50, "Dünya Gündemi", "Uluslararası gelişmeler ve canlı bağlantılar.", "Haber"},
        {135, 60, "Gece Bakışı", "Günün kapsamlı özeti ve konuk analizleri.", "Tartışma"},
    });

    registerChannel(QStringLiteral("NTV.tr"), QStringLiteral("NTV"), {
        {0, 40, "NTV Canlı Haber", "Anlık haber akışı ve canlı saha bağlantıları.", "Haber"},
        {40, 45, "Piyasa Ekranı", "Borsa, döviz ve küresel ekonomi verileri.", "Ekonomi"},
        {85, 50, "Siyasi İşler", "Gündemin kritik başlıkları üzerine stüdyo analizi.", "Tartışma"},
        {135, 60, "NTV Gece Bülteni", "Dünya basını, spor ve hava durumu.", "Haber"},
    });

    registerChannel(QStringLiteral("CNNTurk.tr"), QStringLiteral("CNN Türk"), {
        {0, 40, "CNN Türk Gündem", "Son dakika gelişmeleri ve canlı yayın bağlantıları.", "Haber"},
        {40, 45, "Parametre Ekonomi", "Reel sektör, finans ve piyasa değerlendirmeleri.", "Ekonomi"},
        {85, 50, "Tarafsız Bölge", "Uzman konuklarla Türkiye ve dünya gündemi.", "Tartışma"},
        {135, 60, "Gece Görüşü", "Sıcak başlıkların derinlemesine incelemesi.", "Tartışma"},
    });

    registerChannel(QStringLiteral("Haberturk.tr"), QStringLiteral("Habertürk"), {
        {0, 40, "Habertürk Ana Bülten", "Günün öne çıkan haberleri ve canlı bağlantılar.", "Haber"},
        {40, 45, "Açık ve Net", "Ekonomi, siyaset ve dış politika dosyaları.", "Tartışma"},
        {85, 50, "Teke Tek Bilim", "Bilim, tarih ve teknoloji sohbetleri.", "Bilim"},
        {135, 60, "Gece Raporu", "Günün özeti ve yarının gündemi.", "Haber"},
    });

    registerChannel(QStringLiteral("HalkTV.tr"), QStringLiteral("Halk TV"), {
        {0, 40, "Halk Ana Haber", "Yurttan ve dünyadan günün sıcak gelişmeleri.", "Haber"},
        {40, 45, "Kayda Geçsin", "Araştırmacı gazetecilik ve gündem dosyaları.", "Tartışma"},
        {85, 50, "Sözüm Var", "Canlı stüdyo tartışma ve konuk yorumları.", "Tartışma"},
        {135, 60, "Halk TV Gece Bülteni", "Gece özeti ve yarının manşetleri.", "Haber"},
    });

    registerChannel(QStringLiteral("SozcuTV.tr"), QStringLiteral("Sözcü TV"), {
        {0, 40, "SZC Ana Haber", "Gündemden son dakika gelişmeleri ve canlı bağlantılar.", "Haber"},
        {40, 45, "Para Politika", "Ekonomi, enflasyon ve bütçe analizleri.", "Ekonomi"},
        {85, 50, "Arena", "Dosya haberler ve stüdyo tartışmaları.", "Tartışma"},
        {135, 60, "Gece Bülteni", "Günün kapsamlı haber özeti.", "Haber"},
    });

    registerChannel(QStringLiteral("AHaber.tr"), QStringLiteral("A Haber"), {
        {0, 40, "A Haber Canlı Yayın", "Son dakika haber akışı ve canlı bağlantılar.", "Haber"},
        {40, 45, "Memleket Meselesi", "Günün siyasi ve diplomatik gelişmeleri.", "Tartışma"},
        {85, 50, "Canlı Gündem", "Ekonomi, savunma sanayi ve dış politika.", "Haber"},
        {135, 60, "Gece Ajansı", "Günün özeti ve sıcak gelişmeler.", "Haber"},
    });

    registerChannel(QStringLiteral("Tele1.tr"), QStringLiteral("Tele1"), {
        {0, 40, "Tele1 Ana Haber", "Türkiye ve dünyadan güncel haber bülteni.", "Haber"},
        {40, 45, "18 Dakika", "Günün öne çıkan başlıklarının değerlendirmesi.", "Tartışma"},
        {85, 50, "Forum Hafta Sonu", "Kültür, ekonomi ve siyaset tartışmaları.", "Tartışma"},
        {135, 60, "Gece Bülteni", "Günün son gelişmeleri ve basın turu.", "Haber"},
    });

    registerChannel(QStringLiteral("HaberGlobal.tr"), QStringLiteral("Haber Global"), {
        {0, 40, "Haber Global Canlı", "Anlık haber bülteni ve küresel bağlantılar.", "Haber"},
        {40, 45, "Kayıt Altında", "Strateji, diplomasi ve güvenlik analizleri.", "Tartışma"},
        {85, 50, "Meseleler", "Konuklarla gündem değerlendirmesi.", "Tartışma"},
        {135, 60, "Gece Haberleri", "Dünya ve Türkiye'den son dakika başlıkları.", "Haber"},
    });

    registerChannel(QStringLiteral("TGRTHaber.tr"), QStringLiteral("TGRT Haber"), {
        {0, 40, "TGRT Ana Haber", "Günün sıcak başlıkları ve yurt bağlantıları.", "Haber"},
        {40, 45, "Taksim Meydanı", "Siyaset, ekonomi ve dış politika masası.", "Tartışma"},
        {85, 50, "Ekonomi Gündemi", "Piyasa analizleri ve uzman görüşleri.", "Ekonomi"},
        {135, 60, "Gece Bülteni", "Günün özeti ve spor haberleri.", "Haber"},
    });

    registerChannel(QStringLiteral("UlkeTV.tr"), QStringLiteral("Ülke TV"), {
        {0, 40, "Ülke Ana Haber", "Türkiye ve dünyadan güncel gelişmeler ve canlı bağlantılar.", "Haber"},
        {40, 45, "Sıradışı Tarih", "Tarih, kültür ve güncel analiz programı.", "Tartışma"},
        {85, 50, "En Sıradışı", "Gündemin öne çıkan başlıkları üzerine stüdyo sohbeti.", "Tartışma"},
        {135, 60, "Gece Bülteni", "Günün kapsamlı haber özeti.", "Haber"},
    });

    registerChannel(QStringLiteral("24TV.tr"), QStringLiteral("Kanal 24"), {
        {0, 40, "24 Ana Haber", "Son dakika gelişmeleri, ekonomi ve dış politika başlıkları.", "Haber"},
        {40, 45, "Günün Manşeti", "Medya ve siyaset gündeminin derinlemesine analizi.", "Tartışma"},
        {85, 50, "Kayıt Dışı", "Tarih, strateji ve küresel gelişmeler dosyası.", "Tartışma"},
        {135, 60, "24 Gece", "Gece haber bülteni ve basın özeti.", "Haber"},
    });

    // --- Spor (23-24) ---
    registerChannel(QStringLiteral("TRTSpor.tr"), QStringLiteral("TRT Spor"), {
        {0, 40, "Spor Gündemi", "Günün spor haberleri ve canlı bağlantılar.", "Spor"},
        {40, 50, "Maç Analizi", "Haftanın karşılaşmaları ve teknik değerlendirmeler.", "Spor"},
        {90, 55, "Stadyum Özel", "Süper Lig ve Avrupa kupaları özetleri.", "Spor"},
        {145, 60, "Gece Sporu", "Basketbol, voleybol ve transfer gündemi.", "Spor"},
    });

    registerChannel(QStringLiteral("TRTSpor2.tr"), QStringLiteral("TRT Spor Yıldız"), {
        {0, 45, "Olimpik Kuşak", "Voleybol, basketbol ve atletizm karşılaşmaları.", "Spor"},
        {45, 45, "Filede Heyecan", "Lig karşılaşmaları ve saha röportajları.", "Spor"},
        {90, 50, "Avrupa Potaları", "Basketbol özetleri ve teknik analiz.", "Spor"},
        {140, 60, "Motor Sporları Özeti", "Haftanın yarışlarından öne çıkan anlar.", "Spor"},
    });

    registerChannel(QStringLiteral("TRTBelgesel.tr"), QStringLiteral("TRT Belgesel"), {
        {0, 45, "Anadolu'nun Doğası", "Yaban hayatı ve coğrafya belgeseli.", "Belgesel"},
        {45, 50, "Bilim ve Keşif", "Mühendislik, doğa bilimleri ve tarih dosyaları.", "Belgesel"},
        {95, 45, "Tarihin İzinde", "Arkeoloji ve medeniyetler tarihi.", "Tarih"},
        {140, 60, "Ailenin Yeni Üyesi", "Dünya kültürleri ve insan öyküleri.", "Belgesel"},
    });

    registerChannel(QStringLiteral("DMAX.tr"), QStringLiteral("DMAX"), {
        {0, 45, "Tamirat Tadilat", "Klasik otomobil restorasyonu ve atölye serüveni.", "Belgesel"},
        {45, 50, "Altın Peşinde", "Zorlu doğa koşullarında madencilik ve ekip mücadelesi.", "Belgesel"},
        {95, 45, "Nasıl Yapılmış?", "Modern endüstri ve mühendislik üretim süreçleri.", "Bilim"},
        {140, 60, "Doğada Tek Başına", "Vahşi doğada hayatta kalma belgeseli.", "Belgesel"},
    });

    registerChannel(QStringLiteral("TRTCocuk.tr"), QStringLiteral("TRT Çocuk"), {
        {0, 40, "Rafadan Tayfa", "Mahalle dostluğu ve eğlenceli maceralar.", "Çocuk"},
        {40, 45, "Maysa ve Bulut", "Anadolu kültürü ve doğa sevgisi hikâyeleri.", "Çocuk"},
        {85, 45, "Bilge Çocuk Atölyesi", "Bilim, merak ve eğitici animasyon kuşağı.", "Çocuk"},
        {130, 50, "İbi ve Tosi", "Matematik ve keşif dolu macera.", "Çocuk"},
    });

    registerChannel(QStringLiteral("CartoonNetwork.tr"), QStringLiteral("Cartoon Network Türkiye"), {
        {0, 40, "Gumball'ın Muhteşem Dünyası", "Elmore kasabasından eğlenceli aile maceraları.", "Çocuk"},
        {40, 45, "Kral Şakir", "Şakir ve ailesinin neşeli İstanbul serüvenleri.", "Çocuk"},
        {85, 45, "Kafadar Ayılar", "Üç sevimli ayının şehirdeki günlük maceraları.", "Çocuk"},
        {130, 50, "Teen Titans Go!", "Genç kahramanların komik takım hikâyeleri.", "Çocuk"},
    });

    registerChannel(QStringLiteral("MinikaGO.tr"), QStringLiteral("MinikaGO"), {
        {0, 40, "Beyblade Burst", "Heyecan dolu turnuva ve dostluk mücadelesi.", "Çocuk"},
        {40, 45, "Maceracı Yüzgeçler", "Boğaz'ın derinliklerinde keşif ve eğlence.", "Çocuk"},
        {85, 45, "Dedektif Çetesi", "Gizemleri çözen genç arkadaşların serüveni.", "Çocuk"},
        {130, 50, "Süper Kanatlar Özel", "Dünyayı gezen uçak kahramanların görevleri.", "Çocuk"},
    });

    registerChannel(QStringLiteral("MinikaCocuk.tr"), QStringLiteral("MinikaÇOCUK"), {
        {0, 40, "Neşeli Dünyam", "Okul öncesi eğitici oyun ve şarkı kuşağı.", "Çocuk"},
        {40, 45, "Masha ve Koca Ayı", "Ormanda eğlenceli dostluk hikâyeleri.", "Çocuk"},
        {85, 45, "Pırıl ve Arkadaşları", "Sayılar, renkler ve keşif dolu maceralar.", "Çocuk"},
        {130, 50, "Küçük Kaşifler", "Hayvanlar ve doğa hakkında eğlenceli bilgiler.", "Çocuk"},
    });

    registerChannel(QStringLiteral("TRT2.tr"), QStringLiteral("TRT 2"), {
        {0, 45, "Kültür Sanat Gündemi", "Sergiler, tiyatro ve edebiyat dünyasından haberler.", "Kültür"},
        {45, 50, "Film Önü ve Sinema", "Dünya sinemasından seçkin yapım ve eleştiri.", "Sinema"},
        {95, 55, "Tarih Söyleşileri", "Medeniyet, mimari ve sanat tarihi üzerine sohbet.", "Kültür"},
        {150, 60, "Klasik Müzik Konseri", "Senfoni orkestrası seçme kayıtları.", "Müzik"},
    });
}

void EpgManager::setLoading(bool loading)
{
    if (m_loading != loading) {
        m_loading = loading;
        emit loadingChanged();
    }
}

void EpgManager::setStatusMessage(const QString &msg)
{
    if (m_statusMessage != msg) {
        m_statusMessage = msg;
        emit statusMessageChanged();
    }
}

} // namespace zethropol::tv
