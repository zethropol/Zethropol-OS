pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: epgBar
    implicitHeight: EpgBridge.hasEpgData ? 124 : 68
    color: "#eb0c121d"

    property bool epgPinned: true
    readonly property bool hovered: epgHover.hovered

    signal togglePinned()
    signal requestOpenImport()
    signal userActivity()

    HoverHandler {
        id: epgHover
        onHoveredChanged: {
            if (hovered) epgBar.userActivity()
        }
    }

    // Top separator
    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: "#223047"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        anchors.topMargin: 8
        anchors.bottomMargin: 10
        spacing: 6

        // Compact header strip
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                text: "YAYIN AKIŞI (2 SAATLİK PENCERE)"
                color: "#7c8da4"
                font.pixelSize: 10
                font.bold: true
                font.letterSpacing: 1.1
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                radius: 6
                color: addEpgMouse.containsMouse ? "#24344d" : "#151f2f"
                border.color: addEpgMouse.containsMouse ? "#168cff" : "#27354a"
                border.width: 1
                implicitWidth: addEpgLbl.implicitWidth + 14
                implicitHeight: 22

                Label {
                    id: addEpgLbl
                    anchors.centerIn: parent
                    text: "+ XMLTV EPG"
                    color: "#cbd5e1"
                    font.pixelSize: 10
                    font.bold: true
                }
                MouseArea {
                    id: addEpgMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        epgBar.userActivity()
                        epgBar.requestOpenImport()
                    }
                }
            }

            Rectangle {
                radius: 6
                color: epgBar.epgPinned ? "#1b365d" : (pinMouse.containsMouse ? "#24344d" : "#151f2f")
                border.color: epgBar.epgPinned ? "#168cff" : "#27354a"
                border.width: 1
                implicitWidth: pinLbl.implicitWidth + 14
                implicitHeight: 22

                Label {
                    id: pinLbl
                    anchors.centerIn: parent
                    text: epgBar.epgPinned ? "Sabit" : "Otomatik Gizle"
                    color: epgBar.epgPinned ? "#93c5fd" : "#cbd5e1"
                    font.pixelSize: 10
                    font.bold: true
                }
                MouseArea {
                    id: pinMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: epgBar.togglePinned()
                }
            }
        }

        // Graceful fallback when channel has no XMLTV EPG data
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !EpgBridge.hasEpgData
            spacing: 10

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: "#475569"
            }

            Label {
                objectName: "epgFallbackLabel"
                text: EpgBridge.fallbackMessage
                color: "#94a3b8"
                font.pixelSize: 13
            }

            Item { Layout.fillWidth: true }
        }

        // Active live EPG layout: Now Playing + Upcoming 2 Hours
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: EpgBridge.hasEpgData
            spacing: 12

            // Current Program ("Şu An Yayında")
            Rectangle {
                objectName: "epgNowCard"
                Layout.preferredWidth: Math.min(340, Math.max(250, epgBar.width * 0.34))
                Layout.fillHeight: true
                radius: 10
                color: "#132136"
                border.color: "#168cff"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Rectangle {
                            Layout.preferredWidth: 7
                            Layout.preferredHeight: 7
                            radius: 3.5
                            color: "#ef4444"
                        }

                        Label {
                            text: "ŞU AN YAYINDA"
                            color: "#60b0ff"
                            font.pixelSize: 10
                            font.bold: true
                        }

                        Item { Layout.fillWidth: true }

                        Label {
                            objectName: "epgCurrentTimeLabel"
                            text: EpgBridge.hasCurrentProgram ? EpgBridge.currentTimeRange : ""
                            color: "#94a3b8"
                            font.pixelSize: 11
                        }
                    }

                    Label {
                        objectName: "epgCurrentTitleLabel"
                        Layout.fillWidth: true
                        text: EpgBridge.hasCurrentProgram
                            ? EpgBridge.currentTitle
                            : EpgBridge.fallbackMessage
                        color: "#f0f6fc"
                        font.pixelSize: 13
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        visible: EpgBridge.hasCurrentProgram

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 5
                            radius: 2.5
                            color: "#1e2e47"

                            Rectangle {
                                objectName: "epgProgressFill"
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Math.max(4, parent.width * EpgBridge.currentProgress)
                                radius: 2.5
                                gradient: Gradient {
                                    orientation: Gradient.Horizontal
                                    GradientStop { position: 0.0; color: "#168cff" }
                                    GradientStop { position: 1.0; color: "#665cff" }
                                }
                            }
                        }

                        Label {
                            text: EpgBridge.currentRemainingMinutes + " dk kaldı"
                            color: "#8ab4f8"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }
            }

            // Upcoming 2-hour horizontal cards
            Flickable {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: upcomingRow.implicitWidth
                contentHeight: height
                clip: true
                flickableDirection: Flickable.HorizontalFlick

                Row {
                    id: upcomingRow
                    height: parent.height
                    spacing: 10

                    Repeater {
                        model: EpgBridge.upcomingPrograms

                        Rectangle {
                            id: upcomingCard
                            required property var modelData
                            width: 230
                            height: parent.height
                            radius: 10
                            color: "#111a29"
                            border.color: "#223047"
                            border.width: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: upcomingCard.modelData.timeRange
                                        color: "#60b0ff"
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        text: "+" + upcomingCard.modelData.startsInMinutes + " dk"
                                        color: "#7c8da4"
                                        font.pixelSize: 10
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: upcomingCard.modelData.title
                                    color: "#e2ecf8"
                                    font.pixelSize: 12
                                    font.bold: true
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: upcomingCard.modelData.category.length > 0
                                        ? (upcomingCard.modelData.category + " • " + upcomingCard.modelData.durationMinutes + " dk")
                                        : (upcomingCard.modelData.durationMinutes + " dk")
                                    color: "#7c8da4"
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    Rectangle {
                        visible: EpgBridge.upcomingPrograms.length === 0
                        width: 260
                        height: parent.height
                        radius: 10
                        color: "#101724"
                        border.color: "#1f2b3e"
                        border.width: 1

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 20
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: "Önümüzdeki 2 saat için ek program kaydı bulunmuyor."
                            color: "#7c8da4"
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }
}
