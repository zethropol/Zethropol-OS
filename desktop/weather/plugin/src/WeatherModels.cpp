#include "WeatherModels.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace zethropol::weather {

namespace {

bool isMgmValidNumber(const QJsonValue &val, double *out = nullptr)
{
    if (!val.isDouble()) {
        return false;
    }
    const double d = val.toDouble();
    if (!std::isfinite(d) || d <= -999.0) {
        return false;
    }
    if (out) {
        *out = d;
    }
    return true;
}

double approxDistSq(double lat1, double lon1, double lat2, double lon2)
{
    const double dLat = lat1 - lat2;
    const double dLon = lon1 - lon2;
    return dLat * dLat + dLon * dLon;
}

QString formatDisplayHour(const QString &isoTime)
{
    const int tIdx = isoTime.indexOf(QLatin1Char('T'));
    if (tIdx >= 0 && isoTime.size() >= tIdx + 6) {
        return isoTime.mid(tIdx + 1, 5);
    }
    return isoTime;
}

QString sanitizePlainText(QString text, int maxLen)
{
    QString out;
    out.reserve(std::min<qsizetype>(text.size(), maxLen));
    for (const QChar ch : text) {
        if (ch.isNull() || ch.category() == QChar::Other_Control) {
            continue;
        }
        out.append(ch);
        if (out.size() >= maxLen) {
            break;
        }
    }
    return out.trimmed();
}

} // namespace

int WeatherModels::clampOpacityPercent(int percent)
{
    return std::clamp(percent, kMinOpacityPercent, kMaxOpacityPercent);
}

double WeatherModels::opacityFactorFromPercent(int percent)
{
    return static_cast<double>(clampOpacityPercent(percent)) / 100.0;
}

int WeatherModels::normalizeForecastDays(int days)
{
    if (days == 3 || days == 5 || days == 7) {
        return days;
    }
    return kDefaultForecastDays;
}

QVariantList WeatherModels::sliceDailyForecast(const QVariantList &daily, int days)
{
    const int count = normalizeForecastDays(days);
    if (daily.size() <= count) {
        return daily;
    }
    return daily.mid(0, count);
}

WeatherCodeInfo WeatherModels::describeWmoCode(int code, bool isDay)
{
    switch (code) {
    case 0:
        return {
            QStringLiteral("Açık"),
            QStringLiteral("Açık"),
            isDay ? QStringLiteral("weather-clear") : QStringLiteral("weather-clear-night"),
            isDay ? QStringLiteral("clear-day") : QStringLiteral("clear-night"),
        };
    case 1:
        return {
            QStringLiteral("Az Bulutlu"),
            QStringLiteral("Az Bulutlu"),
            isDay ? QStringLiteral("weather-few-clouds") : QStringLiteral("weather-few-clouds-night"),
            isDay ? QStringLiteral("clear-day") : QStringLiteral("clear-night"),
        };
    case 2:
        return {
            QStringLiteral("Parçalı Bulutlu"),
            QStringLiteral("Parçalı"),
            isDay ? QStringLiteral("weather-clouds") : QStringLiteral("weather-clouds-night"),
            QStringLiteral("cloudy"),
        };
    case 3:
        return {
            QStringLiteral("Çok Bulutlu"),
            QStringLiteral("Bulutlu"),
            QStringLiteral("weather-many-clouds"),
            QStringLiteral("cloudy"),
        };
    case 45:
        return {
            QStringLiteral("Sisli"),
            QStringLiteral("Sisli"),
            QStringLiteral("weather-fog"),
            QStringLiteral("fog"),
        };
    case 48:
        return {
            QStringLiteral("Kırağılı Sis"),
            QStringLiteral("Kırağılı Sis"),
            QStringLiteral("weather-fog"),
            QStringLiteral("fog"),
        };
    case 51:
        return {
            QStringLiteral("Hafif Çiseleme"),
            QStringLiteral("Çiseleme"),
            QStringLiteral("weather-showers-scattered"),
            QStringLiteral("rain"),
        };
    case 53:
        return {
            QStringLiteral("Orta Çiseleme"),
            QStringLiteral("Çiseleme"),
            QStringLiteral("weather-showers-scattered"),
            QStringLiteral("rain"),
        };
    case 55:
        return {
            QStringLiteral("Yoğun Çiseleme"),
            QStringLiteral("Çiseleme"),
            QStringLiteral("weather-showers"),
            QStringLiteral("rain"),
        };
    case 56:
    case 57:
        return {
            QStringLiteral("Dondurucu Çiseleme"),
            QStringLiteral("Dondurucu"),
            QStringLiteral("weather-freezing-rain"),
            QStringLiteral("snow"),
        };
    case 61:
        return {
            QStringLiteral("Hafif Yağmurlu"),
            QStringLiteral("Hafif Yağmur"),
            QStringLiteral("weather-showers-scattered"),
            QStringLiteral("rain"),
        };
    case 63:
        return {
            QStringLiteral("Yağmurlu"),
            QStringLiteral("Yağmurlu"),
            QStringLiteral("weather-showers"),
            QStringLiteral("rain"),
        };
    case 65:
        return {
            QStringLiteral("Şiddetli Yağmur"),
            QStringLiteral("Kuvvetli Yağmur"),
            QStringLiteral("weather-showers"),
            QStringLiteral("rain"),
        };
    case 66:
    case 67:
        return {
            QStringLiteral("Dondurucu Yağmur"),
            QStringLiteral("Dondurucu"),
            QStringLiteral("weather-freezing-rain"),
            QStringLiteral("snow"),
        };
    case 71:
        return {
            QStringLiteral("Hafif Kar Yağışlı"),
            QStringLiteral("Hafif Kar"),
            QStringLiteral("weather-snow-scattered"),
            QStringLiteral("snow"),
        };
    case 73:
        return {
            QStringLiteral("Kar Yağışlı"),
            QStringLiteral("Kar Yağışlı"),
            QStringLiteral("weather-snow"),
            QStringLiteral("snow"),
        };
    case 75:
        return {
            QStringLiteral("Yoğun Kar Yağışlı"),
            QStringLiteral("Yoğun Kar"),
            QStringLiteral("weather-snow"),
            QStringLiteral("snow"),
        };
    case 77:
        return {
            QStringLiteral("Kar Taneleri"),
            QStringLiteral("Kar"),
            QStringLiteral("weather-snow-scattered"),
            QStringLiteral("snow"),
        };
    case 80:
        return {
            QStringLiteral("Hafif Sağanak"),
            QStringLiteral("Sağanak"),
            isDay ? QStringLiteral("weather-showers-scattered-day")
                  : QStringLiteral("weather-showers-scattered-night"),
            QStringLiteral("rain"),
        };
    case 81:
        return {
            QStringLiteral("Sağanak Yağışlı"),
            QStringLiteral("Sağanak"),
            isDay ? QStringLiteral("weather-showers-day") : QStringLiteral("weather-showers-night"),
            QStringLiteral("rain"),
        };
    case 82:
        return {
            QStringLiteral("Kuvvetli Sağanak"),
            QStringLiteral("Şiddetli Sağanak"),
            QStringLiteral("weather-showers"),
            QStringLiteral("storm"),
        };
    case 85:
    case 86:
        return {
            QStringLiteral("Kar Sağanağı"),
            QStringLiteral("Kar Sağanağı"),
            QStringLiteral("weather-snow"),
            QStringLiteral("snow"),
        };
    case 95:
        return {
            QStringLiteral("Gök Gürültülü Fırtına"),
            QStringLiteral("Fırtına"),
            isDay ? QStringLiteral("weather-storm-day") : QStringLiteral("weather-storm-night"),
            QStringLiteral("storm"),
        };
    case 96:
    case 99:
        return {
            QStringLiteral("Dolu ve Fırtına"),
            QStringLiteral("Dolu/Fırtına"),
            QStringLiteral("weather-storm"),
            QStringLiteral("storm"),
        };
    default:
        return {
            QStringLiteral("Değişken Bulutlu"),
            QStringLiteral("Değişken"),
            QStringLiteral("weather-clouds"),
            QStringLiteral("cloudy"),
        };
    }
}

int WeatherModels::mgmEventCodeToWmo(const QString &hadiseKodu)
{
    const QString k = hadiseKodu.trimmed().toUpper();
    if (k == QLatin1String("A")) return 0;
    if (k == QLatin1String("AB")) return 1;
    if (k == QLatin1String("PB")) return 2;
    if (k == QLatin1String("CB")) return 3;
    if (k == QLatin1String("SIS") || k == QLatin1String("PUS") || k == QLatin1String("DMN")) return 45;
    if (k == QLatin1String("HY")) return 61;
    if (k == QLatin1String("Y")) return 63;
    if (k == QLatin1String("KY")) return 65;
    if (k == QLatin1String("HSY")) return 80;
    if (k == QLatin1String("SY")) return 81;
    if (k == QLatin1String("KSY")) return 82;
    if (k == QLatin1String("GSY") || k == QLatin1String("KGY")) return 95;
    if (k == QLatin1String("HKY")) return 71;
    if (k == QLatin1String("K")) return 73;
    if (k == QLatin1String("YKY")) return 75;
    if (k == QLatin1String("KKY")) return 66;
    if (k == QLatin1String("MSY")) return 80;
    return -1;
}

QString WeatherModels::windCompassTr(int degrees)
{
    static const char *kDirections[16] = {
        "K", "KKD", "KD", "DKD", "D", "DGD", "GD", "GGD",
        "G", "GGB", "GB", "BGB", "B", "BKB", "KB", "KKB",
    };
    const int normalized = ((degrees % 360) + 360) % 360;
    const int idx = static_cast<int>(std::floor((normalized + 11.25) / 22.5)) % 16;
    return QString::fromLatin1(kDirections[idx]);
}

QString WeatherModels::turkishShortWeekday(const QDate &date)
{
    switch (date.dayOfWeek()) {
    case 1: return QStringLiteral("Pzt");
    case 2: return QStringLiteral("Sal");
    case 3: return QStringLiteral("Çar");
    case 4: return QStringLiteral("Per");
    case 5: return QStringLiteral("Cum");
    case 6: return QStringLiteral("Cmt");
    case 7: return QStringLiteral("Paz");
    default: return QStringLiteral("—");
    }
}

bool WeatherModels::isAllowedWeatherUrl(const QUrl &url, bool allowLoopbackHttpForTests)
{
    if (!url.isValid() || url.isRelative()) {
        return false;
    }
    if (!url.userInfo().isEmpty()) {
        return false;
    }
    const QString raw = url.toString();
    for (const QChar ch : raw) {
        if (ch.isNull() || ch.category() == QChar::Other_Control) {
            return false;
        }
    }
    const QString scheme = url.scheme().toLower();
    const QString host = url.host().toLower();
    if (host.isEmpty()) {
        return false;
    }
    if (scheme == QLatin1String("https")) {
        return host == QLatin1String("api.open-meteo.com")
               || host == QLatin1String("geocoding-api.open-meteo.com")
               || host == QLatin1String("servis.mgm.gov.tr");
    }
    if (allowLoopbackHttpForTests && scheme == QLatin1String("http")) {
        return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost");
    }
    return false;
}

QString WeatherModels::normalizeTurkishProvinceQuery(const QString &raw)
{
    QString s = sanitizePlainText(raw, 64);
    const int comma = s.indexOf(QLatin1Char(','));
    if (comma > 0) {
        s = s.left(comma).trimmed();
    }
    // Strip common Turkish administrative suffix ("İli", "Province")
    if (s.endsWith(QStringLiteral(" İli"), Qt::CaseInsensitive)) {
        s.chop(4);
    } else if (s.endsWith(QStringLiteral(" Province"), Qt::CaseInsensitive)) {
        s.chop(9);
    }
    return s.trimmed();
}

QVariantList WeatherModels::parseOpenMeteoGeocoding(const QByteArray &rawJson)
{
    QVariantList out;
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
    if (!doc.isObject()) {
        return out;
    }
    const QJsonArray results = doc.object().value(QStringLiteral("results")).toArray();

    struct GeoCandidate
    {
        QVariantMap map;
        bool isTurkey = false;
        qint64 population = 0;
        int featureRank = 0;
    };
    QVector<GeoCandidate> candidates;
    candidates.reserve(results.size());

    for (const QJsonValue &val : results) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        const QString name = sanitizePlainText(obj.value(QStringLiteral("name")).toString(), 80);
        if (name.isEmpty() || !obj.value(QStringLiteral("latitude")).isDouble()
            || !obj.value(QStringLiteral("longitude")).isDouble()) {
            continue;
        }
        const double lat = obj.value(QStringLiteral("latitude")).toDouble();
        const double lon = obj.value(QStringLiteral("longitude")).toDouble();
        if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0
            || lon < -180.0 || lon > 180.0) {
            continue;
        }
        const QString countryCode =
            sanitizePlainText(obj.value(QStringLiteral("country_code")).toString(), 8).toUpper();
        QString country = sanitizePlainText(obj.value(QStringLiteral("country")).toString(), 64);
        if (countryCode == QLatin1String("TR")) {
            country = QStringLiteral("Türkiye");
        }
        const QString admin1 = sanitizePlainText(obj.value(QStringLiteral("admin1")).toString(), 64);
        const QString admin2 = sanitizePlainText(obj.value(QStringLiteral("admin2")).toString(), 64);
        const QString featureCode = obj.value(QStringLiteral("feature_code")).toString().toUpper();
        const qint64 pop = static_cast<qint64>(obj.value(QStringLiteral("population")).toDouble(0));

        QStringList adminParts;
        if (!admin1.isEmpty() && admin1.compare(name, Qt::CaseInsensitive) != 0) {
            adminParts.append(admin1);
        } else if (!admin2.isEmpty() && admin2.compare(name, Qt::CaseInsensitive) != 0) {
            adminParts.append(admin2);
        }
        if (!country.isEmpty()) {
            adminParts.append(country);
        }
        const QString adminSummary = adminParts.join(QStringLiteral(", "));
        const QString displayLabel = adminSummary.isEmpty()
            ? name
            : QStringLiteral("%1 — %2").arg(name, adminSummary);

        int fRank = 1;
        if (featureCode == QLatin1String("PPLC") || featureCode == QLatin1String("PPLA")) {
            fRank = 4;
        } else if (featureCode == QLatin1String("PPLA2") || featureCode == QLatin1String("PPLA3")) {
            fRank = 3;
        } else if (featureCode == QLatin1String("PPLX")) {
            fRank = 2;
        }

        QVariantMap item;
        item.insert(QStringLiteral("name"), name);
        item.insert(QStringLiteral("admin"), adminSummary);
        item.insert(QStringLiteral("province"), admin1.isEmpty() ? name : admin1);
        item.insert(QStringLiteral("countryCode"), countryCode);
        item.insert(QStringLiteral("displayLabel"), displayLabel);
        item.insert(QStringLiteral("latitude"), lat);
        item.insert(QStringLiteral("longitude"), lon);
        item.insert(QStringLiteral("population"), pop);

        candidates.append({item, countryCode == QLatin1String("TR"), pop, fRank});
    }

    // Prioritize Turkey (`TR`), then administrative importance & population so e.g.
    // "Kadıköy" places Kadıköy/İstanbul ahead of small villages.
    std::stable_sort(candidates.begin(), candidates.end(), [](const GeoCandidate &a, const GeoCandidate &b) {
        if (a.isTurkey != b.isTurkey) {
            return a.isTurkey > b.isTurkey;
        }
        if (a.population != b.population) {
            return a.population > b.population;
        }
        return a.featureRank > b.featureRank;
    });

    const qsizetype limit = std::min<qsizetype>(candidates.size(), 10);
    for (qsizetype i = 0; i < limit; ++i) {
        out.append(candidates[i].map);
    }
    return out;
}

WeatherSnapshot WeatherModels::parseOpenMeteoForecast(const QByteArray &rawJson,
                                                      const QString &locationName,
                                                      const QString &locationAdmin,
                                                      const QString &countryCode,
                                                      double lat,
                                                      double lon,
                                                      QString *errorOut)
{
    WeatherSnapshot snap;
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
    if (!doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Hava durumu yanıtı geçersiz JSON biçiminde.");
        }
        return snap;
    }

    const QJsonObject root = doc.object();
    const QJsonObject current = root.value(QStringLiteral("current")).toObject();
    if (current.isEmpty() || !current.value(QStringLiteral("temperature_2m")).isDouble()) {
        if (errorOut) {
            *errorOut = QStringLiteral("Anlık hava durumu ölçümü alınamadı.");
        }
        return snap;
    }

    snap.valid = true;
    snap.locationName = sanitizePlainText(locationName, 80);
    snap.locationAdmin = sanitizePlainText(locationAdmin, 120);
    snap.countryCode = sanitizePlainText(countryCode, 8).toUpper();
    snap.latitude = lat;
    snap.longitude = lon;
    snap.temperatureC = current.value(QStringLiteral("temperature_2m")).toDouble();
    snap.apparentTemperatureC = current.value(QStringLiteral("apparent_temperature")).toDouble(snap.temperatureC);
    snap.humidityPercent = std::clamp(current.value(QStringLiteral("relative_humidity_2m")).toInt(0), 0, 100);
    snap.windSpeedKmh = std::max(0.0, current.value(QStringLiteral("wind_speed_10m")).toDouble(0.0));
    snap.windDirectionDeg = current.value(QStringLiteral("wind_direction_10m")).toInt(0);
    snap.pressureHpa = std::max(0.0, current.value(QStringLiteral("surface_pressure")).toDouble(0.0));
    snap.weatherCode = current.value(QStringLiteral("weather_code")).toInt(0);
    snap.isDay = current.value(QStringLiteral("is_day")).toInt(1) != 0;
    snap.observedIsoTime = sanitizePlainText(current.value(QStringLiteral("time")).toString(), 40);
    snap.updatedDisplayTime = formatDisplayHour(snap.observedIsoTime);
    snap.sourceLabel = QStringLiteral("Open-Meteo");

    // 24-hour hourly strip starting from current local hour
    const QJsonObject hourly = root.value(QStringLiteral("hourly")).toObject();
    const QJsonArray hTimes = hourly.value(QStringLiteral("time")).toArray();
    const QJsonArray hTemps = hourly.value(QStringLiteral("temperature_2m")).toArray();
    const QJsonArray hProbs = hourly.value(QStringLiteral("precipitation_probability")).toArray();
    const QJsonArray hCodes = hourly.value(QStringLiteral("weather_code")).toArray();
    const QJsonArray hIsDay = hourly.value(QStringLiteral("is_day")).toArray();

    const QString currentHourPrefix = snap.observedIsoTime.size() >= 13
        ? snap.observedIsoTime.left(13)
        : QString();
    qsizetype startIdx = 0;
    if (!currentHourPrefix.isEmpty()) {
        for (qsizetype i = 0; i < hTimes.size(); ++i) {
            if (hTimes[i].toString().left(13) >= currentHourPrefix) {
                startIdx = i;
                break;
            }
        }
    }

    const qsizetype hLimit = std::min<qsizetype>(hTimes.size(), startIdx + 24);
    for (qsizetype i = startIdx; i < hLimit; ++i) {
        if (i >= hTemps.size()) {
            break;
        }
        const QString iso = hTimes[i].toString();
        const int wmo = (i < hCodes.size()) ? hCodes[i].toInt(0) : snap.weatherCode;
        const bool dayFlag = (i < hIsDay.size()) ? (hIsDay[i].toInt(1) != 0) : snap.isDay;
        const int prob = (i < hProbs.size()) ? std::clamp(hProbs[i].toInt(0), 0, 100) : 0;
        const WeatherCodeInfo info = describeWmoCode(wmo, dayFlag);

        QVariantMap entry;
        entry.insert(QStringLiteral("isoTime"), iso);
        entry.insert(QStringLiteral("timeLabel"), formatDisplayHour(iso));
        entry.insert(QStringLiteral("temperatureC"), std::round(hTemps[i].toDouble()));
        entry.insert(QStringLiteral("precipProbability"), prob);
        entry.insert(QStringLiteral("weatherCode"), wmo);
        entry.insert(QStringLiteral("iconName"), info.iconName);
        entry.insert(QStringLiteral("conditionText"), info.shortDescriptionTr);
        snap.hourlyForecast.append(entry);
    }

    // 7-day daily forecast array
    const QJsonObject daily = root.value(QStringLiteral("daily")).toObject();
    const QJsonArray dTimes = daily.value(QStringLiteral("time")).toArray();
    const QJsonArray dCodes = daily.value(QStringLiteral("weather_code")).toArray();
    const QJsonArray dMax = daily.value(QStringLiteral("temperature_2m_max")).toArray();
    const QJsonArray dMin = daily.value(QStringLiteral("temperature_2m_min")).toArray();
    const QJsonArray dProb = daily.value(QStringLiteral("precipitation_probability_max")).toArray();

    const qsizetype dCount = std::min<qsizetype>({dTimes.size(), dMax.size(), dMin.size(), 7});
    for (qsizetype i = 0; i < dCount; ++i) {
        const QString dateIso = dTimes[i].toString();
        const QDate date = QDate::fromString(dateIso, Qt::ISODate);
        const int wmo = (i < dCodes.size()) ? dCodes[i].toInt(0) : 0;
        const int prob = (i < dProb.size()) ? std::clamp(dProb[i].toInt(0), 0, 100) : 0;
        const WeatherCodeInfo info = describeWmoCode(wmo, true);

        QString dayLabel = date.isValid() ? turkishShortWeekday(date) : dateIso;
        if (i == 0) {
            dayLabel = QStringLiteral("Bugün");
        }

        QVariantMap day;
        day.insert(QStringLiteral("dateIso"), dateIso);
        day.insert(QStringLiteral("dayName"), dayLabel);
        day.insert(QStringLiteral("weekdayShort"), date.isValid() ? turkishShortWeekday(date) : dateIso);
        day.insert(QStringLiteral("weatherCode"), wmo);
        day.insert(QStringLiteral("iconName"), info.iconName);
        day.insert(QStringLiteral("conditionText"), info.descriptionTr);
        day.insert(QStringLiteral("shortCondition"), info.shortDescriptionTr);
        day.insert(QStringLiteral("tempMinC"), std::round(dMin[i].toDouble()));
        day.insert(QStringLiteral("tempMaxC"), std::round(dMax[i].toDouble()));
        day.insert(QStringLiteral("precipProbability"), prob);
        snap.dailyForecast.append(day);
    }

    return snap;
}

QString WeatherModels::selectMgmCenterId(const QByteArray &rawJson,
                                         double targetLat,
                                         double targetLon,
                                         const QString &districtHint)
{
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
    if (!doc.isArray()) {
        return QString();
    }
    const QJsonArray arr = doc.array();
    const QString hintLower = districtHint.trimmed().toLower();

    QString bestId;
    double bestScore = 1e18;

    for (const QJsonValue &val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        const int merkezId = obj.value(QStringLiteral("merkezId")).toInt(-1);
        if (merkezId <= 0) {
            continue;
        }
        const QString ilce = obj.value(QStringLiteral("ilce")).toString().trimmed().toLower();
        if (!hintLower.isEmpty() && ilce == hintLower) {
            return QString::number(merkezId);
        }
        const double lat = obj.value(QStringLiteral("enlem")).toDouble(0.0);
        const double lon = obj.value(QStringLiteral("boylam")).toDouble(0.0);
        const double distSq = approxDistSq(targetLat, targetLon, lat, lon);
        if (distSq < bestScore) {
            bestScore = distSq;
            bestId = QString::number(merkezId);
        }
    }
    return bestId;
}

MgmObservation WeatherModels::parseMgmObservation(const QByteArray &rawJson)
{
    MgmObservation obs;
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
    if (!doc.isArray() || doc.array().isEmpty()) {
        return obs;
    }
    const QJsonObject obj = doc.array().first().toObject();
    if (obj.isEmpty()) {
        return obs;
    }

    double val = 0.0;
    if (isMgmValidNumber(obj.value(QStringLiteral("sicaklik")), &val) && val > -90.0 && val < 70.0) {
        obs.hasTemperature = true;
        obs.temperatureC = val;
    }
    if (isMgmValidNumber(obj.value(QStringLiteral("nem")), &val) && val >= 0.0 && val <= 100.0) {
        obs.hasHumidity = true;
        obs.humidityPercent = static_cast<int>(std::round(val));
    }
    if (isMgmValidNumber(obj.value(QStringLiteral("ruzgarHiz")), &val) && val >= 0.0 && val < 300.0) {
        obs.hasWindSpeed = true;
        obs.windSpeedKmh = val;
    }
    if (isMgmValidNumber(obj.value(QStringLiteral("ruzgarYon")), &val) && val >= 0.0 && val <= 360.0) {
        obs.hasWindDirection = true;
        obs.windDirectionDeg = static_cast<int>(std::round(val));
    }
    if (isMgmValidNumber(obj.value(QStringLiteral("denizeIndirgenmisBasinc")), &val)
        && val >= 850.0 && val <= 1100.0) {
        obs.hasPressure = true;
        obs.pressureHpa = val;
    } else if (isMgmValidNumber(obj.value(QStringLiteral("aktuelBasinc")), &val)
               && val >= 850.0 && val <= 1100.0) {
        obs.hasPressure = true;
        obs.pressureHpa = val;
    }

    const QString hadise = obj.value(QStringLiteral("hadiseKodu")).toString();
    obs.mappedWmoCode = mgmEventCodeToWmo(hadise);
    obs.observedTime = sanitizePlainText(obj.value(QStringLiteral("veriZamani")).toString(), 40);
    obs.valid = obs.hasTemperature;
    return obs;
}

QByteArray WeatherModels::snapshotToCacheJson(const WeatherSnapshot &snapshot)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("locationName"), snapshot.locationName);
    root.insert(QStringLiteral("locationAdmin"), snapshot.locationAdmin);
    root.insert(QStringLiteral("countryCode"), snapshot.countryCode);
    root.insert(QStringLiteral("latitude"), snapshot.latitude);
    root.insert(QStringLiteral("longitude"), snapshot.longitude);
    root.insert(QStringLiteral("temperatureC"), snapshot.temperatureC);
    root.insert(QStringLiteral("apparentTemperatureC"), snapshot.apparentTemperatureC);
    root.insert(QStringLiteral("humidityPercent"), snapshot.humidityPercent);
    root.insert(QStringLiteral("windSpeedKmh"), snapshot.windSpeedKmh);
    root.insert(QStringLiteral("windDirectionDeg"), snapshot.windDirectionDeg);
    root.insert(QStringLiteral("pressureHpa"), snapshot.pressureHpa);
    root.insert(QStringLiteral("weatherCode"), snapshot.weatherCode);
    root.insert(QStringLiteral("isDay"), snapshot.isDay);
    root.insert(QStringLiteral("observedIsoTime"), snapshot.observedIsoTime);
    root.insert(QStringLiteral("updatedDisplayTime"), snapshot.updatedDisplayTime);
    root.insert(QStringLiteral("sourceLabel"), snapshot.sourceLabel);
    root.insert(QStringLiteral("hourlyForecast"), QJsonArray::fromVariantList(snapshot.hourlyForecast));
    root.insert(QStringLiteral("dailyForecast"), QJsonArray::fromVariantList(snapshot.dailyForecast));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

WeatherSnapshot WeatherModels::snapshotFromCacheJson(const QByteArray &rawJson)
{
    WeatherSnapshot snap;
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson);
    if (!doc.isObject()) {
        return snap;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return snap;
    }
    if (!root.value(QStringLiteral("temperatureC")).isDouble()) {
        return snap;
    }
    snap.valid = true;
    snap.locationName = sanitizePlainText(root.value(QStringLiteral("locationName")).toString(), 80);
    snap.locationAdmin = sanitizePlainText(root.value(QStringLiteral("locationAdmin")).toString(), 120);
    snap.countryCode = sanitizePlainText(root.value(QStringLiteral("countryCode")).toString(), 8).toUpper();
    snap.latitude = root.value(QStringLiteral("latitude")).toDouble(41.0082);
    snap.longitude = root.value(QStringLiteral("longitude")).toDouble(28.9784);
    snap.temperatureC = root.value(QStringLiteral("temperatureC")).toDouble();
    snap.apparentTemperatureC = root.value(QStringLiteral("apparentTemperatureC")).toDouble(snap.temperatureC);
    snap.humidityPercent = std::clamp(root.value(QStringLiteral("humidityPercent")).toInt(0), 0, 100);
    snap.windSpeedKmh = std::max(0.0, root.value(QStringLiteral("windSpeedKmh")).toDouble(0.0));
    snap.windDirectionDeg = root.value(QStringLiteral("windDirectionDeg")).toInt(0);
    snap.pressureHpa = std::max(0.0, root.value(QStringLiteral("pressureHpa")).toDouble(0.0));
    snap.weatherCode = root.value(QStringLiteral("weatherCode")).toInt(0);
    snap.isDay = root.value(QStringLiteral("isDay")).toBool(true);
    snap.observedIsoTime = sanitizePlainText(root.value(QStringLiteral("observedIsoTime")).toString(), 40);
    snap.updatedDisplayTime = sanitizePlainText(root.value(QStringLiteral("updatedDisplayTime")).toString(), 20);
    snap.sourceLabel = sanitizePlainText(root.value(QStringLiteral("sourceLabel")).toString(), 40);
    snap.hourlyForecast = root.value(QStringLiteral("hourlyForecast")).toArray().toVariantList();
    snap.dailyForecast = root.value(QStringLiteral("dailyForecast")).toArray().toVariantList();
    return snap;
}

} // namespace zethropol::weather
