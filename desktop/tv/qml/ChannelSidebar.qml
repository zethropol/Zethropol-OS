pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: sidebar
    color: "#0d131e"
    clip: true

    readonly property bool searchFocused: searchInput.activeFocus

    signal requestCollapse()
    signal requestOpenImport()

    // Left vertical divider
    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: 1
        color: "#222e42"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // Header
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Kanallar"
                color: "#f0f6fc"
                font.pixelSize: 17
                font.bold: true
            }

            Rectangle {
                radius: 9
                color: "#172234"
                border.color: "#2a3950"
                border.width: 1
                implicitWidth: countLbl.implicitWidth + 14
                implicitHeight: 22

                Label {
                    id: countLbl
                    anchors.centerIn: parent
                    text: ChannelBridge.filteredChannelCount + " / " + ChannelBridge.totalChannelCount
                    color: "#8ab4f8"
                    font.pixelSize: 11
                    font.bold: true
                }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                radius: 7
                color: addSrcMouse.containsMouse ? "#2596ff" : "#168cff"
                implicitWidth: addSrcLbl.implicitWidth + 14
                implicitHeight: 28

                Label {
                    id: addSrcLbl
                    anchors.centerIn: parent
                    text: "+ Kaynak"
                    color: "#ffffff"
                    font.pixelSize: 11
                    font.bold: true
                }

                MouseArea {
                    id: addSrcMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.requestOpenImport()
                }
            }

            Rectangle {
                radius: 7
                color: collapseMouse.containsMouse ? "#243248" : "#151e2e"
                border.color: "#2a384e"
                border.width: 1
                implicitWidth: 28
                implicitHeight: 28

                Label {
                    anchors.centerIn: parent
                    text: "⟩"
                    color: "#cbd5e1"
                    font.pixelSize: 13
                    font.bold: true
                }

                MouseArea {
                    id: collapseMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.requestCollapse()
                }
            }
        }

        // Fast channel search
        TextField {
            id: searchInput
            objectName: "channelSearchField"
            Layout.fillWidth: true
            implicitHeight: 36
            placeholderText: "Kanal ara (ad, kategori veya numara)..."
            placeholderTextColor: "#64748b"
            color: "#f0f6fc"
            font.pixelSize: 13
            leftPadding: 12
            rightPadding: clearSearchBtn.visible ? 30 : 12
            text: ChannelBridge.searchQuery
            onTextEdited: ChannelBridge.searchQuery = text

            background: Rectangle {
                radius: 9
                color: "#111927"
                border.color: searchInput.activeFocus ? "#168cff" : "#263348"
                border.width: 1
            }

            Rectangle {
                id: clearSearchBtn
                visible: searchInput.text.length > 0
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                radius: 11
                color: clearMouse.containsMouse ? "#29384e" : "transparent"

                Label {
                    anchors.centerIn: parent
                    text: "×"
                    color: "#94a3b8"
                    font.pixelSize: 14
                }

                MouseArea {
                    id: clearMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        searchInput.text = ""
                        ChannelBridge.searchQuery = ""
                    }
                }
            }
        }

        // Category & Favorite filter chips (overflow-safe horizontal strip with mouse wheel & ‹/› arrows)
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            spacing: 4

            Rectangle {
                id: catLeftBtn
                readonly property bool canScrollLeft: catFlickable.contentX > 2
                visible: catFlickable.contentWidth > catFlickable.width + 2
                Layout.preferredWidth: 22
                Layout.preferredHeight: 28
                radius: 7
                color: canScrollLeft
                    ? (catLeftMouse.containsMouse ? "#24344d" : "#141d2c")
                    : "#0f1623"
                border.color: canScrollLeft ? "#263348" : "#1a2333"
                border.width: 1
                opacity: canScrollLeft ? 1.0 : 0.45

                Label {
                    anchors.centerIn: parent
                    text: "‹"
                    color: "#cbd5e1"
                    font.pixelSize: 14
                    font.bold: true
                }

                MouseArea {
                    id: catLeftMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: catLeftBtn.canScrollLeft
                    cursorShape: catLeftBtn.canScrollLeft ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: catFlickable.scrollBy(-140)
                }
            }

            Flickable {
                id: catFlickable
                objectName: "categoryChipsFlickable"
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                contentWidth: Math.max(width, catRow.implicitWidth + 4)
                contentHeight: 34
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalFlick

                function scrollBy(delta) {
                    var maxX = Math.max(0, contentWidth - width)
                    contentX = Math.max(0, Math.min(maxX, contentX + delta))
                }

                Behavior on contentX {
                    NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                }

                WheelHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: function(wheel) {
                        var delta = wheel.angleDelta.x !== 0 ? -wheel.angleDelta.x : -wheel.angleDelta.y
                        catFlickable.scrollBy(delta * 0.8)
                    }
                }

                Row {
                    id: catRow
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 2
                    rightPadding: 2

                    Repeater {
                        model: ChannelBridge.categories

                        Rectangle {
                            id: chipItem
                            required property string modelData
                            readonly property bool isSelected: ChannelBridge.selectedCategory === chipItem.modelData
                            radius: 14
                            height: 28
                            width: chipLbl.implicitWidth + 20
                            color: chipItem.isSelected
                                ? "#168cff"
                                : (chipMouse.containsMouse ? "#1d2b40" : "#141d2c")
                            border.color: chipItem.isSelected
                                ? "#60b0ff"
                                : (chipMouse.containsMouse ? "#168cff" : "#263348")
                            border.width: 1

                            Label {
                                id: chipLbl
                                anchors.centerIn: parent
                                text: chipItem.modelData === "Favoriler" ? "★ Favoriler" : chipItem.modelData
                                color: chipItem.isSelected ? "#ffffff" : "#cbd5e1"
                                font.pixelSize: 11
                                font.bold: chipItem.isSelected
                            }

                            MouseArea {
                                id: chipMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: ChannelBridge.selectedCategory = chipItem.modelData
                            }
                        }
                    }
                }
            }

            Rectangle {
                id: catRightBtn
                readonly property bool canScrollRight: catFlickable.contentX < (catFlickable.contentWidth - catFlickable.width - 2)
                visible: catFlickable.contentWidth > catFlickable.width + 2
                Layout.preferredWidth: 22
                Layout.preferredHeight: 28
                radius: 7
                color: canScrollRight
                    ? (catRightMouse.containsMouse ? "#24344d" : "#141d2c")
                    : "#0f1623"
                border.color: canScrollRight ? "#263348" : "#1a2333"
                border.width: 1
                opacity: canScrollRight ? 1.0 : 0.45

                Label {
                    anchors.centerIn: parent
                    text: "›"
                    color: "#cbd5e1"
                    font.pixelSize: 14
                    font.bold: true
                }

                MouseArea {
                    id: catRightMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: catRightBtn.canScrollRight
                    cursorShape: catRightBtn.canScrollRight ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: catFlickable.scrollBy(140)
                }
            }
        }

        // Interactive single-click + hover channel list
        ListView {
            id: channelList
            objectName: "channelListView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 6
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: ChannelBridge

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            delegate: Rectangle {
                id: rowItem
                objectName: "channelRow_" + index
                width: channelList.width
                height: 62
                radius: 11

                required property int index
                required property string channelId
                required property int channelNumber
                required property string channelName
                required property string channelGroup
                required property string logoUrl
                required property string streamUrl
                required property string tvgId
                required property bool favorite
                required property bool active

                readonly property bool hovered: rowMouseArea.containsMouse

                color: active
                    ? (hovered ? "#1a3152" : "#142640")
                    : (hovered ? "#1a2638" : "#111826")
                border.color: active
                    ? "#168cff"
                    : (hovered ? "#80168cff" : "#1f2b3e")
                border.width: active ? 1.5 : 1

                Behavior on color { ColorAnimation { duration: 110 } }
                Behavior on border.color { ColorAnimation { duration: 110 } }

                // Active left accent indicator
                Rectangle {
                    visible: rowItem.active
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    width: 3
                    radius: 1.5
                    color: "#168cff"
                }

                // Mandatory full-row hover + single-click MouseArea (Qt.PointingHandCursor)
                MouseArea {
                    id: rowMouseArea
                    objectName: "channelRowMouseArea_" + rowItem.index
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: ChannelBridge.selectChannel(rowItem.index)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    spacing: 10

                    // Channel number
                    Label {
                        text: rowItem.channelNumber < 10
                            ? ("0" + rowItem.channelNumber)
                            : String(rowItem.channelNumber)
                        color: rowItem.active ? "#60b0ff" : "#64748b"
                        font.pixelSize: 12
                        font.bold: true
                        Layout.preferredWidth: 22
                        horizontalAlignment: Text.AlignHCenter
                    }

                    // Logo / Monogram avatar
                    Rectangle {
                        Layout.preferredWidth: 38
                        Layout.preferredHeight: 38
                        radius: 9
                        color: rowItem.active ? "#1c365c" : "#182233"
                        border.color: rowItem.active ? "#389dff" : "#26344a"
                        border.width: 1
                        clip: true

                        Image {
                            id: logoImg
                            anchors.fill: parent
                            anchors.margins: 4
                            source: rowItem.logoUrl
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            visible: status === Image.Ready && rowItem.logoUrl.length > 0
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: !logoImg.visible
                            text: rowItem.channelName.length > 0
                                ? rowItem.channelName.substring(0, 2).toUpperCase()
                                : "TV"
                            color: rowItem.active ? "#93c5fd" : "#94a3b8"
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }

                    // Name + group
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: rowItem.channelName
                            color: rowItem.active ? "#ffffff" : (rowItem.hovered ? "#f0f6fc" : "#d8e2ef")
                            font.pixelSize: 13
                            font.bold: rowItem.active || rowItem.hovered
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            spacing: 6
                            Rectangle {
                                visible: rowItem.active
                                Layout.preferredWidth: 6
                                Layout.preferredHeight: 6
                                radius: 3
                                color: "#ef4444"
                            }
                            Label {
                                Layout.fillWidth: true
                                text: rowItem.active
                                    ? ("Yayında • " + rowItem.channelGroup)
                                    : rowItem.channelGroup
                                color: rowItem.active ? "#7dd3fc" : "#7c8b9e"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // Favorite toggle button (layered above rowMouseArea)
                    Rectangle {
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        radius: 7
                        color: favMouse.containsMouse ? "#26364f" : "transparent"

                        Label {
                            anchors.centerIn: parent
                            text: rowItem.favorite ? "★" : "☆"
                            color: rowItem.favorite ? "#fbbf24" : (favMouse.containsMouse ? "#e2e8f0" : "#64748b")
                            font.pixelSize: 15
                        }

                        MouseArea {
                            id: favMouse
                            objectName: "channelFavMouseArea_" + rowItem.index
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: ChannelBridge.toggleFavorite(rowItem.channelId)
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 32
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: channelList.count === 0
                text: ChannelBridge.selectedCategory === "Favoriler"
                    ? "Henüz favori kanal eklenmedi."
                    : "Arama ölçütüne uyan kanal bulunamadı."
                color: "#64748b"
                font.pixelSize: 13
            }
        }

        // Subtle status footer
        Label {
            Layout.fillWidth: true
            visible: ChannelBridge.statusMessage.length > 0
            text: ChannelBridge.statusMessage
            color: "#7c8b9e"
            font.pixelSize: 11
            elide: Text.ElideRight
        }
    }
}
