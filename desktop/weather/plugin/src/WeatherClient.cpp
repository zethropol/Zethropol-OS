#include "WeatherClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>

#include <cmath>

namespace zethropol::weather {

namespace {

constexpr const char *kDefaultOpenMeteoGeocodingBase = "https://geocoding-api.open-meteo.com/v1/search";
constexpr const char *kDefaultOpenMeteoForecastBase = "https://api.open-meteo.com/v1/forecast";
constexpr const char *kDefaultMgmBase = "https://servis.mgm.gov.tr/web";
constexpr const char *kUserAgent = "Zethropol-Weather/0.1 (+https://github.com/zethropol/Zethropol-OS)";

} // namespace

WeatherClient::WeatherClient(QObject *parent)
    : QObject(parent)
{}

WeatherClient::~WeatherClient()
{
    abortActiveWeatherReplies();
    if (QNetworkReply *reply = m_geoReply.data()) {
        m_geoReply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void WeatherClient::componentComplete()
{
    // Startup auto-fetch: queue immediately on the next event-loop turn so that property bindings
    // (location, forecastDays, MGM toggle) assigned by the QML engine are applied first.
    m_initialRefreshRequested = true;
    QTimer::singleShot(0, this, [this]() {
        if (m_requestSeq == 0) {
            refresh();
        }
    });
}

void WeatherClient::setLocationName(const QString &name)
{
    const QString cleaned = name.trimmed();
    if (!cleaned.isEmpty() && m_locationName != cleaned) {
        m_locationName = cleaned;
        emit locationChanged();
    }
}

void WeatherClient::setLocationAdmin(const QString &admin)
{
    const QString cleaned = admin.trimmed();
    if (m_locationAdmin != cleaned) {
        m_locationAdmin = cleaned;
        emit locationChanged();
    }
}

void WeatherClient::setCountryCode(const QString &code)
{
    const QString cleaned = code.trimmed().toUpper();
    if (m_countryCode != cleaned) {
        m_countryCode = cleaned;
        emit locationChanged();
    }
}

void WeatherClient::setLatitude(double lat)
{
    if (std::isfinite(lat) && lat >= -90.0 && lat <= 90.0 && std::abs(m_latitude - lat) > 1e-6) {
        m_latitude = lat;
        emit locationChanged();
    }
}

void WeatherClient::setLongitude(double lon)
{
    if (std::isfinite(lon) && lon >= -180.0 && lon <= 180.0 && std::abs(m_longitude - lon) > 1e-6) {
        m_longitude = lon;
        emit locationChanged();
    }
}

void WeatherClient::setForecastDays(int days)
{
    const int normalized = WeatherModels::normalizeForecastDays(days);
    if (m_forecastDays != normalized) {
        m_forecastDays = normalized;
        emit forecastDaysChanged();
    }
}

void WeatherClient::setUseMgmObservation(bool enabled)
{
    if (m_useMgmObservation != enabled) {
        m_useMgmObservation = enabled;
        emit settingsChanged();
    }
}

QString WeatherClient::conditionText() const
{
    if (!m_snapshot.valid) {
        return QStringLiteral("Veri bekleniyor");
    }
    return WeatherModels::describeWmoCode(m_snapshot.weatherCode, m_snapshot.isDay).descriptionTr;
}

QString WeatherClient::conditionIcon() const
{
    if (!m_snapshot.valid) {
        return QStringLiteral("weather-none-available");
    }
    return WeatherModels::describeWmoCode(m_snapshot.weatherCode, m_snapshot.isDay).iconName;
}

QString WeatherClient::themeMood() const
{
    if (!m_snapshot.valid) {
        return QStringLiteral("cloudy");
    }
    return WeatherModels::describeWmoCode(m_snapshot.weatherCode, m_snapshot.isDay).mood;
}

QVariantList WeatherClient::visibleDailyForecast() const
{
    return WeatherModels::sliceDailyForecast(m_snapshot.dailyForecast, m_forecastDays);
}

int WeatherClient::clampOpacityPercent(int percent)
{
    return WeatherModels::clampOpacityPercent(percent);
}

int WeatherClient::normalizeForecastDays(int days)
{
    return WeatherModels::normalizeForecastDays(days);
}

void WeatherClient::setCacheDirectoryOverride(const QString &dirPath)
{
    m_cacheDirOverride = dirPath;
}

void WeatherClient::setEndpointOverridesForTest(const QString &geocodingBase,
                                                const QString &forecastBase,
                                                const QString &mgmBase)
{
    m_geocodingBaseOverride = geocodingBase;
    m_forecastBaseOverride = forecastBase;
    m_mgmBaseOverride = mgmBase;
}

QString WeatherClient::cacheFilePath() const
{
    const QString base = !m_cacheDirOverride.isEmpty()
        ? m_cacheDirOverride
        : (QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
           + QStringLiteral("/zethropol/weather"));
    return base + QStringLiteral("/cache.json");
}

void WeatherClient::configureLocation(const QString &name,
                                      const QString &admin,
                                      const QString &countryCode,
                                      double latitude,
                                      double longitude)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude)
        || latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
        return;
    }
    m_locationName = name.trimmed().isEmpty() ? QStringLiteral("İstanbul") : name.trimmed();
    m_locationAdmin = admin.trimmed();
    m_countryCode = countryCode.trimmed().toUpper();
    m_latitude = latitude;
    m_longitude = longitude;
    emit locationChanged();
}

void WeatherClient::abortActiveWeatherReplies()
{
    if (QNetworkReply *reply = m_forecastReply.data()) {
        m_forecastReply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (QNetworkReply *reply = m_mgmReply.data()) {
        m_mgmReply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}

void WeatherClient::refresh()
{
    abortActiveWeatherReplies();
    const quint64 seq = ++m_requestSeq;
    setLoading(true);
    setErrorMessage(QString());

    const bool allowLoopback = !m_forecastBaseOverride.isEmpty();
    const QString baseStr = allowLoopback
        ? m_forecastBaseOverride
        : QString::fromLatin1(kDefaultOpenMeteoForecastBase);

    QUrl url(baseStr);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("latitude"), QString::number(m_latitude, 'f', 4));
    query.addQueryItem(QStringLiteral("longitude"), QString::number(m_longitude, 'f', 4));
    query.addQueryItem(QStringLiteral("current"),
                       QStringLiteral("temperature_2m,relative_humidity_2m,apparent_temperature,"
                                      "is_day,weather_code,surface_pressure,wind_speed_10m,wind_direction_10m"));
    query.addQueryItem(QStringLiteral("hourly"),
                       QStringLiteral("temperature_2m,precipitation_probability,weather_code,is_day"));
    query.addQueryItem(QStringLiteral("daily"),
                       QStringLiteral("weather_code,temperature_2m_max,temperature_2m_min,"
                                      "precipitation_probability_max"));
    query.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("7"));
    query.addQueryItem(QStringLiteral("timezone"), QStringLiteral("auto"));
    url.setQuery(query);

    if (!WeatherModels::isAllowedWeatherUrl(url, allowLoopback)) {
        handleFetchFailure(QStringLiteral("Güvenlik engeli: Geçersiz hava durumu uç noktası."));
        return;
    }

    QNetworkRequest req(url);
    req.setTransferTimeout(kNetworkTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_network.get(req);
    m_forecastReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, seq]() {
        reply->deleteLater();
        if (m_forecastReply != reply || m_requestSeq != seq) {
            return;
        }
        m_forecastReply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            handleFetchFailure(QStringLiteral("Bağlantı hatası: %1").arg(reply->errorString()));
            return;
        }

        const QByteArray payload = reply->read(WeatherModels::kMaxResponseBytes + 1);
        if (payload.size() > WeatherModels::kMaxResponseBytes) {
            handleFetchFailure(QStringLiteral("Hava durumu yanıtı güvenlik boyut sınırını aştı."));
            return;
        }

        QString parseErr;
        WeatherSnapshot snap = WeatherModels::parseOpenMeteoForecast(
            payload, m_locationName, m_locationAdmin, m_countryCode, m_latitude, m_longitude, &parseErr);
        if (!snap.valid) {
            handleFetchFailure(QStringLiteral("Veri hatası: %1")
                                   .arg(parseErr.isEmpty() ? QStringLiteral("Hava durumu verisi çözümlenemedi.")
                                                           : parseErr));
            return;
        }

        applySnapshot(snap, false);
        saveSnapshotToCache(snap);

        // Optional MGM live observation enrichment for Turkey (`TR`) locations
        if (m_useMgmObservation && m_countryCode == QLatin1String("TR")) {
            startMgmLookup(seq);
        } else {
            setLoading(false);
            emit refreshCompleted(true, false);
        }
    });
}

void WeatherClient::startMgmLookup(quint64 reqSeq)
{
    const bool allowLoopback = !m_mgmBaseOverride.isEmpty();
    const QString base = allowLoopback ? m_mgmBaseOverride : QString::fromLatin1(kDefaultMgmBase);

    QString province = WeatherModels::normalizeTurkishProvinceQuery(m_locationAdmin);
    if (province.isEmpty() || province.compare(QStringLiteral("Türkiye"), Qt::CaseInsensitive) == 0) {
        province = WeatherModels::normalizeTurkishProvinceQuery(m_locationName);
    }

    QUrl url(base + QStringLiteral("/merkezler"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("il"), province);
    url.setQuery(q);

    if (!WeatherModels::isAllowedWeatherUrl(url, allowLoopback)) {
        setLoading(false);
        emit refreshCompleted(true, false);
        return;
    }

    QNetworkRequest req(url);
    req.setTransferTimeout(6000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Origin", "https://www.mgm.gov.tr");
    req.setRawHeader("Referer", "https://www.mgm.gov.tr/");
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_network.get(req);
    m_mgmReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, reqSeq]() {
        reply->deleteLater();
        if (m_mgmReply != reply || m_requestSeq != reqSeq) {
            return;
        }
        m_mgmReply = nullptr;

        if (reply->error() != QNetworkReply::NoError) {
            // MGM unavailable -> keep already-verified Open-Meteo data cleanly
            setLoading(false);
            emit refreshCompleted(true, false);
            return;
        }
        const QByteArray payload = reply->read(WeatherModels::kMaxResponseBytes + 1);
        if (payload.size() > WeatherModels::kMaxResponseBytes) {
            setLoading(false);
            emit refreshCompleted(true, false);
            return;
        }
        const QString merkezId = WeatherModels::selectMgmCenterId(
            payload, m_latitude, m_longitude, m_locationName);
        if (merkezId.isEmpty()) {
            setLoading(false);
            emit refreshCompleted(true, false);
            return;
        }
        startMgmObservationFetch(merkezId, reqSeq);
    });
}

void WeatherClient::startMgmObservationFetch(const QString &merkezId, quint64 reqSeq)
{
    const bool allowLoopback = !m_mgmBaseOverride.isEmpty();
    const QString base = allowLoopback ? m_mgmBaseOverride : QString::fromLatin1(kDefaultMgmBase);

    QUrl url(base + QStringLiteral("/sondurumlar"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("merkezid"), merkezId);
    url.setQuery(q);

    if (!WeatherModels::isAllowedWeatherUrl(url, allowLoopback)) {
        setLoading(false);
        emit refreshCompleted(true, false);
        return;
    }

    QNetworkRequest req(url);
    req.setTransferTimeout(6000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Origin", "https://www.mgm.gov.tr");
    req.setRawHeader("Referer", "https://www.mgm.gov.tr/");
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_network.get(req);
    m_mgmReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, reqSeq]() {
        reply->deleteLater();
        if (m_mgmReply != reply || m_requestSeq != reqSeq) {
            return;
        }
        m_mgmReply = nullptr;

        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray payload = reply->read(WeatherModels::kMaxResponseBytes + 1);
            if (payload.size() <= WeatherModels::kMaxResponseBytes) {
                const MgmObservation obs = WeatherModels::parseMgmObservation(payload);
                if (obs.valid && m_snapshot.valid) {
                    if (obs.hasTemperature) {
                        m_snapshot.temperatureC = obs.temperatureC;
                    }
                    if (obs.hasHumidity) {
                        m_snapshot.humidityPercent = obs.humidityPercent;
                    }
                    if (obs.hasWindSpeed) {
                        m_snapshot.windSpeedKmh = obs.windSpeedKmh;
                    }
                    if (obs.hasWindDirection) {
                        m_snapshot.windDirectionDeg = obs.windDirectionDeg;
                    }
                    if (obs.hasPressure) {
                        m_snapshot.pressureHpa = obs.pressureHpa;
                    }
                    if (obs.mappedWmoCode >= 0) {
                        m_snapshot.weatherCode = obs.mappedWmoCode;
                    }
                    m_snapshot.sourceLabel = QStringLiteral("Open-Meteo + MGM");
                    saveSnapshotToCache(m_snapshot);
                    emit dataChanged();
                }
            }
        }
        setLoading(false);
        emit refreshCompleted(true, false);
    });
}

void WeatherClient::searchLocations(const QString &queryText)
{
    const QString cleaned = queryText.trimmed();
    if (cleaned.size() < 2) {
        m_searchResults.clear();
        m_searchError = cleaned.isEmpty()
            ? QString()
            : QStringLiteral("Arama için en az 2 karakter girin.");
        m_searching = false;
        emit searchChanged();
        return;
    }

    if (QNetworkReply *reply = m_geoReply.data()) {
        m_geoReply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }

    const bool allowLoopback = !m_geocodingBaseOverride.isEmpty();
    const QString baseStr = allowLoopback
        ? m_geocodingBaseOverride
        : QString::fromLatin1(kDefaultOpenMeteoGeocodingBase);

    QUrl url(baseStr);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), cleaned);
    q.addQueryItem(QStringLiteral("count"), QStringLiteral("15"));
    q.addQueryItem(QStringLiteral("language"), QStringLiteral("tr"));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(q);

    if (!WeatherModels::isAllowedWeatherUrl(url, allowLoopback)) {
        m_searchResults.clear();
        m_searchError = QStringLiteral("Güvenlik engeli: Geçersiz konum arama adresi.");
        m_searching = false;
        emit searchChanged();
        return;
    }

    m_searching = true;
    m_searchError.clear();
    emit searchChanged();

    QNetworkRequest req(url);
    req.setTransferTimeout(kNetworkTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    req.setRawHeader("Accept", "application/json");

    QNetworkReply *reply = m_network.get(req);
    m_geoReply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_geoReply != reply) {
            return;
        }
        m_geoReply = nullptr;
        m_searching = false;

        if (reply->error() != QNetworkReply::NoError) {
            m_searchResults.clear();
            m_searchError = QStringLiteral("Konum araması başarısız: %1").arg(reply->errorString());
            emit searchChanged();
            return;
        }

        const QByteArray payload = reply->read(WeatherModels::kMaxResponseBytes + 1);
        if (payload.size() > WeatherModels::kMaxResponseBytes) {
            m_searchResults.clear();
            m_searchError = QStringLiteral("Konum yanıtı boyut sınırını aştı.");
            emit searchChanged();
            return;
        }

        m_searchResults = WeatherModels::parseOpenMeteoGeocoding(payload);
        if (m_searchResults.isEmpty()) {
            m_searchError = QStringLiteral("Eşleşen il veya ilçe bulunamadı.");
        } else {
            m_searchError.clear();
        }
        emit searchChanged();
    });
}

void WeatherClient::clearSearchResults()
{
    if (!m_searchResults.isEmpty() || !m_searchError.isEmpty() || m_searching) {
        if (QNetworkReply *reply = m_geoReply.data()) {
            m_geoReply = nullptr;
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
        m_searching = false;
        m_searchResults.clear();
        m_searchError.clear();
        emit searchChanged();
    }
}

bool WeatherClient::loadCachedSnapshot()
{
    QFile f(cacheFilePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = f.read(WeatherModels::kMaxResponseBytes + 1);
    if (bytes.size() > WeatherModels::kMaxResponseBytes) {
        return false;
    }
    const WeatherSnapshot snap = WeatherModels::snapshotFromCacheJson(bytes);
    if (!snap.valid) {
        return false;
    }
    applySnapshot(snap, true);
    return true;
}

bool WeatherClient::saveSnapshotToCache(const WeatherSnapshot &snap) const
{
    if (!snap.valid) {
        return false;
    }
    const QString path = cacheFilePath();
    const QFileInfo fi(path);
    QDir dir(fi.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return false;
    }
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) {
        return false;
    }
    save.write(WeatherModels::snapshotToCacheJson(snap));
    return save.commit();
}

void WeatherClient::applySnapshot(const WeatherSnapshot &snap, bool fromOfflineCache)
{
    m_snapshot = snap;
    m_offlineCache = fromOfflineCache;
    emit dataChanged();
    emit forecastDaysChanged();
}

void WeatherClient::handleFetchFailure(const QString &reason)
{
    setErrorMessage(reason);
    const bool hadCached = m_snapshot.valid || loadCachedSnapshot();
    if (hadCached) {
        m_offlineCache = true;
        emit dataChanged();
    }
    setLoading(false);
    emit refreshCompleted(false, hadCached);
}

void WeatherClient::setLoading(bool loading)
{
    if (m_loading != loading) {
        m_loading = loading;
        emit loadingChanged();
    }
}

void WeatherClient::setErrorMessage(const QString &err)
{
    if (m_errorMessage != err) {
        m_errorMessage = err;
        emit errorChanged();
    }
}

} // namespace zethropol::weather
