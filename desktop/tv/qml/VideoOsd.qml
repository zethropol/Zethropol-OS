pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: osdRoot
    implicitHeight: 54
    radius: 14
    color: "#e60e1522"
    border.color: "#2b394f"
    border.width: 1

    property var player: null
    property bool sidebarOpen: true
    property bool epgPinned: true
    property bool isFullScreen: false
    property bool osdVisible: true
    readonly property bool hovered: osdHover.hovered

    signal requestToggleSidebar()
    signal requestToggleEpg()
    signal requestToggleFullScreen()
    signal requestOpenImport()
    signal requestPreviousChannel()
    signal requestNextChannel()
    signal userActivity()

    visible: opacity > 0.01
    opacity: osdVisible ? 1.0 : 0.0

    Behavior on opacity {
        NumberAnimation { duration: 180 }
    }

    HoverHandler {
        id: osdHover
        onHoveredChanged: {
            if (hovered) osdRoot.userActivity()
        }
    }

    component OsdBtn: Rectangle {
        id: btn
        property string labelText: ""
        property bool activeHighlight: false
        property string tipText: ""
        signal clicked()

        radius: 8
        implicitHeight: 34
        implicitWidth: Math.max(36, btnLabel.implicitWidth + 18)
        color: activeHighlight
            ? (btnMouse.containsMouse ? "#2596ff" : "#168cff")
            : (btnMouse.containsMouse ? "#233248" : "#151f2f")
        border.color: activeHighlight ? "#60b0ff" : (btnMouse.containsMouse ? "#168cff" : "#283549")
        border.width: 1

        Label {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.labelText
            color: btn.activeHighlight ? "#ffffff" : "#e2ecf8"
            font.pixelSize: 12
            font.bold: btn.activeHighlight
        }

        ToolTip.visible: btnMouse.containsMouse && btn.tipText.length > 0
        ToolTip.text: btn.tipText
        ToolTip.delay: 500

        MouseArea {
            id: btnMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                osdRoot.userActivity()
                btn.clicked()
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        OsdBtn {
            objectName: "osdPlayPauseButton"
            labelText: (osdRoot.player && osdRoot.player.playbackState === "playing") ? "⏸ Duraklat" : "▶ Oynat"
            activeHighlight: true
            tipText: "Oynat / Duraklat (Boşluk)"
            onClicked: if (osdRoot.player) osdRoot.player.togglePause()
        }

        OsdBtn {
            objectName: "osdStopButton"
            labelText: "■"
            tipText: "Yayını Durdur"
            onClicked: if (osdRoot.player) osdRoot.player.stop()
        }

        OsdBtn {
            objectName: "osdPrevChannelButton"
            labelText: "⟨ Önceki"
            tipText: "Önceki Kanal (Yukarı / PgUp)"
            onClicked: osdRoot.requestPreviousChannel()
        }

        OsdBtn {
            objectName: "osdNextChannelButton"
            labelText: "Sonraki ⟩"
            tipText: "Sonraki Kanal (Aşağı / PgDn)"
            onClicked: osdRoot.requestNextChannel()
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 24
            color: "#253145"
        }

        OsdBtn {
            objectName: "osdMuteButton"
            labelText: (osdRoot.player && osdRoot.player.muted) ? "🔇" : "🔊"
            tipText: (osdRoot.player && osdRoot.player.audioStatus.length > 0)
                ? ("Sessiz (M) • " + osdRoot.player.audioStatus)
                : "Sessiz (M)"
            onClicked: if (osdRoot.player) osdRoot.player.muted = !osdRoot.player.muted
        }

        Slider {
            id: volumeSlider
            objectName: "osdVolumeSlider"
            Layout.preferredWidth: 100
            from: 0
            to: 100
            stepSize: 1
            value: osdRoot.player ? osdRoot.player.volume : 80
            onMoved: {
                if (osdRoot.player) {
                    osdRoot.player.muted = false
                    osdRoot.player.volume = Math.round(value)
                }
                osdRoot.userActivity()
            }
        }

        Label {
            text: (osdRoot.player && osdRoot.player.muted)
                ? "0%"
                : ((osdRoot.player ? osdRoot.player.volume : 80) + "%")
            color: "#9aa7b8"
            font.pixelSize: 11
            Layout.preferredWidth: 32
        }

        Rectangle {
            objectName: "osdAudioStatusBadge"
            visible: osdRoot.player !== null
                     && osdRoot.player.playbackState === "playing"
                     && (!osdRoot.player.hasAudioTrack || osdRoot.player.audioOutputError)
            radius: 7
            implicitHeight: 24
            implicitWidth: audioBadgeLbl.implicitWidth + 14
            color: (osdRoot.player && osdRoot.player.audioOutputError) ? "#3b171c" : "#2f2511"
            border.color: (osdRoot.player && osdRoot.player.audioOutputError) ? "#ef4444" : "#f59e0b"
            border.width: 1

            Label {
                id: audioBadgeLbl
                anchors.centerIn: parent
                text: (osdRoot.player && osdRoot.player.audioOutputError) ? "Ses Hatası" : "Ses İzi Yok"
                color: (osdRoot.player && osdRoot.player.audioOutputError) ? "#fca5a5" : "#fcd34d"
                font.pixelSize: 10
                font.bold: true
            }

            ToolTip.visible: audioBadgeHover.hovered && osdRoot.player !== null && osdRoot.player.audioStatus.length > 0
            ToolTip.text: osdRoot.player ? osdRoot.player.audioStatus : ""
            ToolTip.delay: 250

            HoverHandler {
                id: audioBadgeHover
            }
        }

        Item { Layout.fillWidth: true }

        OsdBtn {
            objectName: "osdAspectButton"
            labelText: {
                var a = osdRoot.player ? osdRoot.player.aspectRatio : "auto"
                if (a === "auto") return "Oran: Oto"
                if (a === "fill") return "Oran: Doldur"
                return "Oran: " + a
            }
            tipText: "Görüntü en-boy oranını değiştir"
            onClicked: if (osdRoot.player) osdRoot.player.cycleAspectRatio()
        }

        OsdBtn {
            objectName: "osdImportButton"
            labelText: "+ M3U / EPG"
            tipText: "M3U kanal listesi veya XMLTV EPG ekle"
            onClicked: osdRoot.requestOpenImport()
        }

        OsdBtn {
            objectName: "osdEpgButton"
            labelText: "EPG"
            activeHighlight: osdRoot.epgPinned
            tipText: "Alt 2 saatlik yayın akışı şeridini sabitle / gizle (E)"
            onClicked: osdRoot.requestToggleEpg()
        }

        OsdBtn {
            objectName: "osdSidebarButton"
            labelText: osdRoot.sidebarOpen ? "Kanallar ⟩" : "⟨ Kanallar"
            activeHighlight: osdRoot.sidebarOpen
            tipText: "Sağ kanal listesini aç / daralt (C)"
            onClicked: osdRoot.requestToggleSidebar()
        }

        OsdBtn {
            objectName: "osdFullscreenButton"
            labelText: osdRoot.isFullScreen ? "⤡ Çık" : "⤢ Tam Ekran"
            tipText: "Tam Ekran (F / F11)"
            onClicked: osdRoot.requestToggleFullScreen()
        }
    }
}
