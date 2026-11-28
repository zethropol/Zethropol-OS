#pragma once

#include <QByteArray>
#include <QDate>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace zethropol::weather {

struct WeatherCodeInfo
{
    QString descriptionTr;
    QString shortDescriptionTr;
    QString iconName;
    QString mood;
};

struct WeatherSnapshot
{
    bool valid = false;
    QString locationName;
    QString locationAdmin;
    QString countryCode;
    double latitude = 41.0082;
    double longitude = 28.9784;

    double temperatureC = 0.0;
    double apparentTemperatureC = 0.0;
    int humidityPercent = 0;
    double windSpeedKmh = 0.0;
    int windDirectionDeg = 0;
    double pressureHpa = 0.0;
    int weatherCode = 0;
    bool isDay = true;

    QString observedIsoTime;
    QString updatedDisplayTime;
    QString sourceLabel;

    QVariantList hourlyForecast; // Up to 24 entries
    QVariantList dailyForecast;  // Up to 7 entries
};

struct MgmObservation
{
    bool valid = false;
    bool hasTemperature = false;
    double temperatureC = 0.0;
    bool hasHumidity = false;
    int humidityPercent = 0;
    bool hasWindSpeed = false;
    double windSpeedKmh = 0.0;
    bool hasWindDirection = false;
    int windDirectionDeg = 0;
    bool hasPressure = false;
    double pressureHpa = 0.0;
    int mappedWmoCode = -1;
    QString stationLabel;
    QString observedTime;
};

struct WeatherModels
{
    static constexpr int kMinOpacityPercent = 15;
    static constexpr int kMaxOpacityPercent = 100;
    static constexpr int kDefaultDesktopOpacityPercent = 82;
    static constexpr int kDefaultPopupOpacityPercent = 92;
    static constexpr int kDefaultForecastDays = 5;
    static constexpr qint64 kMaxResponseBytes = 512 * 1024; // 512 KiB strict cap

    /// Clamps integer opacity percentage strictly to [15, 100].
    static int clampOpacityPercent(int percent);

    /// Converts integer opacity percentage to clamped [0.15, 1.00] factor.
    static double opacityFactorFromPercent(int percent);

    /// Normalizes forecast day selection strictly to 3, 5, or 7 (defaults to 5).
    static int normalizeForecastDays(int days);

    /// Slices a 7-day forecast list to the requested 3 / 5 / 7 day window.
    static QVariantList sliceDailyForecast(const QVariantList &daily, int days);

    /// Maps WMO weather interpretation code (0-99) to Turkish labels, KDE icon name, and gradient mood.
    static WeatherCodeInfo describeWmoCode(int code, bool isDay);

    /// Maps MGM `hadiseKodu` string to an equivalent WMO code (-1 if unknown).
    static int mgmEventCodeToWmo(const QString &hadiseKodu);

    /// Formats 16-point Turkish wind compass direction (K, KKD, KD, ..., KKB).
    static QString windCompassTr(int degrees);

    /// Formats Turkish abbreviated weekday (Pzt, Sal, Çar, Per, Cum, Cmt, Paz).
    static QString turkishShortWeekday(const QDate &date);

    /// Validates that an outbound URL is strictly HTTPS (or loopback HTTP strictly in unit tests).
    static bool isAllowedWeatherUrl(const QUrl &url, bool allowLoopbackHttpForTests = false);

    /// Normalizes Turkish province name for MGM `merkezler?il=` query.
    static QString normalizeTurkishProvinceQuery(const QString &raw);

    /// Parses Open-Meteo geocoding JSON, prioritizing Turkey (`TR`) and higher population cities/districts.
    static QVariantList parseOpenMeteoGeocoding(const QByteArray &rawJson);

    /// Parses Open-Meteo `forecast` JSON (current + next 24h hourly + 7-day daily).
    static WeatherSnapshot parseOpenMeteoForecast(const QByteArray &rawJson,
                                                  const QString &locationName,
                                                  const QString &locationAdmin,
                                                  const QString &countryCode,
                                                  double lat,
                                                  double lon,
                                                  QString *errorOut = nullptr);

    /// Selects the closest MGM `merkezId` from `servis.mgm.gov.tr/web/merkezler` JSON.
    static QString selectMgmCenterId(const QByteArray &rawJson,
                                     double targetLat,
                                     double targetLon,
                                     const QString &districtHint);

    /// Parses MGM `servis.mgm.gov.tr/web/sondurumlar` JSON while filtering `-9999` sentinels.
    static MgmObservation parseMgmObservation(const QByteArray &rawJson);

    /// Serializes / deserializes WeatherSnapshot for atomic local offline cache persistence.
    static QByteArray snapshotToCacheJson(const WeatherSnapshot &snapshot);
    static WeatherSnapshot snapshotFromCacheJson(const QByteArray &rawJson);
};

} // namespace zethropol::weather
