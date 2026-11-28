import QtQuick
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import org.zethropol.weather 1.0

PlasmoidItem {
    id: root
    objectName: "zethropolWeatherRoot"

    readonly property bool isPlanarDesktop: Plasmoid.formFactor === PlasmaCore.Types.Planar
    // Only genuine narrow panel / system-tray form factors use the compact pill.
    // Desktop (Planar) and plasmawindowed (Application) open the full Zethropol card directly.
    readonly property bool isPanelFormFactor: Plasmoid.formFactor === PlasmaCore.Types.Horizontal
        || Plasmoid.formFactor === PlasmaCore.Types.Vertical
    readonly property bool useCustomGlassBackground: {
        if (!Plasmoid.configuration || Plasmoid.configuration.useCustomGlassBackground === undefined)
            return true
        return Boolean(Plasmoid.configuration.useCustomGlassBackground)
    }

    Plasmoid.title: "Zethropol Hava Durumu — " + weatherSource.locationName
    Plasmoid.backgroundHints: root.useCustomGlassBackground
        ? PlasmaCore.Types.NoBackground
        : PlasmaCore.Types.DefaultBackground

    preferredRepresentation: root.isPanelFormFactor ? compactRepresentation : fullRepresentation

    readonly property int configuredForecastDays: {
        if (!Plasmoid.configuration)
            return 5
        var raw = Plasmoid.configuration.forecastDays
        if (raw === undefined || raw === null || raw === "")
            return 5
        return weatherSource.normalizeForecastDays(Math.round(Number(raw)))
    }

    readonly property int configuredDesktopOpacityPercent: {
        if (!Plasmoid.configuration)
            return 82
        var raw = Plasmoid.configuration.desktopOpacity
        if (raw === undefined || raw === null || raw === "")
            return 82
        var n = Number(raw)
        return Number.isFinite(n) ? weatherSource.clampOpacityPercent(Math.round(n)) : 82
    }

    readonly property int configuredPopupOpacityPercent: {
        if (!Plasmoid.configuration)
            return 92
        var raw = Plasmoid.configuration.popupOpacity
        if (raw === undefined || raw === null || raw === "")
            return 92
        var n = Number(raw)
        return Number.isFinite(n) ? weatherSource.clampOpacityPercent(Math.round(n)) : 92
    }

    readonly property real desktopOpacityFactor: root.configuredDesktopOpacityPercent / 100.0
    readonly property real popupOpacityFactor: root.configuredPopupOpacityPercent / 100.0
    readonly property real activeSurfaceOpacity: root.isPlanarDesktop
        ? root.desktopOpacityFactor
        : root.popupOpacityFactor

    readonly property int configuredRefreshMinutes: {
        if (!Plasmoid.configuration)
            return 15
        var raw = Number(Plasmoid.configuration.refreshIntervalMinutes)
        return Number.isFinite(raw) ? Math.max(5, Math.min(120, Math.round(raw))) : 15
    }

    function syncConfigToClient(triggerRefresh) {
        if (!Plasmoid.configuration)
            return
        var name = String(Plasmoid.configuration.locationName || "İstanbul")
        var admin = String(Plasmoid.configuration.locationAdmin || "İstanbul, Türkiye")
        var country = String(Plasmoid.configuration.countryCode || "TR")
        var lat = Number(Plasmoid.configuration.latitude)
        var lon = Number(Plasmoid.configuration.longitude)
        if (!Number.isFinite(lat) || !Number.isFinite(lon)) {
            lat = 41.0082
            lon = 28.9784
        }

        var locationChanged = (weatherSource.locationName !== name
                               || Math.abs(weatherSource.latitude - lat) > 0.0005
                               || Math.abs(weatherSource.longitude - lon) > 0.0005)
        weatherSource.configureLocation(name, admin, country, lat, lon)
        weatherSource.forecastDays = root.configuredForecastDays
        weatherSource.useMgmObservation = Plasmoid.configuration.useMgmObservation !== undefined
            ? Boolean(Plasmoid.configuration.useMgmObservation)
            : true

        if (triggerRefresh || locationChanged) {
            weatherSource.refresh()
        }
    }

    function selectForecastDays(days) {
        var normalized = weatherSource.normalizeForecastDays(days)
        weatherSource.forecastDays = normalized
        if (Plasmoid.configuration) {
            Plasmoid.configuration.forecastDays = normalized
        }
    }

    // NOTE: the id must NOT be `weatherClient`: the representations declare a `weatherClient`
    // property, and an unqualified `weatherClient` inside their instantiation scope would resolve
    // to that (still unset) property instead of this object (-> undefined / TypeError).
    readonly property alias weatherClientRef: weatherSource

    WeatherClient {
        id: weatherSource
        objectName: "weatherClient"
        forecastDays: root.configuredForecastDays
    }

    Connections {
        target: Plasmoid.configuration
        ignoreUnknownSignals: true
        function onLocationNameChanged() { root.syncConfigToClient(true) }
        function onLocationAdminChanged() { root.syncConfigToClient(false) }
        function onCountryCodeChanged() { root.syncConfigToClient(true) }
        function onLatitudeChanged() { root.syncConfigToClient(true) }
        function onLongitudeChanged() { root.syncConfigToClient(true) }
        function onForecastDaysChanged() { weatherSource.forecastDays = root.configuredForecastDays }
        function onUseMgmObservationChanged() { root.syncConfigToClient(true) }
    }

    Timer {
        id: autoRefreshTimer
        interval: root.configuredRefreshMinutes * 60 * 1000
        repeat: true
        running: true
        onTriggered: weatherSource.refresh()
    }

    Component.onCompleted: {
        weatherSource.loadCachedSnapshot()
        root.syncConfigToClient(true)
    }

    compactRepresentation: CompactRepresentation {
        objectName: "weatherCompactRepresentation"
        weatherClient: root.weatherClientRef
        onToggleRequested: root.expanded = !root.expanded
    }

    fullRepresentation: FullRepresentation {
        objectName: "weatherFullRepresentation"
        weatherClient: root.weatherClientRef
        isDesktopCard: !root.isPanelFormFactor
        surfaceOpacity: root.activeSurfaceOpacity
        forecastDays: root.configuredForecastDays
        onSelectForecastDaysRequested: function(days) {
            root.selectForecastDays(days)
        }
        onRefreshRequested: root.weatherClientRef.refresh()
    }
}
