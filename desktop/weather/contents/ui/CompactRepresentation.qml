import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: compactRoot

    required property var weatherClient
    signal toggleRequested()

    implicitWidth: pillRow.implicitWidth + 16
    implicitHeight: Math.max(28, pillRow.implicitHeight + 6)
    Layout.minimumWidth: pillRow.implicitWidth + 12
    Layout.preferredWidth: pillRow.implicitWidth + 16
    Layout.minimumHeight: 24
    Layout.fillHeight: true

    Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        radius: 10
        color: compactMouse.containsMouse
            ? Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.20)
            : Qt.rgba(13 / 255, 19 / 255, 30 / 255, 0.35)
        border.width: 1
        border.color: compactMouse.containsMouse
            ? Qt.rgba(56 / 255, 189 / 255, 248 / 255, 0.55)
            : Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.16)

        Behavior on color { ColorAnimation { duration: 140 } }

        RowLayout {
            id: pillRow
            anchors.centerIn: parent
            spacing: 6

            Kirigami.Icon {
                objectName: "compactWeatherIcon"
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                source: compactRoot.weatherClient ? compactRoot.weatherClient.conditionIcon : "weather-clear"
                fallback: "weather-clouds"
            }

            Controls.Label {
                objectName: "compactTempLabel"
                text: (compactRoot.weatherClient && compactRoot.weatherClient.hasData)
                    ? (Math.round(compactRoot.weatherClient.temperatureC) + "°")
                    : "—°"
                color: "#f0f6fc"
                font.pixelSize: 13
                font.bold: true
                style: Text.Outline
                styleColor: Qt.rgba(4 / 255, 8 / 255, 16 / 255, 0.72)
            }

            Rectangle {
                visible: compactRoot.weatherClient ? compactRoot.weatherClient.offlineCache : false
                width: 6
                height: 6
                radius: 3
                color: "#f59e0b"
            }
        }
    }

    MouseArea {
        id: compactMouse
        objectName: "compactClickArea"
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: compactRoot.toggleRequested()
    }

    Controls.ToolTip.visible: compactMouse.containsMouse
    Controls.ToolTip.delay: 450
    Controls.ToolTip.text: {
        if (!compactRoot.weatherClient)
            return "Zethropol Hava Durumu"
        if (!compactRoot.weatherClient.hasData)
            return compactRoot.weatherClient.locationName + " — Veri yükleniyor..."
        var line = compactRoot.weatherClient.locationName + ": "
            + Math.round(compactRoot.weatherClient.temperatureC) + "°C, "
            + compactRoot.weatherClient.conditionText
            + " (Hissedilen " + Math.round(compactRoot.weatherClient.apparentTemperatureC) + "°C)"
        if (compactRoot.weatherClient.offlineCache)
            line += " • " + compactRoot.weatherClient.offlineBadgeText
        return line
    }
}
