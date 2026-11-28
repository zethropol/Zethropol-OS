#include "WeatherClient.h"
#include "WeatherModels.h"

#include <QDate>
#include <QFile>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace zethropol::weather;

namespace {

QByteArray sampleOpenMeteoForecastJson()
{
    return QByteArrayLiteral(
        "{\n"
        "  \"latitude\": 41.0082,\n"
        "  \"longitude\": 28.9784,\n"
        "  \"current\": {\n"
        "    \"time\": \"2026-11-28T15:00\",\n"
        "    \"temperature_2m\": 17.4,\n"
        "    \"relative_humidity_2m\": 68,\n"
        "    \"apparent_temperature\": 16.6,\n"
        "    \"is_day\": 1,\n"
        "    \"weather_code\": 2,\n"
        "    \"surface_pressure\": 1012.3,\n"
        "    \"wind_speed_10m\": 11.2,\n"
        "    \"wind_direction_10m\": 45\n"
        "  },\n"
        "  \"hourly\": {\n"
        "    \"time\": ["
        "\"2026-11-28T14:00\",\"2026-11-28T15:00\",\"2026-11-28T16:00\",\"2026-11-28T17:00\","
        "\"2026-11-28T18:00\",\"2026-11-28T19:00\",\"2026-11-28T20:00\",\"2026-11-28T21:00\","
        "\"2026-11-28T22:00\",\"2026-11-28T23:00\",\"2026-11-29T00:00\",\"2026-11-29T01:00\","
        "\"2026-11-29T02:00\",\"2026-11-29T03:00\",\"2026-11-29T04:00\",\"2026-11-29T05:00\","
        "\"2026-11-29T06:00\",\"2026-11-29T07:00\",\"2026-11-29T08:00\",\"2026-11-29T09:00\","
        "\"2026-11-29T10:00\",\"2026-11-29T11:00\",\"2026-11-29T12:00\",\"2026-11-29T13:00\","
        "\"2026-11-29T14:00\",\"2026-11-29T15:00\"],\n"
        "    \"temperature_2m\": [17,17.4,17,16,15,14,13,13,12,12,11,11,10,10,10,10,11,12,13,14,15,16,17,18,18,17],\n"
        "    \"precipitation_probability\": [0,10,15,20,25,30,10,5,0,0,0,0,5,10,15,20,25,30,35,40,20,10,5,0,0,0],\n"
        "    \"weather_code\": [2,2,3,61,61,3,2,1,0,0,0,1,2,3,3,61,61,80,3,2,1,0,0,1,2,2],\n"
        "    \"is_day\": [1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1]\n"
        "  },\n"
        "  \"daily\": {\n"
        "    \"time\": [\"2026-11-28\",\"2026-11-29\",\"2026-11-30\",\"2026-12-01\",\"2026-12-02\",\"2026-12-03\",\"2026-12-04\"],\n"
        "    \"weather_code\": [2,61,3,0,80,73,95],\n"
        "    \"temperature_2m_max\": [18.2,15.6,14.0,16.8,13.5,4.1,11.0],\n"
        "    \"temperature_2m_min\": [11.1,9.4,8.2,8.9,7.0,-1.5,5.2],\n"
        "    \"precipitation_probability_max\": [20,75,30,5,65,80,90]\n"
        "  }\n"
        "}\n");
}

} // namespace

class WeatherCoreTest : public QObject
{
    Q_OBJECT

private slots:
    void wmoCodesMapToNaturalTurkishIconsAndMoods()
    {
        const WeatherCodeInfo clearDay = WeatherModels::describeWmoCode(0, true);
        QCOMPARE(clearDay.descriptionTr, QStringLiteral("Açık"));
        QCOMPARE(clearDay.iconName, QStringLiteral("weather-clear"));
        QCOMPARE(clearDay.mood, QStringLiteral("clear-day"));

        const WeatherCodeInfo clearNight = WeatherModels::describeWmoCode(0, false);
        QCOMPARE(clearNight.descriptionTr, QStringLiteral("Açık"));
        QCOMPARE(clearNight.iconName, QStringLiteral("weather-clear-night"));
        QCOMPARE(clearNight.mood, QStringLiteral("clear-night"));

        QCOMPARE(WeatherModels::describeWmoCode(2, true).descriptionTr, QStringLiteral("Parçalı Bulutlu"));
        QCOMPARE(WeatherModels::describeWmoCode(3, true).descriptionTr, QStringLiteral("Çok Bulutlu"));
        QCOMPARE(WeatherModels::describeWmoCode(45, true).descriptionTr, QStringLiteral("Sisli"));
        QCOMPARE(WeatherModels::describeWmoCode(61, true).descriptionTr, QStringLiteral("Hafif Yağmurlu"));
        QCOMPARE(WeatherModels::describeWmoCode(63, true).descriptionTr, QStringLiteral("Yağmurlu"));
        QCOMPARE(WeatherModels::describeWmoCode(73, true).descriptionTr, QStringLiteral("Kar Yağışlı"));
        QCOMPARE(WeatherModels::describeWmoCode(81, true).descriptionTr, QStringLiteral("Sağanak Yağışlı"));
        QCOMPARE(WeatherModels::describeWmoCode(95, true).descriptionTr, QStringLiteral("Gök Gürültülü Fırtına"));
        QCOMPARE(WeatherModels::describeWmoCode(99, true).mood, QStringLiteral("storm"));

        // Unknown WMO code degrades gracefully
        const WeatherCodeInfo fallback = WeatherModels::describeWmoCode(777, true);
        QVERIFY(!fallback.descriptionTr.isEmpty());
        QVERIFY(!fallback.iconName.isEmpty());

        // Turkish weekdays & 16-point wind compass
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 23)), QStringLiteral("Pzt"));
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 24)), QStringLiteral("Sal"));
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 25)), QStringLiteral("Çar"));
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 26)), QStringLiteral("Per"));
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 27)), QStringLiteral("Cum"));
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 28)), QStringLiteral("Cmt"));
        QCOMPARE(WeatherModels::turkishShortWeekday(QDate(2026, 11, 29)), QStringLiteral("Paz"));

        QCOMPARE(WeatherModels::windCompassTr(0), QStringLiteral("K"));
        QCOMPARE(WeatherModels::windCompassTr(45), QStringLiteral("KD"));
        QCOMPARE(WeatherModels::windCompassTr(90), QStringLiteral("D"));
        QCOMPARE(WeatherModels::windCompassTr(180), QStringLiteral("G"));
        QCOMPARE(WeatherModels::windCompassTr(270), QStringLiteral("B"));
        QCOMPARE(WeatherModels::windCompassTr(359), QStringLiteral("K"));
    }

    void forecastDaysSlicingAndOpacityBoundsAreStrictlyEnforced()
    {
        // Opacity clamped strictly to [15%, 100%] ([0.15, 1.00])
        QCOMPARE(WeatherModels::clampOpacityPercent(-10), 15);
        QCOMPARE(WeatherModels::clampOpacityPercent(0), 15);
        QCOMPARE(WeatherModels::clampOpacityPercent(14), 15);
        QCOMPARE(WeatherModels::clampOpacityPercent(15), 15);
        QCOMPARE(WeatherModels::clampOpacityPercent(82), 82);
        QCOMPARE(WeatherModels::clampOpacityPercent(92), 92);
        QCOMPARE(WeatherModels::clampOpacityPercent(100), 100);
        QCOMPARE(WeatherModels::clampOpacityPercent(150), 100);
        QVERIFY(qAbs(WeatherModels::opacityFactorFromPercent(15) - 0.15) < 1e-6);
        QVERIFY(qAbs(WeatherModels::opacityFactorFromPercent(82) - 0.82) < 1e-6);
        QVERIFY(qAbs(WeatherModels::opacityFactorFromPercent(100) - 1.00) < 1e-6);

        // Forecast day selector accepts strictly 3, 5, or 7 (defaults to 5)
        QCOMPARE(WeatherModels::normalizeForecastDays(3), 3);
        QCOMPARE(WeatherModels::normalizeForecastDays(5), 5);
        QCOMPARE(WeatherModels::normalizeForecastDays(7), 7);
        QCOMPARE(WeatherModels::normalizeForecastDays(0), 5);
        QCOMPARE(WeatherModels::normalizeForecastDays(4), 5);
        QCOMPARE(WeatherModels::normalizeForecastDays(14), 5);

        QString err;
        const WeatherSnapshot snap = WeatherModels::parseOpenMeteoForecast(
            sampleOpenMeteoForecastJson(),
            QStringLiteral("İstanbul"),
            QStringLiteral("İstanbul, Türkiye"),
            QStringLiteral("TR"),
            41.0082,
            28.9784,
            &err);
        QVERIFY2(snap.valid, qPrintable(err));
        QCOMPARE(snap.dailyForecast.size(), 7);
        QCOMPARE(snap.hourlyForecast.size(), 24);
        QCOMPARE(WeatherModels::sliceDailyForecast(snap.dailyForecast, 3).size(), 3);
        QCOMPARE(WeatherModels::sliceDailyForecast(snap.dailyForecast, 5).size(), 5);
        QCOMPARE(WeatherModels::sliceDailyForecast(snap.dailyForecast, 7).size(), 7);
        QCOMPARE(WeatherModels::sliceDailyForecast(snap.dailyForecast, 99).size(), 5);

        // First day labeled "Bugün" with Turkish weekdayShort "Cmt"
        const QVariantMap day0 = snap.dailyForecast[0].toMap();
        QCOMPARE(day0.value(QStringLiteral("dayName")).toString(), QStringLiteral("Bugün"));
        QCOMPARE(day0.value(QStringLiteral("weekdayShort")).toString(), QStringLiteral("Cmt"));
        QCOMPARE(day0.value(QStringLiteral("precipProbability")).toInt(), 20);
        const QVariantMap day1 = snap.dailyForecast[1].toMap();
        QCOMPARE(day1.value(QStringLiteral("dayName")).toString(), QStringLiteral("Paz"));
    }

    void geocodingPrioritizesTurkeyAndMgmSentinelFilteringWorks()
    {
        const QByteArray geoJson = QByteArrayLiteral(
            "{\"results\":["
            "{\"name\":\"Kadıköy\",\"latitude\":40.62,\"longitude\":29.22,\"country_code\":\"TR\",\"country\":\"Türkiye Cumhuriyeti\",\"admin1\":\"Yalova\",\"population\":2700,\"feature_code\":\"PPL\"},"
            "{\"name\":\"Kadikoy\",\"latitude\":42.0,\"longitude\":25.0,\"country_code\":\"BG\",\"country\":\"Bulgaria\",\"admin1\":\"Haskovo\",\"population\":5000,\"feature_code\":\"PPL\"},"
            "{\"name\":\"Kadıköy\",\"latitude\":40.9909,\"longitude\":29.0293,\"country_code\":\"TR\",\"country\":\"Türkiye Cumhuriyeti\",\"admin1\":\"İstanbul\",\"population\":485000,\"feature_code\":\"PPLA2\"}"
            "]}");
        const QVariantList results = WeatherModels::parseOpenMeteoGeocoding(geoJson);
        QCOMPARE(results.size(), 3);
        // İstanbul Kadıköy (TR, pop 485k) MUST rank #1!
        QCOMPARE(results[0].toMap().value(QStringLiteral("admin")).toString(), QStringLiteral("İstanbul, Türkiye"));
        QCOMPARE(results[0].toMap().value(QStringLiteral("countryCode")).toString(), QStringLiteral("TR"));
        // Bulgaria ranked after TR entries
        QCOMPARE(results[2].toMap().value(QStringLiteral("countryCode")).toString(), QStringLiteral("BG"));

        // MGM center selection & -9999 sentinel filtering
        const QByteArray mgmCenters = QByteArrayLiteral(
            "[{\"merkezId\":93401,\"il\":\"İstanbul\",\"ilce\":\"Bakırköy\",\"enlem\":40.98,\"boylam\":28.82},"
            " {\"merkezId\":93414,\"il\":\"İstanbul\",\"ilce\":\"Kadıköy\",\"enlem\":40.99,\"boylam\":29.03}]");
        QCOMPARE(WeatherModels::selectMgmCenterId(mgmCenters, 40.99, 29.03, QStringLiteral("Kadıköy")),
                 QStringLiteral("93414"));

        const QByteArray mgmObsJson = QByteArrayLiteral(
            "[{\"sicaklik\":16.2,\"nem\":74,\"ruzgarHiz\":9.5,\"ruzgarYon\":30,"
            "\"aktuelBasinc\":-9999,\"denizeIndirgenmisBasinc\":1015.8,\"hadiseKodu\":\"CB\",\"veriZamani\":\"2026-11-28T12:00:00Z\"}]");
        const MgmObservation obs = WeatherModels::parseMgmObservation(mgmObsJson);
        QVERIFY(obs.valid);
        QVERIFY(obs.hasTemperature);
        QCOMPARE(obs.temperatureC, 16.2);
        QCOMPARE(obs.humidityPercent, 74);
        QVERIFY(obs.hasPressure);
        QCOMPARE(obs.pressureHpa, 1015.8);
        QCOMPARE(obs.mappedWmoCode, 3);

        // All-sentinel MGM observation rejected cleanly
        const QByteArray badMgm = QByteArrayLiteral("[{\"sicaklik\":-9999,\"nem\":-9999}]");
        QVERIFY(!WeatherModels::parseMgmObservation(badMgm).valid);
    }

    void securityUrlAllowlistRejectsUnsafeSchemesAndUnknownHosts()
    {
        QVERIFY(WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("https://api.open-meteo.com/v1/forecast"))));
        QVERIFY(WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search?name=Istanbul"))));
        QVERIFY(WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("https://servis.mgm.gov.tr/web/merkezler?il=Istanbul"))));

        QVERIFY(!WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("http://api.open-meteo.com/v1/forecast"))));
        QVERIFY(!WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("file:///etc/passwd"))));
        QVERIFY(!WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("https://evil.example.org/weather"))));
        QVERIFY(!WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("https://user:pass@api.open-meteo.com/v1/forecast"))));
        QVERIFY(!WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("http://127.0.0.1:8080/v1/forecast")), false));
        QVERIFY(WeatherModels::isAllowedWeatherUrl(QUrl(QStringLiteral("http://127.0.0.1:8080/v1/forecast")), true));
    }

    void asyncLocalHttpServerEnrichesMgmAndFallsBackToOfflineCacheOnNetworkFailure()
    {
        bool failAll = false;
        bool failMgm = false;

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &failAll, &failMgm]() {
            while (QTcpSocket *sock = server.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, &failAll, &failMgm]() {
                    const QByteArray req = sock->readAll();
                    const QByteArray firstLine = req.split('\n').value(0);
                    QByteArray body;
                    QByteArray status = "200 OK";

                    if (failAll) {
                        status = "503 Service Unavailable";
                        body = "{\"error\":true}";
                    } else if (firstLine.contains("GET /forecast")) {
                        body = sampleOpenMeteoForecastJson();
                    } else if (firstLine.contains("GET /search")) {
                        body = "{\"results\":[{\"name\":\"Çankaya\",\"latitude\":39.918,\"longitude\":32.854,\"country_code\":\"TR\",\"country\":\"Türkiye Cumhuriyeti\",\"admin1\":\"Ankara\",\"population\":942000,\"feature_code\":\"PPLA2\"}]}";
                    } else if (firstLine.contains("GET /mgm/merkezler")) {
                        if (failMgm) {
                            status = "500 Internal Server Error";
                            body = "err";
                        } else {
                            body = "[{\"merkezId\":93414,\"il\":\"İstanbul\",\"ilce\":\"İstanbul\",\"enlem\":41.01,\"boylam\":28.98}]";
                        }
                    } else if (firstLine.contains("GET /mgm/sondurumlar")) {
                        body = "[{\"sicaklik\":19.0,\"nem\":62,\"ruzgarHiz\":14.0,\"ruzgarYon\":90,\"denizeIndirgenmisBasinc\":1018.4,\"hadiseKodu\":\"A\",\"veriZamani\":\"2026-11-28T12:15:00Z\"}]";
                    } else {
                        status = "404 Not Found";
                        body = "{}";
                    }

                    const QByteArray resp =
                        "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
                QObject::connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            }
        });

        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());

        WeatherClient client;
        client.setCacheDirectoryOverride(cacheDir.path());
        client.setEndpointOverridesForTest(
            QStringLiteral("http://127.0.0.1:%1/search").arg(port),
            QStringLiteral("http://127.0.0.1:%1/forecast").arg(port),
            QStringLiteral("http://127.0.0.1:%1/mgm").arg(port));

        // 1. Live Open-Meteo + MGM enrichment succeeds and writes atomic disk cache
        QSignalSpy doneSpy(&client, &WeatherClient::refreshCompleted);
        client.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 5000);
        QCOMPARE(doneSpy.first().at(0).toBool(), true);
        QCOMPARE(doneSpy.first().at(1).toBool(), false);
        QVERIFY(client.hasData());
        QVERIFY(!client.offlineCache());
        QCOMPARE(client.sourceLabel(), QStringLiteral("Open-Meteo + MGM"));
        QCOMPARE(client.temperatureC(), 19.0);
        QCOMPARE(client.humidityPercent(), 62);
        QCOMPARE(client.conditionText(), QStringLiteral("Açık"));
        QCOMPARE(client.dailyForecast().size(), 7);
        QCOMPARE(client.visibleDailyForecast().size(), 5);
        QVERIFY(QFile::exists(client.cacheFilePath()));

        // Dynamic 3/5/7 day slicing updates immediately without re-fetching
        client.setForecastDays(3);
        QCOMPARE(client.visibleDailyForecast().size(), 3);
        client.setForecastDays(7);
        QCOMPARE(client.visibleDailyForecast().size(), 7);

        // 2. MGM failure falls back cleanly to Open-Meteo only
        failMgm = true;
        doneSpy.clear();
        client.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 5000);
        QCOMPARE(doneSpy.first().at(0).toBool(), true);
        QCOMPARE(client.sourceLabel(), QStringLiteral("Open-Meteo"));
        QCOMPARE(client.temperatureC(), 17.4);

        // 3. Turkish geocoding search resolves Çankaya, Ankara
        QSignalSpy searchSpy(&client, &WeatherClient::searchChanged);
        client.searchLocations(QStringLiteral("Çankaya"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.searching() && !client.searchResults().isEmpty(), 5000);
        QCOMPARE(client.searchResults().first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Çankaya"));

        // 4. Fresh client under total network outage loads cached snapshot & sets offlineCache=true
        failAll = true;
        WeatherClient offlineClient;
        offlineClient.setCacheDirectoryOverride(cacheDir.path());
        offlineClient.setEndpointOverridesForTest(
            QStringLiteral("http://127.0.0.1:%1/search").arg(port),
            QStringLiteral("http://127.0.0.1:%1/forecast").arg(port),
            QStringLiteral("http://127.0.0.1:%1/mgm").arg(port));

        QSignalSpy offSpy(&offlineClient, &WeatherClient::refreshCompleted);
        offlineClient.refresh();
        QTRY_COMPARE_WITH_TIMEOUT(offSpy.count(), 1, 5000);
        QCOMPARE(offSpy.first().at(0).toBool(), false);
        QCOMPARE(offSpy.first().at(1).toBool(), true);
        QVERIFY(offlineClient.hasData());
        QVERIFY(offlineClient.offlineCache());
        QCOMPARE(offlineClient.offlineBadgeText(), QStringLiteral("Çevrimdışı önbellek"));
        QCOMPARE(offlineClient.dailyForecast().size(), 7);
    }
};

QTEST_MAIN(WeatherCoreTest)
#include "WeatherCoreTest.moc"
