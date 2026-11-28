import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: fullRoot

    required property var weatherClient
    property bool isDesktopCard: false
    property real surfaceOpacity: 0.88
    property int forecastDays: 5

    signal selectForecastDaysRequested(int days)
    signal refreshRequested()

    readonly property real clampedOpacity: Math.max(0.15, Math.min(1.0, Number.isFinite(surfaceOpacity) ? surfaceOpacity : 0.88))
    readonly property bool lowOpacityContrastGuard: clampedOpacity < 0.55
    readonly property color textOutlineColor: Qt.rgba(4 / 255, 8 / 255, 16 / 255, lowOpacityContrastGuard ? 0.92 : 0.42)
    readonly property int textOutlineStyle: lowOpacityContrastGuard ? Text.Outline : Text.Normal
    readonly property string mood: weatherClient ? weatherClient.themeMood : "cloudy"

    readonly property color moodTopColor: {
        switch (mood) {
        case "clear-day": return Qt.rgba(29 / 255, 92 / 255, 198 / 255, Math.min(0.62, clampedOpacity * 0.58))
        case "clear-night": return Qt.rgba(35 / 255, 31 / 255, 94 / 255, Math.min(0.66, clampedOpacity * 0.62))
        case "rain": return Qt.rgba(14 / 255, 78 / 255, 118 / 255, Math.min(0.65, clampedOpacity * 0.60))
        case "snow": return Qt.rgba(37 / 255, 84 / 255, 134 / 255, Math.min(0.62, clampedOpacity * 0.58))
        case "storm": return Qt.rgba(58 / 255, 42 / 255, 128 / 255, Math.min(0.68, clampedOpacity * 0.64))
        case "fog": return Qt.rgba(58 / 255, 75 / 255, 98 / 255, Math.min(0.58, clampedOpacity * 0.55))
        default: return Qt.rgba(26 / 255, 48 / 255, 82 / 255, Math.min(0.58, clampedOpacity * 0.55))
        }
    }

    readonly property real weeklyMinTemp: {
        var list = weatherClient ? weatherClient.visibleDailyForecast : []
        if (!list || list.length === 0) return 0
        var mn = 999
        for (var i = 0; i < list.length; ++i) {
            var v = Number(list[i].tempMinC)
            if (Number.isFinite(v) && v < mn) mn = v
        }
        return mn === 999 ? 0 : mn
    }

    readonly property real weeklyMaxTemp: {
        var list = weatherClient ? weatherClient.visibleDailyForecast : []
        if (!list || list.length === 0) return 30
        var mx = -999
        for (var i = 0; i < list.length; ++i) {
            var v = Number(list[i].tempMaxC)
            if (Number.isFinite(v) && v > mx) mx = v
        }
        return mx === -999 ? 30 : Math.max(mx, weeklyMinTemp + 1)
    }

    // Deterministic, overflow-free sizing based on the selected 3 / 5 / 7 day row count.
    readonly property int dayRowCount: (forecastDays === 3 || forecastDays === 7) ? forecastDays : 5
    readonly property real dayRowHeight: 38
    readonly property real dayRowSpacing: 5
    readonly property real columnSpacing: 12
    readonly property real cardPadding: 32
    // header 30 + hero 64 + metrics 46 + 24h strip 100 + range selector 26 + status bar 34 + 6 column gaps
    readonly property real fixedSectionsHeight: 30 + 64 + 46 + 100 + 26 + 34 + 6 * columnSpacing
    readonly property real dailyBlockHeight: dayRowCount * dayRowHeight + (dayRowCount - 1) * dayRowSpacing
    readonly property real cardMinimumHeight: cardPadding + fixedSectionsHeight + dailyBlockHeight
    readonly property real measuredContentHeight: contentColumn.implicitHeight + cardPadding
    readonly property real cardPreferredHeight: Math.max(cardMinimumHeight, measuredContentHeight)
    readonly property real cardMinimumWidth: 340
    readonly property real cardPreferredWidth: 396

    implicitWidth: cardPreferredWidth
    implicitHeight: cardPreferredHeight
    Layout.minimumWidth: cardMinimumWidth
    Layout.minimumHeight: cardMinimumHeight
    Layout.preferredWidth: cardPreferredWidth
    Layout.preferredHeight: cardPreferredHeight

    // Main Zethropol Dark Glassmorphic Surface
    Rectangle {
        id: glassCard
        objectName: "weatherGlassCard"
        anchors.fill: parent
        radius: 20
        color: Qt.rgba(11 / 255, 17 / 255, 29 / 255, fullRoot.clampedOpacity)
        border.width: 1
        border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, fullRoot.lowOpacityContrastGuard ? 0.38 : 0.22)
        clip: true

        Behavior on color { ColorAnimation { duration: 180 } }

        // Dynamic atmospheric gradient wash
        Rectangle {
            anchors.fill: parent
            radius: glassCard.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: fullRoot.moodTopColor }
                GradientStop { position: 0.45; color: Qt.rgba(13 / 255, 20 / 255, 34 / 255, fullRoot.clampedOpacity * 0.22) }
                GradientStop { position: 1.0; color: Qt.rgba(8 / 255, 12 / 255, 22 / 255, fullRoot.clampedOpacity * 0.45) }
            }
        }

        // Top glass sheen edge
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            height: 1
            color: Qt.rgba(255 / 255, 255 / 255, 255 / 255, 0.16)
        }

        Flickable {
            anchors.fill: parent
            anchors.margins: 16
            contentWidth: width
            contentHeight: contentColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: contentColumn
                width: parent.width
                spacing: 12

                // 1. Header: Location + Offline/Source Pill + Refresh Button
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1

                        Controls.Label {
                            objectName: "locationNameLabel"
                            Layout.fillWidth: true
                            text: fullRoot.weatherClient ? fullRoot.weatherClient.locationName : "İstanbul"
                            color: "#f8fafc"
                            font.pixelSize: 18
                            font.bold: true
                            elide: Text.ElideRight
                            style: fullRoot.textOutlineStyle
                            styleColor: fullRoot.textOutlineColor
                        }

                        Controls.Label {
                            objectName: "locationAdminLabel"
                            Layout.fillWidth: true
                            text: fullRoot.weatherClient ? fullRoot.weatherClient.locationAdmin : "İstanbul, Türkiye"
                            color: "#a8bad0"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            style: fullRoot.textOutlineStyle
                            styleColor: fullRoot.textOutlineColor
                        }
                    }

                    // Offline cache or live data source badge
                    Rectangle {
                        objectName: "statusSourceBadge"
                        visible: Boolean(fullRoot.weatherClient && (fullRoot.weatherClient.offlineCache || fullRoot.weatherClient.hasData))
                        radius: 8
                        implicitHeight: 22
                        implicitWidth: badgeLbl.implicitWidth + 14
                        color: (fullRoot.weatherClient && fullRoot.weatherClient.offlineCache)
                            ? Qt.rgba(245 / 255, 158 / 255, 11 / 255, 0.22)
                            : Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.18)
                        border.width: 1
                        border.color: (fullRoot.weatherClient && fullRoot.weatherClient.offlineCache)
                            ? Qt.rgba(251 / 255, 191 / 255, 36 / 255, 0.55)
                            : Qt.rgba(56 / 255, 189 / 255, 248 / 255, 0.36)

                        Controls.Label {
                            id: badgeLbl
                            objectName: "statusSourceLabel"
                            anchors.centerIn: parent
                            text: (fullRoot.weatherClient && fullRoot.weatherClient.offlineCache)
                                ? fullRoot.weatherClient.offlineBadgeText
                                : (fullRoot.weatherClient ? fullRoot.weatherClient.sourceLabel : "Open-Meteo")
                            color: (fullRoot.weatherClient && fullRoot.weatherClient.offlineCache)
                                ? "#fcd34d"
                                : "#7dd3fc"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }

                    // Refresh button
                    Rectangle {
                        objectName: "refreshButton"
                        width: 30
                        height: 30
                        radius: 9
                        color: refreshMouse.containsMouse
                            ? Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.32)
                            : Qt.rgba(255 / 255, 255 / 255, 255 / 255, 0.08)
                        border.width: 1
                        border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.25)

                        Kirigami.Icon {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            source: "view-refresh"
                            color: "#e2e8f0"
                            RotationAnimator on rotation {
                                from: 0
                                to: 360
                                duration: 900
                                loops: Animation.Infinite
                                running: fullRoot.weatherClient ? fullRoot.weatherClient.loading : false
                            }
                        }

                        MouseArea {
                            id: refreshMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: fullRoot.refreshRequested()
                        }
                    }
                }

                // 2. Hero Current Weather Row
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Kirigami.Icon {
                        objectName: "heroWeatherIcon"
                        Layout.preferredWidth: 64
                        Layout.preferredHeight: 64
                        source: fullRoot.weatherClient ? fullRoot.weatherClient.conditionIcon : "weather-clear"
                        fallback: "weather-clouds"
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Controls.Label {
                            objectName: "heroTempLabel"
                            text: (fullRoot.weatherClient && fullRoot.weatherClient.hasData)
                                ? (Math.round(fullRoot.weatherClient.temperatureC) + "°C")
                                : "—°C"
                            color: "#ffffff"
                            font.pixelSize: 38
                            font.bold: true
                            style: Text.Outline
                            styleColor: fullRoot.textOutlineColor
                        }

                        Controls.Label {
                            objectName: "heroConditionLabel"
                            text: fullRoot.weatherClient ? fullRoot.weatherClient.conditionText : "Veri bekleniyor"
                            color: "#e2e8f0"
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            style: fullRoot.textOutlineStyle
                            styleColor: fullRoot.textOutlineColor
                        }

                        Controls.Label {
                            objectName: "heroFeelsLikeLabel"
                            visible: Boolean(fullRoot.weatherClient && fullRoot.weatherClient.hasData)
                            text: fullRoot.weatherClient
                                ? ("Hissedilen " + Math.round(fullRoot.weatherClient.apparentTemperatureC) + "°C")
                                : ""
                            color: "#94a3b8"
                            font.pixelSize: 11
                            style: fullRoot.textOutlineStyle
                            styleColor: fullRoot.textOutlineColor
                        }
                    }
                }

                // 3. Four Glass Metric Pills (Humidity, Wind, Pressure, Updated)
                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 6
                    rowSpacing: 6

                    Repeater {
                        model: [
                            {
                                title: "NEM",
                                val: (fullRoot.weatherClient && fullRoot.weatherClient.hasData)
                                    ? ("%" + fullRoot.weatherClient.humidityPercent)
                                    : "—"
                            },
                            {
                                title: "RÜZGAR",
                                val: (fullRoot.weatherClient && fullRoot.weatherClient.hasData)
                                    ? (Math.round(fullRoot.weatherClient.windSpeedKmh) + " km/sa " + fullRoot.weatherClient.windDirectionText)
                                    : "—"
                            },
                            {
                                title: "BASINÇ",
                                val: (fullRoot.weatherClient && fullRoot.weatherClient.hasData && fullRoot.weatherClient.pressureHpa > 0)
                                    ? (Math.round(fullRoot.weatherClient.pressureHpa) + " hPa")
                                    : "—"
                            },
                            {
                                title: "GÜNCELLEME",
                                val: (fullRoot.weatherClient && fullRoot.weatherClient.lastUpdatedText.length > 0)
                                    ? fullRoot.weatherClient.lastUpdatedText
                                    : "—"
                            }
                        ]

                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 46
                            radius: 10
                            color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, Math.max(0.28, fullRoot.clampedOpacity * 0.58))
                            border.width: 1
                            border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, fullRoot.lowOpacityContrastGuard ? 0.28 : 0.14)

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 1

                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData.title
                                    color: "#7c91ab"
                                    font.pixelSize: 9
                                    font.bold: true
                                    font.letterSpacing: 0.7
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData.val
                                    color: "#f1f5f9"
                                    font.pixelSize: 11
                                    font.bold: true
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    style: fullRoot.textOutlineStyle
                                    styleColor: fullRoot.textOutlineColor
                                }
                            }
                        }
                    }
                }

                // 4. 24-Hour Horizontal Strip
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    visible: Boolean(fullRoot.weatherClient && fullRoot.weatherClient.hourlyForecast.length > 0)

                    Controls.Label {
                        text: "24 SAATLİK AKIŞ"
                        color: "#7c91ab"
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 1.0
                        style: fullRoot.textOutlineStyle
                        styleColor: fullRoot.textOutlineColor
                    }

                    ListView {
                        objectName: "hourlyListView"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 82
                        orientation: ListView.Horizontal
                        spacing: 6
                        clip: true
                        model: fullRoot.weatherClient ? fullRoot.weatherClient.hourlyForecast : []

                        delegate: Rectangle {
                            required property var modelData
                            width: 56
                            height: 80
                            radius: 11
                            color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, Math.max(0.28, fullRoot.clampedOpacity * 0.54))
                            border.width: 1
                            border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, fullRoot.lowOpacityContrastGuard ? 0.26 : 0.12)

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 5
                                spacing: 2

                                Controls.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: modelData.timeLabel || ""
                                    color: "#94a3b8"
                                    font.pixelSize: 10
                                }

                                Kirigami.Icon {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    source: modelData.iconName || "weather-clouds"
                                    fallback: "weather-clouds"
                                }

                                Controls.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: Math.round(Number(modelData.temperatureC)) + "°"
                                    color: "#f8fafc"
                                    font.pixelSize: 12
                                    font.bold: true
                                    style: fullRoot.textOutlineStyle
                                    styleColor: fullRoot.textOutlineColor
                                }

                                Controls.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: Number(modelData.precipProbability) > 0
                                        ? ("%" + modelData.precipProbability)
                                        : " "
                                    color: "#38bdf8"
                                    font.pixelSize: 9
                                    font.bold: true
                                }
                            }
                        }
                    }
                }

                // 5. Compact 3 / 5 / 7 Day Forecast Switcher Header
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Controls.Label {
                        text: "GÜNLÜK TAHMİN"
                        color: "#7c91ab"
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 1.0
                        style: fullRoot.textOutlineStyle
                        styleColor: fullRoot.textOutlineColor
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        objectName: "forecastRangeSelector"
                        radius: 9
                        implicitHeight: 26
                        implicitWidth: rangeRow.implicitWidth + 6
                        color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, Math.max(0.40, fullRoot.clampedOpacity * 0.72))
                        border.width: 1
                        border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.22)

                        RowLayout {
                            id: rangeRow
                            anchors.centerIn: parent
                            spacing: 2

                            Repeater {
                                model: [3, 5, 7]

                                delegate: Rectangle {
                                    required property int modelData
                                    readonly property bool active: fullRoot.forecastDays === modelData
                                    objectName: "forecastDaysBtn_" + modelData
                                    radius: 7
                                    implicitHeight: 20
                                    implicitWidth: btnLbl.implicitWidth + 14
                                    color: active
                                        ? "#168cff"
                                        : (btnMouse.containsMouse ? Qt.rgba(255 / 255, 255 / 255, 255 / 255, 0.10) : "transparent")

                                    Behavior on color { ColorAnimation { duration: 120 } }

                                    Controls.Label {
                                        id: btnLbl
                                        anchors.centerIn: parent
                                        text: modelData + " Gün"
                                        color: active ? "#ffffff" : "#cbd5e1"
                                        font.pixelSize: 10
                                        font.bold: active
                                    }

                                    MouseArea {
                                        id: btnMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: fullRoot.selectForecastDaysRequested(modelData)
                                    }
                                }
                            }
                        }
                    }
                }

                // 6. Daily Forecast Cards (dynamically sliced to 3 / 5 / 7 days)
                ColumnLayout {
                    objectName: "dailyForecastColumn"
                    Layout.fillWidth: true
                    spacing: 5

                    Repeater {
                        objectName: "dailyForecastRepeater"
                        model: fullRoot.weatherClient ? fullRoot.weatherClient.visibleDailyForecast : []

                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            objectName: "dailyForecastRow_" + index
                            Layout.fillWidth: true
                            implicitHeight: 38
                            radius: 10
                            color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, Math.max(0.28, fullRoot.clampedOpacity * 0.56))
                            border.width: 1
                            border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, fullRoot.lowOpacityContrastGuard ? 0.28 : 0.13)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 8

                                Controls.Label {
                                    Layout.preferredWidth: 46
                                    text: modelData.dayName || ""
                                    color: index === 0 ? "#7dd3fc" : "#f1f5f9"
                                    font.pixelSize: 12
                                    font.bold: index === 0
                                    style: fullRoot.textOutlineStyle
                                    styleColor: fullRoot.textOutlineColor
                                }

                                Kirigami.Icon {
                                    Layout.preferredWidth: 22
                                    Layout.preferredHeight: 22
                                    source: modelData.iconName || "weather-clouds"
                                    fallback: "weather-clouds"
                                }

                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData.shortCondition || modelData.conditionText || ""
                                    color: "#cbd5e1"
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                    style: fullRoot.textOutlineStyle
                                    styleColor: fullRoot.textOutlineColor
                                }

                                Controls.Label {
                                    Layout.preferredWidth: 34
                                    text: Number(modelData.precipProbability) > 0
                                        ? ("%" + modelData.precipProbability)
                                        : "—"
                                    color: Number(modelData.precipProbability) > 0 ? "#38bdf8" : "#64748b"
                                    font.pixelSize: 10
                                    font.bold: Number(modelData.precipProbability) > 0
                                    horizontalAlignment: Text.AlignRight
                                }

                                // Visual min/max temperature range bar
                                Controls.Label {
                                    Layout.preferredWidth: 28
                                    text: Math.round(Number(modelData.tempMinC)) + "°"
                                    color: "#94a3b8"
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignRight
                                    style: fullRoot.textOutlineStyle
                                    styleColor: fullRoot.textOutlineColor
                                }

                                Rectangle {
                                    Layout.preferredWidth: 46
                                    Layout.preferredHeight: 5
                                    radius: 3
                                    color: Qt.rgba(255 / 255, 255 / 255, 255 / 255, 0.12)

                                    Rectangle {
                                        readonly property real span: Math.max(1, fullRoot.weeklyMaxTemp - fullRoot.weeklyMinTemp)
                                        readonly property real startRatio: Math.max(0, Math.min(1, (Number(modelData.tempMinC) - fullRoot.weeklyMinTemp) / span))
                                        readonly property real endRatio: Math.max(startRatio + 0.12, Math.min(1, (Number(modelData.tempMaxC) - fullRoot.weeklyMinTemp) / span))
                                        x: parent.width * startRatio
                                        width: Math.max(6, parent.width * (endRatio - startRatio))
                                        height: parent.height
                                        radius: 3
                                        gradient: Gradient {
                                            orientation: Gradient.Horizontal
                                            GradientStop { position: 0.0; color: "#38bdf8" }
                                            GradientStop { position: 1.0; color: "#f59e0b" }
                                        }
                                    }
                                }

                                Controls.Label {
                                    Layout.preferredWidth: 28
                                    text: Math.round(Number(modelData.tempMaxC)) + "°"
                                    color: "#ffffff"
                                    font.pixelSize: 12
                                    font.bold: true
                                    horizontalAlignment: Text.AlignRight
                                    style: fullRoot.textOutlineStyle
                                    styleColor: fullRoot.textOutlineColor
                                }
                            }
                        }
                    }
                }

                // 7. Bottom status bar: live error reason + "Tekrar Dene" (never a silent "—°C")
                Rectangle {
                    id: statusBar
                    objectName: "weatherStatusBar"
                    readonly property bool hasError: Boolean(fullRoot.weatherClient && fullRoot.weatherClient.errorMessage.length > 0)
                    readonly property bool isLoading: Boolean(fullRoot.weatherClient && fullRoot.weatherClient.loading)
                    readonly property string statusText: hasError
                        ? fullRoot.weatherClient.errorMessage
                        : (isLoading ? "Hava durumu verisi yükleniyor…"
                           : (fullRoot.weatherClient && fullRoot.weatherClient.hasData
                              ? ("Son güncelleme " + fullRoot.weatherClient.lastUpdatedText)
                              : "Veri bekleniyor"))
                    Layout.fillWidth: true
                    Layout.minimumHeight: 34
                    Layout.preferredHeight: Math.max(34, statusRow.implicitHeight + 12)
                    radius: 10
                    color: hasError ? Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.18) : Qt.rgba(15 / 255, 23 / 255, 42 / 255, Math.max(0.28, fullRoot.clampedOpacity * 0.50))
                    border.width: 1
                    border.color: hasError ? Qt.rgba(248 / 255, 113 / 255, 113 / 255, 0.45) : Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.14)

                    RowLayout {
                        id: statusRow
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 6
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        spacing: 8

                        Controls.Label {
                            objectName: "weatherStatusLabel"
                            Layout.fillWidth: true
                            text: statusBar.statusText
                            color: statusBar.hasError ? "#fecaca" : "#94a3b8"
                            font.pixelSize: 10
                            wrapMode: Text.WordWrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }

                        Rectangle {
                            objectName: "retryButton"
                            visible: statusBar.hasError
                            Layout.preferredWidth: retryLbl.implicitWidth + 18
                            Layout.preferredHeight: 22
                            radius: 7
                            color: retryMouse.containsMouse ? "#168cff" : Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.55)

                            Controls.Label {
                                id: retryLbl
                                anchors.centerIn: parent
                                text: "Tekrar Dene"
                                color: "#ffffff"
                                font.pixelSize: 10
                                font.bold: true
                            }

                            MouseArea {
                                id: retryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: fullRoot.refreshRequested()
                            }
                        }
                    }
                }
            }
        }
    }
}
