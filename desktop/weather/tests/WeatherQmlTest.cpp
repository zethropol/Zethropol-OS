#include <QColor>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtQml>
#include <QtTest>

namespace {

class MockPlasmaTypes : public QObject
{
    Q_OBJECT
public:
    enum BackgroundHints {
        NoBackground = 0,
        DefaultBackground = 1,
        StandardBackground = 1,
    };
    Q_ENUM(BackgroundHints)

    enum FormFactor {
        Planar = 0,
        MediaCenter = 1,
        Horizontal = 2,
        Vertical = 3,
        Application = 4,
    };
    Q_ENUM(FormFactor)
};

class MockWeatherConfig : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString locationName READ locationName WRITE setLocationName NOTIFY locationNameChanged)
    Q_PROPERTY(QString locationAdmin READ locationAdmin WRITE setLocationAdmin NOTIFY locationAdminChanged)
    Q_PROPERTY(QString countryCode READ countryCode WRITE setCountryCode NOTIFY countryCodeChanged)
    Q_PROPERTY(double latitude READ latitude WRITE setLatitude NOTIFY latitudeChanged)
    Q_PROPERTY(double longitude READ longitude WRITE setLongitude NOTIFY longitudeChanged)
    Q_PROPERTY(int forecastDays READ forecastDays WRITE setForecastDays NOTIFY forecastDaysChanged)
    Q_PROPERTY(int desktopOpacity READ desktopOpacity WRITE setDesktopOpacity NOTIFY desktopOpacityChanged)
    Q_PROPERTY(int popupOpacity READ popupOpacity WRITE setPopupOpacity NOTIFY popupOpacityChanged)
    Q_PROPERTY(bool useCustomGlassBackground READ useCustomGlassBackground WRITE setUseCustomGlassBackground NOTIFY useCustomGlassBackgroundChanged)
    Q_PROPERTY(bool useMgmObservation READ useMgmObservation WRITE setUseMgmObservation NOTIFY useMgmObservationChanged)
    Q_PROPERTY(int refreshIntervalMinutes READ refreshIntervalMinutes WRITE setRefreshIntervalMinutes NOTIFY refreshIntervalMinutesChanged)

public:
    explicit MockWeatherConfig(QObject *parent = nullptr)
        : QObject(parent)
    {}

    QString locationName() const { return m_locationName; }
    void setLocationName(const QString &v) { if (m_locationName != v) { m_locationName = v; emit locationNameChanged(); } }
    QString locationAdmin() const { return m_locationAdmin; }
    void setLocationAdmin(const QString &v) { if (m_locationAdmin != v) { m_locationAdmin = v; emit locationAdminChanged(); } }
    QString countryCode() const { return m_countryCode; }
    void setCountryCode(const QString &v) { if (m_countryCode != v) { m_countryCode = v; emit countryCodeChanged(); } }
    double latitude() const { return m_latitude; }
    void setLatitude(double v) { if (m_latitude != v) { m_latitude = v; emit latitudeChanged(); } }
    double longitude() const { return m_longitude; }
    void setLongitude(double v) { if (m_longitude != v) { m_longitude = v; emit longitudeChanged(); } }
    int forecastDays() const { return m_forecastDays; }
    void setForecastDays(int v) { if (m_forecastDays != v) { m_forecastDays = v; emit forecastDaysChanged(); } }
    int desktopOpacity() const { return m_desktopOpacity; }
    void setDesktopOpacity(int v) { if (m_desktopOpacity != v) { m_desktopOpacity = v; emit desktopOpacityChanged(); } }
    int popupOpacity() const { return m_popupOpacity; }
    void setPopupOpacity(int v) { if (m_popupOpacity != v) { m_popupOpacity = v; emit popupOpacityChanged(); } }
    bool useCustomGlassBackground() const { return m_useCustomGlassBackground; }
    void setUseCustomGlassBackground(bool v) { if (m_useCustomGlassBackground != v) { m_useCustomGlassBackground = v; emit useCustomGlassBackgroundChanged(); } }
    bool useMgmObservation() const { return m_useMgmObservation; }
    void setUseMgmObservation(bool v) { if (m_useMgmObservation != v) { m_useMgmObservation = v; emit useMgmObservationChanged(); } }
    int refreshIntervalMinutes() const { return m_refreshIntervalMinutes; }
    void setRefreshIntervalMinutes(int v) { if (m_refreshIntervalMinutes != v) { m_refreshIntervalMinutes = v; emit refreshIntervalMinutesChanged(); } }

signals:
    void locationNameChanged();
    void locationAdminChanged();
    void countryCodeChanged();
    void latitudeChanged();
    void longitudeChanged();
    void forecastDaysChanged();
    void desktopOpacityChanged();
    void popupOpacityChanged();
    void useCustomGlassBackgroundChanged();
    void useMgmObservationChanged();
    void refreshIntervalMinutesChanged();

private:
    QString m_locationName = QStringLiteral("İstanbul");
    QString m_locationAdmin = QStringLiteral("İstanbul, Türkiye");
    QString m_countryCode = QStringLiteral("TR");
    double m_latitude = 41.0082;
    double m_longitude = 28.9784;
    int m_forecastDays = 5;
    int m_desktopOpacity = 82;
    int m_popupOpacity = 92;
    bool m_useCustomGlassBackground = true;
    bool m_useMgmObservation = true;
    int m_refreshIntervalMinutes = 15;
};

class MockPlasmoidAttached : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle NOTIFY titleChanged)
    Q_PROPERTY(int backgroundHints READ backgroundHints WRITE setBackgroundHints NOTIFY backgroundHintsChanged)
    Q_PROPERTY(int formFactor READ formFactor WRITE setFormFactor NOTIFY formFactorChanged)
    Q_PROPERTY(QObject *configuration READ configuration CONSTANT)

public:
    explicit MockPlasmoidAttached(QObject *parent = nullptr)
        : QObject(parent)
        , m_config(new MockWeatherConfig(this))
    {}

    QString title() const { return m_title; }
    void setTitle(const QString &t) { if (m_title != t) { m_title = t; emit titleChanged(); } }
    int backgroundHints() const { return m_backgroundHints; }
    void setBackgroundHints(int h) { if (m_backgroundHints != h) { m_backgroundHints = h; emit backgroundHintsChanged(); } }
    int formFactor() const { return m_formFactor; }
    void setFormFactor(int f) { if (m_formFactor != f) { m_formFactor = f; emit formFactorChanged(); } }
    MockWeatherConfig *configObject() const { return m_config; }
    QObject *configuration() const { return m_config; }

signals:
    void titleChanged();
    void backgroundHintsChanged();
    void formFactorChanged();

private:
    QString m_title;
    int m_backgroundHints = MockPlasmaTypes::DefaultBackground;
    int m_formFactor = MockPlasmaTypes::Planar;
    MockWeatherConfig *m_config = nullptr;
};

class MockPlasmoid : public QObject
{
    Q_OBJECT
public:
    static MockPlasmoidAttached *qmlAttachedProperties(QObject *object)
    {
        return new MockPlasmoidAttached(object);
    }
};

class MockPlasmoidItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QQmlComponent *compactRepresentation READ compactRepresentation WRITE setCompactRepresentation NOTIFY compactRepresentationChanged)
    Q_PROPERTY(QQmlComponent *fullRepresentation READ fullRepresentation WRITE setFullRepresentation NOTIFY fullRepresentationChanged)
    Q_PROPERTY(QQmlComponent *preferredRepresentation READ preferredRepresentation WRITE setPreferredRepresentation NOTIFY preferredRepresentationChanged)
    Q_PROPERTY(bool expanded READ expanded WRITE setExpanded NOTIFY expandedChanged)

public:
    explicit MockPlasmoidItem(QQuickItem *parent = nullptr)
        : QQuickItem(parent)
    {}

    QQmlComponent *compactRepresentation() const { return m_compact; }
    void setCompactRepresentation(QQmlComponent *c) { if (m_compact != c) { m_compact = c; emit compactRepresentationChanged(); } }
    QQmlComponent *fullRepresentation() const { return m_full; }
    void setFullRepresentation(QQmlComponent *c) { if (m_full != c) { m_full = c; emit fullRepresentationChanged(); } }
    QQmlComponent *preferredRepresentation() const { return m_preferred; }
    void setPreferredRepresentation(QQmlComponent *c) { if (m_preferred != c) { m_preferred = c; emit preferredRepresentationChanged(); } }
    bool expanded() const { return m_expanded; }
    void setExpanded(bool e) { if (m_expanded != e) { m_expanded = e; emit expandedChanged(); } }

signals:
    void compactRepresentationChanged();
    void fullRepresentationChanged();
    void preferredRepresentationChanged();
    void expandedChanged();

private:
    QQmlComponent *m_compact = nullptr;
    QQmlComponent *m_full = nullptr;
    QQmlComponent *m_preferred = nullptr;
    bool m_expanded = false;
};

QQuickItem *findVisualItemByObjectName(QQuickItem *root, const QString &name)
{
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *found = findVisualItemByObjectName(child, name)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

QML_DECLARE_TYPEINFO(MockPlasmoid, QML_HAS_ATTACHED_PROPERTIES)

class WeatherQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_cacheHome;

private slots:
    void initTestCase()
    {
        // Keep the startup auto-refresh away from the real user cache directory
        QVERIFY(m_cacheHome.isValid());
        qputenv("XDG_CACHE_HOME", m_cacheHome.path().toUtf8());
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        qmlRegisterUncreatableType<MockPlasmaTypes>("org.kde.plasma.core", 2, 0, "Types", QStringLiteral("Enum holder"));
        qmlRegisterType<MockPlasmoidItem>("org.kde.plasma.plasmoid", 2, 0, "PlasmoidItem");
        qmlRegisterUncreatableType<MockPlasmoid>("org.kde.plasma.plasmoid", 2, 0, "Plasmoid", QStringLiteral("Attached"));
    }

    void metadataAndKConfigXmlSchemaMatchPlasma6Contract()
    {
        QFile metaFile(QStringLiteral(WEATHER_METADATA_JSON));
        QVERIFY(metaFile.open(QIODevice::ReadOnly));
        const QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
        QCOMPARE(meta.value(QStringLiteral("KPackageStructure")).toString(), QStringLiteral("Plasma/Applet"));
        QCOMPARE(meta.value(QStringLiteral("X-Plasma-API-Minimum-Version")).toString(), QStringLiteral("6.0"));
        const QJsonObject plugin = meta.value(QStringLiteral("KPlugin")).toObject();
        QCOMPARE(plugin.value(QStringLiteral("Id")).toString(), QStringLiteral("org.zethropol.plasma.weather"));
        QCOMPARE(plugin.value(QStringLiteral("Name")).toString(), QStringLiteral("Zethropol Hava Durumu"));

        QFile xmlFile(QStringLiteral(WEATHER_MAIN_XML));
        QVERIFY(xmlFile.open(QIODevice::ReadOnly));
        const QString xml = QString::fromUtf8(xmlFile.readAll());
        QVERIFY(xml.contains(QStringLiteral("name=\"locationName\"")));
        QVERIFY(xml.contains(QStringLiteral("<default>İstanbul</default>")));
        QVERIFY(xml.contains(QStringLiteral("name=\"forecastDays\"")));
        QVERIFY(xml.contains(QStringLiteral("<default>5</default>")));
        QVERIFY(xml.contains(QStringLiteral("name=\"desktopOpacity\"")));
        QVERIFY(xml.contains(QStringLiteral("<default>82</default>")));
        QVERIFY(xml.contains(QStringLiteral("name=\"popupOpacity\"")));
        QVERIFY(xml.contains(QStringLiteral("<default>92</default>")));
        QVERIFY(xml.contains(QStringLiteral("<min>15</min>")));

        QFile cfgQml(QStringLiteral(WEATHER_CONFIG_QML));
        QVERIFY(cfgQml.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(cfgQml.readAll()).contains(QStringLiteral("configGeneral.qml")));
    }

    void mainQmlLoadsPluginSwitchesFormFactorOpacityAndForecastDaysWithoutWarnings()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(WEATHER_QML_IMPORT_DIR));
        engine.addImportPath(QStringLiteral(WEATHER_TEST_STUB_IMPORT_DIR));

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent comp(&engine, QUrl::fromLocalFile(QStringLiteral(WEATHER_UI_DIR "/main.qml")));
        QVERIFY2(comp.isReady(), qPrintable(comp.errorString()));

        std::unique_ptr<QObject> obj(comp.create());
        QVERIFY2(obj != nullptr, qPrintable(comp.errorString()));
        auto *item = qobject_cast<MockPlasmoidItem *>(obj.get());
        QVERIFY(item != nullptr);

        auto *attached = qobject_cast<MockPlasmoidAttached *>(
            qmlAttachedPropertiesObject<MockPlasmoid>(item, false));
        QVERIFY(attached != nullptr);
        MockWeatherConfig *cfg = attached->configObject();
        QVERIFY(cfg != nullptr);

        // 1. Default Desktop (Planar) mode -> fullRepresentation + NoBackground + 82% desktop opacity + 5 days
        QCOMPARE(attached->backgroundHints(), static_cast<int>(MockPlasmaTypes::NoBackground));
        QCOMPARE(item->preferredRepresentation(), item->fullRepresentation());
        QCOMPARE(item->property("configuredForecastDays").toInt(), 5);
        QVERIFY(qAbs(item->property("activeSurfaceOpacity").toReal() - 0.82) < 1e-4);

        // 2. Panel (Horizontal) mode -> compactRepresentation + 92% popup opacity
        attached->setFormFactor(MockPlasmaTypes::Horizontal);
        QCOMPARE(item->preferredRepresentation(), item->compactRepresentation());
        QVERIFY(qAbs(item->property("activeSurfaceOpacity").toReal() - 0.92) < 1e-4);

        // 2b. plasmawindowed (Application) and Vertical panel: Application opens the full card directly,
        //     Vertical panel uses the compact pill.
        attached->setFormFactor(MockPlasmaTypes::Application);
        QCOMPARE(item->preferredRepresentation(), item->fullRepresentation());
        attached->setFormFactor(MockPlasmaTypes::Vertical);
        QCOMPARE(item->preferredRepresentation(), item->compactRepresentation());
        attached->setFormFactor(MockPlasmaTypes::MediaCenter);
        QCOMPARE(item->preferredRepresentation(), item->fullRepresentation());

        // 3. Opacity bounds clamping & low-opacity contrast protection in FullRepresentation
        attached->setFormFactor(MockPlasmaTypes::Planar);
        cfg->setDesktopOpacity(5); // Below 15% -> clamped to 15%
        QCOMPARE(item->property("configuredDesktopOpacityPercent").toInt(), 15);
        QVERIFY(qAbs(item->property("activeSurfaceOpacity").toReal() - 0.15) < 1e-4);

        cfg->setDesktopOpacity(140); // Above 100% -> clamped to 100%
        QCOMPARE(item->property("configuredDesktopOpacityPercent").toInt(), 100);

        cfg->setDesktopOpacity(20); // Low opacity -> triggers contrast guard on FullRepresentation
        std::unique_ptr<QObject> fullObj(item->fullRepresentation()->create(qmlContext(item)));
        QVERIFY2(fullObj != nullptr, qPrintable(item->fullRepresentation()->errorString()));
        auto *fullItem = qobject_cast<QQuickItem *>(fullObj.get());
        QVERIFY(fullItem != nullptr);
        QCOMPARE(fullItem->property("lowOpacityContrastGuard").toBool(), true);
        QCOMPARE(fullItem->property("forecastDays").toInt(), 5);

        // Selecting 3 / 7 days persists to Plasmoid.configuration.forecastDays
        QVERIFY(QMetaObject::invokeMethod(item, "selectForecastDays", Q_ARG(QVariant, 3)));
        QCOMPARE(cfg->forecastDays(), 3);
        QCOMPARE(fullItem->property("forecastDays").toInt(), 3);

        QVERIFY(QMetaObject::invokeMethod(item, "selectForecastDays", Q_ARG(QVariant, 7)));
        QCOMPARE(cfg->forecastDays(), 7);
        QCOMPARE(fullItem->property("forecastDays").toInt(), 7);

        // 4. CompactRepresentation instantiates cleanly and toggles expanded state
        std::unique_ptr<QObject> compactObj(item->compactRepresentation()->create(qmlContext(item)));
        QVERIFY2(compactObj != nullptr, qPrintable(item->compactRepresentation()->errorString()));
        auto *compactItem = qobject_cast<QQuickItem *>(compactObj.get());
        QVERIFY(compactItem != nullptr);
        QVERIFY(findVisualItemByObjectName(compactItem, QStringLiteral("compactTempLabel")) != nullptr);

        // 5. Regression (`main.qml:152 TypeError: Cannot call method 'refresh' of undefined`):
        //    `weatherClientRef` and both representations must reference the real WeatherClient
        //    instance, and emitting the refresh / toggle signals must not raise any JS error.
        QObject *srcClient = item->findChild<QObject *>(QStringLiteral("weatherClient"));
        QVERIFY(srcClient != nullptr);
        QVERIFY(item->property("weatherClientRef").value<QObject *>() == srcClient);
        QVERIFY(fullItem->property("weatherClient").value<QObject *>() == srcClient);
        QVERIFY(compactItem->property("weatherClient").value<QObject *>() == srcClient);
        QVERIFY(QMetaObject::invokeMethod(fullItem, "refreshRequested"));
        QVERIFY(QMetaObject::invokeMethod(compactItem, "toggleRequested"));
        QVERIFY(item->expanded());
        QVERIFY(QMetaObject::invokeMethod(fullItem, "selectForecastDaysRequested", Q_ARG(int, 5)));
        QCOMPARE(cfg->forecastDays(), 5);

        QString warnMsg;
        for (const QQmlError &e : warnings) {
            warnMsg += e.toString() + QLatin1Char('\n');
        }
        QVERIFY2(warnings.isEmpty(), qPrintable(warnMsg));
    }

    void mainQmlStartsLiveRefreshImmediatelyWithIstanbulDefaults()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(WEATHER_QML_IMPORT_DIR));
        engine.addImportPath(QStringLiteral(WEATHER_TEST_STUB_IMPORT_DIR));

        QQmlComponent comp(&engine, QUrl::fromLocalFile(QStringLiteral(WEATHER_UI_DIR "/main.qml")));
        QVERIFY2(comp.isReady(), qPrintable(comp.errorString()));
        std::unique_ptr<QObject> obj(comp.create());
        QVERIFY2(obj != nullptr, qPrintable(comp.errorString()));

        QObject *client = obj->findChild<QObject *>(QStringLiteral("weatherClient"));
        QVERIFY(client != nullptr);
        QVERIFY(qAbs(client->property("latitude").toDouble() - 41.0082) < 1e-6);
        QVERIFY(qAbs(client->property("longitude").toDouble() - 28.9784) < 1e-6);
        // Component.onCompleted -> syncConfigToClient(true) -> refresh(): an active request is in flight right away
        QCOMPARE(client->property("loading").toBool(), true);

        QSignalSpy doneSpy(client, SIGNAL(refreshCompleted(bool,bool)));
        QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() >= 1, 30000);
        const bool success = doneSpy.first().at(0).toBool();
        if (success) {
            QCOMPARE(client->property("hasData").toBool(), true);
            QVERIFY(client->property("dailyForecast").toList().size() == 7);
            QVERIFY(client->property("errorMessage").toString().isEmpty());
        } else {
            // Offline CI: a real error reason is reported instead of a silent placeholder
            QVERIFY(client->property("errorMessage").toString().startsWith(QStringLiteral("Bağlantı hatası:"))
                    || client->property("errorMessage").toString().startsWith(QStringLiteral("Veri hatası:")));
        }
    }

    void fullRepresentationSizesByForecastDaysAndShowsErrorWithRetry()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(WEATHER_QML_IMPORT_DIR));

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent stubComp(&engine);
        stubComp.setData(QByteArrayLiteral(
            "import QtQml\n"
            "QtObject {\n"
            "  property string themeMood: \"cloudy\"\n"
            "  property string locationName: \"İstanbul\"\n"
            "  property string locationAdmin: \"İstanbul, Türkiye\"\n"
            "  property bool offlineCache: false\n"
            "  property bool hasData: false\n"
            "  property string sourceLabel: \"Open-Meteo\"\n"
            "  property string offlineBadgeText: \"Çevrimdışı önbellek\"\n"
            "  property bool loading: false\n"
            "  property string conditionIcon: \"weather-none-available\"\n"
            "  property string conditionText: \"Veri bekleniyor\"\n"
            "  property real temperatureC: 0\n"
            "  property real apparentTemperatureC: 0\n"
            "  property int humidityPercent: 0\n"
            "  property real windSpeedKmh: 0\n"
            "  property string windDirectionText: \"K\"\n"
            "  property real pressureHpa: 0\n"
            "  property string lastUpdatedText: \"\"\n"
            "  property var hourlyForecast: []\n"
            "  property var visibleDailyForecast: []\n"
            "  property string errorMessage: \"Bağlantı hatası: Host bulunamadı\"\n"
            "}\n"), QUrl());
        QVERIFY2(stubComp.isReady(), qPrintable(stubComp.errorString()));
        std::unique_ptr<QObject> stub(stubComp.create());
        QVERIFY(stub != nullptr);

        QQmlComponent fullComp(&engine, QUrl::fromLocalFile(QStringLiteral(WEATHER_UI_DIR "/FullRepresentation.qml")));
        QVERIFY2(fullComp.isReady(), qPrintable(fullComp.errorString()));
        std::unique_ptr<QObject> fullObj(fullComp.createWithInitialProperties(
            {{QStringLiteral("weatherClient"), QVariant::fromValue(stub.get())},
             {QStringLiteral("forecastDays"), 3}}));
        QVERIFY2(fullObj != nullptr, qPrintable(fullComp.errorString()));
        auto *fullItem = qobject_cast<QQuickItem *>(fullObj.get());
        QVERIFY(fullItem != nullptr);

        // Dynamic, overflow-free card sizing: 3 < 5 < 7 rows
        const qreal min3 = fullItem->property("cardMinimumHeight").toReal();
        fullItem->setProperty("forecastDays", 5);
        const qreal min5 = fullItem->property("cardMinimumHeight").toReal();
        fullItem->setProperty("forecastDays", 7);
        const qreal min7 = fullItem->property("cardMinimumHeight").toReal();
        QVERIFY(min3 < min5);
        QVERIFY(min5 < min7);
        QCOMPARE(min5 - min3, 2 * (38.0 + 5.0));
        QCOMPARE(min7 - min5, 2 * (38.0 + 5.0));
        QVERIFY(fullItem->property("cardPreferredHeight").toReal() >= min7);
        QVERIFY(fullItem->property("cardMinimumWidth").toReal() >= 320);
        QVERIFY(fullItem->property("cardPreferredWidth").toReal() >= fullItem->property("cardMinimumWidth").toReal());

        // Status bar surfaces the real error reason with a "Tekrar Dene" action
        QQuickItem *statusBar = findVisualItemByObjectName(fullItem, QStringLiteral("weatherStatusBar"));
        QQuickItem *retry = findVisualItemByObjectName(fullItem, QStringLiteral("retryButton"));
        QQuickItem *statusLabel = findVisualItemByObjectName(fullItem, QStringLiteral("weatherStatusLabel"));
        QVERIFY(statusBar != nullptr);
        QVERIFY(retry != nullptr);
        QVERIFY(statusLabel != nullptr);
        QCOMPARE(statusLabel->property("text").toString(), QStringLiteral("Bağlantı hatası: Host bulunamadı"));
        QCOMPARE(retry->isVisible(), true);

        stub->setProperty("errorMessage", QString());
        QCOMPARE(retry->isVisible(), false);

        QString warnMsg;
        for (const QQmlError &e : warnings) {
            warnMsg += e.toString() + QLatin1Char('\n');
        }
        QVERIFY2(warnings.isEmpty(), qPrintable(warnMsg));
    }

    void configGeneralQmlExposesOpacitySlidersForecastSelectorAndTurkishSearch()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(WEATHER_QML_IMPORT_DIR));

        QList<QQmlError> warnings;
        QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                         [&warnings](const QList<QQmlError> &errs) { warnings.append(errs); });

        QQmlComponent comp(&engine, QUrl::fromLocalFile(QStringLiteral(WEATHER_UI_DIR "/configGeneral.qml")));
        QVERIFY2(comp.isReady(), qPrintable(comp.errorString()));

        std::unique_ptr<QObject> obj(comp.create());
        QVERIFY2(obj != nullptr, qPrintable(comp.errorString()));
        auto *page = qobject_cast<QQuickItem *>(obj.get());
        QVERIFY(page != nullptr);

        QCOMPARE(page->property("cfg_locationName").toString(), QStringLiteral("İstanbul"));
        QCOMPARE(page->property("cfg_forecastDays").toInt(), 5);
        QCOMPARE(page->property("cfg_desktopOpacity").toInt(), 82);
        QCOMPARE(page->property("cfg_popupOpacity").toInt(), 92);
        QCOMPARE(page->property("cfg_useCustomGlassBackground").toBool(), true);

        QQuickItem *deskSlider = findVisualItemByObjectName(page, QStringLiteral("desktopOpacitySlider"));
        QQuickItem *popSlider = findVisualItemByObjectName(page, QStringLiteral("popupOpacitySlider"));
        QVERIFY(deskSlider != nullptr);
        QVERIFY(popSlider != nullptr);
        QCOMPARE(deskSlider->property("from").toReal(), 15.0);
        QCOMPARE(deskSlider->property("to").toReal(), 100.0);
        QCOMPARE(popSlider->property("from").toReal(), 15.0);
        QCOMPARE(popSlider->property("to").toReal(), 100.0);

        QVERIFY(findVisualItemByObjectName(page, QStringLiteral("locationSearchField")) != nullptr);
        QVERIFY(findVisualItemByObjectName(page, QStringLiteral("configDaysRadio_3")) != nullptr);
        QVERIFY(findVisualItemByObjectName(page, QStringLiteral("configDaysRadio_5")) != nullptr);
        QVERIFY(findVisualItemByObjectName(page, QStringLiteral("configDaysRadio_7")) != nullptr);

        // Security audit: ensure zero dynamic code execution or shell process invocations in any QML file
        for (const QString &file : {
                 QStringLiteral(WEATHER_UI_DIR "/main.qml"),
                 QStringLiteral(WEATHER_UI_DIR "/CompactRepresentation.qml"),
                 QStringLiteral(WEATHER_UI_DIR "/FullRepresentation.qml"),
                 QStringLiteral(WEATHER_UI_DIR "/configGeneral.qml"),
             }) {
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString src = QString::fromUtf8(f.readAll());
            QVERIFY(!src.contains(QStringLiteral("Qt.createQmlObject")));
            QVERIFY(!src.contains(QStringLiteral("QProcess")));
            QVERIFY(!src.contains(QStringLiteral("eval(")));
        }

        QString warnMsg;
        for (const QQmlError &e : warnings) {
            warnMsg += e.toString() + QLatin1Char('\n');
        }
        QVERIFY2(warnings.isEmpty(), qPrintable(warnMsg));
    }
};

QTEST_MAIN(WeatherQmlTest)
#include "WeatherQmlTest.moc"
