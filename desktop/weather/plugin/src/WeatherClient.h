#pragma once

#include "WeatherModels.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QNetworkReply;

namespace zethropol::weather {

/// Hardened HTTPS-only Qt6::Network weather client for `org.zethropol.plasma.weather`.
/// - Zero shell / subprocess invocation (`QProcess` is never used).
/// - Primary forecast & geocoding: Open-Meteo (API-key-free, `language=tr`, 7-day + 24h).
/// - Optional live Turkish observation enrichment/fallback: MGM (`servis.mgm.gov.tr`).
/// - Atomic local disk cache fallback with `offlineCache=true` ("Çevrimdışı önbellek").
class WeatherClient : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString locationName READ locationName WRITE setLocationName NOTIFY locationChanged)
    Q_PROPERTY(QString locationAdmin READ locationAdmin WRITE setLocationAdmin NOTIFY locationChanged)
    Q_PROPERTY(QString countryCode READ countryCode WRITE setCountryCode NOTIFY locationChanged)
    Q_PROPERTY(double latitude READ latitude WRITE setLatitude NOTIFY locationChanged)
    Q_PROPERTY(double longitude READ longitude WRITE setLongitude NOTIFY locationChanged)

    Q_PROPERTY(int forecastDays READ forecastDays WRITE setForecastDays NOTIFY forecastDaysChanged)
    Q_PROPERTY(bool useMgmObservation READ useMgmObservation WRITE setUseMgmObservation NOTIFY settingsChanged)

    Q_PROPERTY(bool hasData READ hasData NOTIFY dataChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool offlineCache READ offlineCache NOTIFY dataChanged)
    Q_PROPERTY(QString offlineBadgeText READ offlineBadgeText CONSTANT)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY dataChanged)
    Q_PROPERTY(QString lastUpdatedText READ lastUpdatedText NOTIFY dataChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)

    Q_PROPERTY(double temperatureC READ temperatureC NOTIFY dataChanged)
    Q_PROPERTY(double apparentTemperatureC READ apparentTemperatureC NOTIFY dataChanged)
    Q_PROPERTY(int humidityPercent READ humidityPercent NOTIFY dataChanged)
    Q_PROPERTY(double windSpeedKmh READ windSpeedKmh NOTIFY dataChanged)
    Q_PROPERTY(int windDirectionDeg READ windDirectionDeg NOTIFY dataChanged)
    Q_PROPERTY(QString windDirectionText READ windDirectionText NOTIFY dataChanged)
    Q_PROPERTY(double pressureHpa READ pressureHpa NOTIFY dataChanged)
    Q_PROPERTY(int weatherCode READ weatherCode NOTIFY dataChanged)
    Q_PROPERTY(bool isDay READ isDay NOTIFY dataChanged)
    Q_PROPERTY(QString conditionText READ conditionText NOTIFY dataChanged)
    Q_PROPERTY(QString conditionIcon READ conditionIcon NOTIFY dataChanged)
    Q_PROPERTY(QString themeMood READ themeMood NOTIFY dataChanged)

    Q_PROPERTY(QVariantList hourlyForecast READ hourlyForecast NOTIFY dataChanged)
    Q_PROPERTY(QVariantList dailyForecast READ dailyForecast NOTIFY dataChanged)
    Q_PROPERTY(QVariantList visibleDailyForecast READ visibleDailyForecast NOTIFY forecastDaysChanged)

    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged)
    Q_PROPERTY(QString searchError READ searchError NOTIFY searchChanged)

public:
    static constexpr int kNetworkTimeoutMs = 10000;

    explicit WeatherClient(QObject *parent = nullptr);
    ~WeatherClient() override;

    QString locationName() const { return m_locationName; }
    void setLocationName(const QString &name);
    QString locationAdmin() const { return m_locationAdmin; }
    void setLocationAdmin(const QString &admin);
    QString countryCode() const { return m_countryCode; }
    void setCountryCode(const QString &code);
    double latitude() const { return m_latitude; }
    void setLatitude(double lat);
    double longitude() const { return m_longitude; }
    void setLongitude(double lon);

    int forecastDays() const { return m_forecastDays; }
    void setForecastDays(int days);

    bool useMgmObservation() const { return m_useMgmObservation; }
    void setUseMgmObservation(bool enabled);

    bool hasData() const { return m_snapshot.valid; }
    bool loading() const { return m_loading; }
    bool offlineCache() const { return m_offlineCache; }
    QString offlineBadgeText() const { return QStringLiteral("Çevrimdışı önbellek"); }
    QString sourceLabel() const { return m_snapshot.sourceLabel; }
    QString lastUpdatedText() const { return m_snapshot.updatedDisplayTime; }
    QString errorMessage() const { return m_errorMessage; }

    double temperatureC() const { return m_snapshot.temperatureC; }
    double apparentTemperatureC() const { return m_snapshot.apparentTemperatureC; }
    int humidityPercent() const { return m_snapshot.humidityPercent; }
    double windSpeedKmh() const { return m_snapshot.windSpeedKmh; }
    int windDirectionDeg() const { return m_snapshot.windDirectionDeg; }
    QString windDirectionText() const { return WeatherModels::windCompassTr(m_snapshot.windDirectionDeg); }
    double pressureHpa() const { return m_snapshot.pressureHpa; }
    int weatherCode() const { return m_snapshot.weatherCode; }
    bool isDay() const { return m_snapshot.isDay; }
    QString conditionText() const;
    QString conditionIcon() const;
    QString themeMood() const;

    QVariantList hourlyForecast() const { return m_snapshot.hourlyForecast; }
    QVariantList dailyForecast() const { return m_snapshot.dailyForecast; }
    QVariantList visibleDailyForecast() const;

    bool searching() const { return m_searching; }
    QVariantList searchResults() const { return m_searchResults; }
    QString searchError() const { return m_searchError; }

    Q_INVOKABLE void configureLocation(const QString &name,
                                       const QString &admin,
                                       const QString &countryCode,
                                       double latitude,
                                       double longitude);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void searchLocations(const QString &query);
    Q_INVOKABLE void clearSearchResults();
    Q_INVOKABLE bool loadCachedSnapshot();

    Q_INVOKABLE static int clampOpacityPercent(int percent);
    Q_INVOKABLE static int normalizeForecastDays(int days);

    /// C++-only deterministic test seams (never exposed to QML).
    void setCacheDirectoryOverride(const QString &dirPath);
    void setEndpointOverridesForTest(const QString &geocodingBase,
                                     const QString &forecastBase,
                                     const QString &mgmBase);
    QString cacheFilePath() const;

signals:
    void locationChanged();
    void forecastDaysChanged();
    void settingsChanged();
    void dataChanged();
    void loadingChanged();
    void errorChanged();
    void searchChanged();
    void refreshCompleted(bool success, bool fromCache);

private:
    void abortActiveWeatherReplies();
    void startMgmLookup(quint64 reqSeq);
    void startMgmObservationFetch(const QString &merkezId, quint64 reqSeq);
    void applySnapshot(const WeatherSnapshot &snap, bool fromOfflineCache);
    void handleFetchFailure(const QString &reason);
    bool saveSnapshotToCache(const WeatherSnapshot &snap) const;
    void setLoading(bool loading);
    void setErrorMessage(const QString &err);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_forecastReply;
    QPointer<QNetworkReply> m_mgmReply;
    QPointer<QNetworkReply> m_geoReply;
    quint64 m_requestSeq = 0;

    QString m_locationName = QStringLiteral("İstanbul");
    QString m_locationAdmin = QStringLiteral("İstanbul, Türkiye");
    QString m_countryCode = QStringLiteral("TR");
    double m_latitude = 41.0082;
    double m_longitude = 28.9784;

    int m_forecastDays = WeatherModels::kDefaultForecastDays;
    bool m_useMgmObservation = true;

    WeatherSnapshot m_snapshot;
    bool m_loading = false;
    bool m_offlineCache = false;
    QString m_errorMessage;

    bool m_searching = false;
    QVariantList m_searchResults;
    QString m_searchError;

    QString m_cacheDirOverride;
    QString m_geocodingBaseOverride;
    QString m_forecastBaseOverride;
    QString m_mgmBaseOverride;
};

} // namespace zethropol::weather
