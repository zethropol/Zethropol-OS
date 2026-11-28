import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.zethropol.weather 1.0

Item {
    id: configPage
    objectName: "weatherConfigGeneralPage"

    implicitWidth: Math.max(500, formLayout.implicitWidth + 24)
    implicitHeight: formLayout.implicitHeight + 36

    // KConfigXT configuration bindings (matched to contents/config/main.xml)
    property string cfg_locationName: "İstanbul"
    property string cfg_locationNameDefault: "İstanbul"
    property string cfg_locationAdmin: "İstanbul, Türkiye"
    property string cfg_locationAdminDefault: "İstanbul, Türkiye"
    property string cfg_countryCode: "TR"
    property string cfg_countryCodeDefault: "TR"
    property double cfg_latitude: 41.0082
    property double cfg_latitudeDefault: 41.0082
    property double cfg_longitude: 28.9784
    property double cfg_longitudeDefault: 28.9784

    property int cfg_forecastDays: 5
    property int cfg_forecastDaysDefault: 5

    property int cfg_desktopOpacity: 82
    property int cfg_desktopOpacityDefault: 82
    property int cfg_popupOpacity: 92
    property int cfg_popupOpacityDefault: 92
    property bool cfg_useCustomGlassBackground: true
    property bool cfg_useCustomGlassBackgroundDefault: true

    property bool cfg_useMgmObservation: true
    property bool cfg_useMgmObservationDefault: true
    property int cfg_refreshIntervalMinutes: 15
    property int cfg_refreshIntervalMinutesDefault: 15

    WeatherClient {
        id: searchClient
        objectName: "configSearchClient"
    }

    function applyLocationSelection(name, admin, countryCode, lat, lon) {
        configPage.cfg_locationName = name
        configPage.cfg_locationAdmin = admin
        configPage.cfg_countryCode = countryCode
        configPage.cfg_latitude = lat
        configPage.cfg_longitude = lon
        searchClient.configureLocation(name, admin, countryCode, lat, lon)
        searchClient.clearSearchResults()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: formLayout.implicitHeight + 28
        clip: true

        Kirigami.FormLayout {
            id: formLayout
            anchors.top: parent.top
            anchors.topMargin: 14
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 12

            // --- Konum (Türkiye İl / İlçe Arama) ---
            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: "Konum (Türkiye İl / İlçe)"
            }

            RowLayout {
                Kirigami.FormData.label: "İl veya ilçe ara:"
                Layout.fillWidth: true
                spacing: 6

                Controls.TextField {
                    id: locationSearchField
                    objectName: "locationSearchField"
                    Layout.fillWidth: true
                    placeholderText: "Örn. İstanbul, Kadıköy, Çankaya, Karşıyaka, Nilüfer..."
                    onAccepted: searchClient.searchLocations(text)
                }

                Controls.Button {
                    objectName: "locationSearchButton"
                    text: searchClient.searching ? "Aranıyor..." : "Ara"
                    enabled: !searchClient.searching && locationSearchField.text.trim().length >= 2
                    onClicked: searchClient.searchLocations(locationSearchField.text)
                }
            }

            // Quick Turkey city chips
            Flow {
                Kirigami.FormData.label: "Hızlı seçim:"
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: [
                        { name: "İstanbul", admin: "İstanbul, Türkiye", cc: "TR", lat: 41.0082, lon: 28.9784 },
                        { name: "Ankara", admin: "Ankara, Türkiye", cc: "TR", lat: 39.9334, lon: 32.8597 },
                        { name: "İzmir", admin: "İzmir, Türkiye", cc: "TR", lat: 38.4237, lon: 27.1428 },
                        { name: "Bursa", admin: "Bursa, Türkiye", cc: "TR", lat: 40.1885, lon: 29.0610 },
                        { name: "Antalya", admin: "Antalya, Türkiye", cc: "TR", lat: 36.8969, lon: 30.7133 }
                    ]

                    delegate: Controls.Button {
                        required property var modelData
                        text: modelData.name
                        onClicked: configPage.applyLocationSelection(
                            modelData.name, modelData.admin, modelData.cc, modelData.lat, modelData.lon)
                    }
                }
            }

            // Live search results
            ColumnLayout {
                Layout.fillWidth: true
                visible: searchClient.searchResults.length > 0 || searchClient.searchError.length > 0
                spacing: 4

                Controls.Label {
                    visible: searchClient.searchError.length > 0
                    text: searchClient.searchError
                    color: "#ef4444"
                    wrapMode: Text.WordWrap
                }

                ListView {
                    objectName: "locationSearchResultsList"
                    visible: searchClient.searchResults.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(180, searchClient.searchResults.length * 36)
                    clip: true
                    spacing: 3
                    model: searchClient.searchResults

                    delegate: Controls.ItemDelegate {
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 300
                        height: 34
                        text: modelData.displayLabel || modelData.name
                        onClicked: configPage.applyLocationSelection(
                            modelData.name,
                            modelData.admin,
                            modelData.countryCode,
                            Number(modelData.latitude),
                            Number(modelData.longitude))
                    }
                }
            }

            // Selected active location + quick test refresh button
            RowLayout {
                Kirigami.FormData.label: "Seçili konum:"
                Layout.fillWidth: true
                spacing: 8

                Controls.Label {
                    objectName: "selectedLocationSummary"
                    Layout.fillWidth: true
                    text: configPage.cfg_locationName
                        + (configPage.cfg_locationAdmin.length > 0 ? (" (" + configPage.cfg_locationAdmin + ")") : "")
                        + " [" + configPage.cfg_latitude.toFixed(2) + ", " + configPage.cfg_longitude.toFixed(2) + "]"
                    wrapMode: Text.WordWrap
                }

                Controls.Button {
                    objectName: "configRefreshNowButton"
                    text: searchClient.loading ? "Yenileniyor..." : "Yenile"
                    enabled: !searchClient.loading
                    onClicked: {
                        searchClient.configureLocation(
                            configPage.cfg_locationName,
                            configPage.cfg_locationAdmin,
                            configPage.cfg_countryCode,
                            configPage.cfg_latitude,
                            configPage.cfg_longitude)
                        searchClient.useMgmObservation = configPage.cfg_useMgmObservation
                        searchClient.refresh()
                    }
                }
            }

            // --- Tahmin Aralığı (3 / 5 / 7 Gün) ---
            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: "Tahmin ve Veri Kaynağı"
            }

            RowLayout {
                Kirigami.FormData.label: "Günlük tahmin aralığı:"
                spacing: 6

                Repeater {
                    model: [3, 5, 7]

                    delegate: Controls.RadioButton {
                        required property int modelData
                        objectName: "configDaysRadio_" + modelData
                        text: modelData + " Gün"
                        checked: configPage.cfg_forecastDays === modelData
                        onClicked: configPage.cfg_forecastDays = modelData
                    }
                }
            }

            Controls.CheckBox {
                objectName: "useMgmCheckBox"
                Kirigami.FormData.label: "MGM gözlem:"
                text: "Türkiye konumlarında açık MGM anlık gözlem verisiyle zenginleştir"
                checked: configPage.cfg_useMgmObservation
                onToggled: configPage.cfg_useMgmObservation = checked
            }

            Controls.SpinBox {
                objectName: "refreshIntervalSpinBox"
                Kirigami.FormData.label: "Yenileme aralığı (dk):"
                from: 5
                to: 120
                stepSize: 5
                value: configPage.cfg_refreshIntervalMinutes
                onValueModified: configPage.cfg_refreshIntervalMinutes = value
            }

            // --- Opaklık (Saydamlık) ve Zethropol Cam Görünümü ---
            Item {
                Kirigami.FormData.isSection: true
                Kirigami.FormData.label: "Opaklık ve Zethropol Cam Yüzeyi"
            }

            RowLayout {
                Kirigami.FormData.label: "Masaüstü kartı opaklığı:"
                Layout.fillWidth: true

                Controls.Slider {
                    id: desktopOpacitySlider
                    objectName: "desktopOpacitySlider"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 220
                    from: 15
                    to: 100
                    stepSize: 1
                    snapMode: Controls.Slider.SnapAlways
                    value: configPage.cfg_desktopOpacity
                    onMoved: configPage.cfg_desktopOpacity = Math.round(value)
                }

                Controls.Label {
                    objectName: "desktopOpacityValueLabel"
                    Layout.minimumWidth: 46
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(desktopOpacitySlider.value) + " %"
                }
            }

            RowLayout {
                Kirigami.FormData.label: "Açılır pencere opaklığı:"
                Layout.fillWidth: true

                Controls.Slider {
                    id: popupOpacitySlider
                    objectName: "popupOpacitySlider"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 220
                    from: 15
                    to: 100
                    stepSize: 1
                    snapMode: Controls.Slider.SnapAlways
                    value: configPage.cfg_popupOpacity
                    onMoved: configPage.cfg_popupOpacity = Math.round(value)
                }

                Controls.Label {
                    objectName: "popupOpacityValueLabel"
                    Layout.minimumWidth: 46
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(popupOpacitySlider.value) + " %"
                }
            }

            Controls.CheckBox {
                objectName: "customGlassBackgroundCheckBox"
                Kirigami.FormData.label: "Arka plan modu:"
                text: "Özel Zethropol cam kart arka planı kullan (Plasma çerçevesiz NoBackground)"
                checked: configPage.cfg_useCustomGlassBackground
                onToggled: configPage.cfg_useCustomGlassBackground = checked
            }

            // Live opacity contrast preview swatch
            Rectangle {
                Kirigami.FormData.label: "Canlı önizleme:"
                Layout.fillWidth: true
                implicitHeight: 54
                radius: 12
                color: Qt.rgba(11 / 255, 17 / 255, 29 / 255, configPage.cfg_desktopOpacity / 100.0)
                border.width: 1
                border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, configPage.cfg_desktopOpacity < 55 ? 0.38 : 0.22)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    Controls.Label {
                        text: configPage.cfg_locationName + " • 18°C"
                        color: "#ffffff"
                        font.pixelSize: 14
                        font.bold: true
                        style: configPage.cfg_desktopOpacity < 55 ? Text.Outline : Text.Normal
                        styleColor: Qt.rgba(4 / 255, 8 / 255, 16 / 255, 0.90)
                    }

                    Item { Layout.fillWidth: true }

                    Controls.Label {
                        text: "Masaüstü %" + configPage.cfg_desktopOpacity
                            + " · Pencere %" + configPage.cfg_popupOpacity
                            + " · " + configPage.cfg_forecastDays + " Gün"
                        color: "#cbd5e1"
                        font.pixelSize: 11
                        style: configPage.cfg_desktopOpacity < 55 ? Text.Outline : Text.Normal
                        styleColor: Qt.rgba(4 / 255, 8 / 255, 16 / 255, 0.85)
                    }
                }
            }
        }
    }
}
