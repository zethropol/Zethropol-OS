import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import Zethropol.TV 1.0

ApplicationWindow {
    id: root
    objectName: "zethropolTvWindow"
    visible: true
    width: 1360
    height: 780
    minimumWidth: 920
    minimumHeight: 540
    title: "Zethropol TV"
    color: "#080c12"

    property bool sidebarOpen: true
    property bool epgPinned: true
    property bool osdActive: true
    property bool isFullScreen: root.visibility === Window.FullScreen
    property bool lastResolvedFromPage: false
    property bool pageFallbackTriedForChannel: false

    function revealOsd() {
        root.osdActive = true
        osdHideTimer.restart()
    }

    function toggleFullScreen() {
        if (root.visibility === Window.FullScreen) {
            root.showNormal()
        } else {
            root.showFullScreen()
        }
        revealOsd()
    }

    Timer {
        id: osdHideTimer
        interval: 3500
        repeat: false
        running: true
        onTriggered: {
            if (!videoOsd.hovered && !epgBar.hovered && !importDialog.opened
                    && mpvPlayer.playbackState === "playing") {
                root.osdActive = false
            }
        }
    }

    Connections {
        target: ChannelBridge
        function onChannelSelected(streamUrl, name, tvgId) {
            root.lastResolvedFromPage = false
            root.pageFallbackTriedForChannel = false
            EpgBridge.setActiveChannel(tvgId, name)
            StreamResolverBridge.resolveChannel(
                streamUrl,
                ChannelBridge.currentChannelSourcePageUrl,
                name,
                false,
                ChannelBridge.currentChannelFallbackUrls)
            root.revealOsd()
        }
    }

    Connections {
        target: StreamResolverBridge
        function onResolvingChanged() {
            if (StreamResolverBridge.resolving) {
                mpvPlayer.setResolvingState(StreamResolverBridge.statusMessage)
            }
        }
        function onStreamResolvedWithHeaders(resolvedUrl, channelName, fromWebPage, httpReferer, httpUserAgent) {
            root.lastResolvedFromPage = fromWebPage
            mpvPlayer.playUrlWithHeaders(resolvedUrl, httpReferer, httpUserAgent)
            root.revealOsd()
        }
        function onResolveFailed(errorMessage, channelName, drmDetected) {
            mpvPlayer.setExternalError(errorMessage)
            root.revealOsd()
        }
    }

    // Automatic failover across the channel's remaining verified backup URLs whenever
    // the active stream hits a playback error (e.g., 403/404 or expired token).
    Connections {
        target: mpvPlayer
        function onPlaybackStateChanged() {
            if (mpvPlayer.playbackState === "error"
                    && !StreamResolverBridge.resolving) {
                if (StreamResolverBridge.hasPendingFallbacks) {
                    root.pageFallbackTriedForChannel = true
                    StreamResolverBridge.tryNextFallback(ChannelBridge.currentChannelName)
                } else if (!mpvPlayer.retrying && StreamResolverBridge.lastResolvedWasYoutube) {
                    const ytHint = StreamResolverBridge.youtubeDiagnosticMessage()
                    if (ytHint.length > 0 && mpvPlayer.errorMessage !== ytHint) {
                        mpvPlayer.setExternalError(ytHint)
                    }
                }
            }
        }
    }

    // Keyboard shortcuts (safe while typing in search field)
    Shortcut {
        sequences: ["F", "F11"]
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: root.toggleFullScreen()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (importDialog.opened) {
                importDialog.close()
            } else if (root.isFullScreen) {
                root.showNormal()
            }
        }
    }
    Shortcut {
        sequence: "Space"
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: {
            mpvPlayer.togglePause()
            root.revealOsd()
        }
    }
    Shortcut {
        sequence: "M"
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: {
            mpvPlayer.muted = !mpvPlayer.muted
            root.revealOsd()
        }
    }
    Shortcut {
        sequences: ["Up", "PgUp"]
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: ChannelBridge.previousChannel()
    }
    Shortcut {
        sequences: ["Down", "PgDown"]
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: ChannelBridge.nextChannel()
    }
    Shortcut {
        sequence: "C"
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: {
            root.sidebarOpen = !root.sidebarOpen
            root.revealOsd()
        }
    }
    Shortcut {
        sequence: "E"
        enabled: !channelSidebar.searchFocused && !importDialog.opened
        onActivated: {
            root.epgPinned = !root.epgPinned
            root.revealOsd()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Left / Center: Full-focus live TV stage
        Item {
            id: videoStage
            objectName: "videoStage"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                anchors.fill: parent
                color: "#06090f"
            }

            MpvItem {
                id: mpvPlayer
                objectName: "tvMpvPlayer"
                anchors.fill: parent
            }

            // Mouse movement wakes OSD; double-click toggles fullscreen
            MouseArea {
                id: stageMouseArea
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onPositionChanged: root.revealOsd()
                onClicked: root.revealOsd()
                onDoubleClicked: root.toggleFullScreen()
            }

            // Top channel info header (auto-hides with OSD)
            Rectangle {
                id: topBanner
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 72
                visible: opacity > 0.01
                opacity: (root.osdActive || mpvPlayer.playbackState !== "playing") ? 1.0 : 0.0
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#d9080c12" }
                    GradientStop { position: 1.0; color: "#00080c12" }
                }

                Behavior on opacity {
                    NumberAnimation { duration: 180 }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 22
                    anchors.rightMargin: 22
                    spacing: 14

                    Rectangle {
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 32
                        radius: 8
                        color: "#16243b"
                        border.color: "#168cff"
                        border.width: 1
                        visible: ChannelBridge.currentChannelNumber > 0

                        Label {
                            anchors.centerIn: parent
                            text: ChannelBridge.currentChannelNumber < 10
                                ? ("0" + ChannelBridge.currentChannelNumber)
                                : String(ChannelBridge.currentChannelNumber)
                            color: "#e9f3ff"
                            font.pixelSize: 13
                            font.bold: true
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            spacing: 10
                            Label {
                                objectName: "activeChannelTitleLabel"
                                text: ChannelBridge.currentChannelName.length > 0
                                    ? ChannelBridge.currentChannelName
                                    : "Zethropol TV"
                                color: "#f0f6fc"
                                font.pixelSize: 18
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.maximumWidth: Math.max(240, videoStage.width - 360)
                            }

                            Rectangle {
                                visible: mpvPlayer.playbackState === "playing"
                                radius: 10
                                color: "#33ef4444"
                                border.color: "#ef4444"
                                border.width: 1
                                implicitWidth: liveRow.implicitWidth + 14
                                implicitHeight: 22

                                Row {
                                    id: liveRow
                                    anchors.centerIn: parent
                                    spacing: 6
                                    Rectangle {
                                        width: 7
                                        height: 7
                                        radius: 3.5
                                        color: "#ef4444"
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Label {
                                        text: "CANLI"
                                        color: "#fecaca"
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }
                            }
                        }

                        Label {
                            text: ChannelBridge.currentChannelGroup.length > 0
                                ? ChannelBridge.currentChannelGroup
                                : "Sağdaki listeden bir kanal seçin veya M3U kaynağı ekleyin"
                            color: "#8d98a5"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }

                    // Quick sidebar reopen button when collapsed
                    Rectangle {
                        visible: !root.sidebarOpen
                        radius: 8
                        color: reopenMouse.containsMouse ? "#24344d" : "#cc111827"
                        border.color: reopenMouse.containsMouse ? "#168cff" : "#303740"
                        border.width: 1
                        implicitWidth: reopenLabel.implicitWidth + 24
                        implicitHeight: 34

                        Label {
                            id: reopenLabel
                            anchors.centerIn: parent
                            text: "Kanallar (C)"
                            color: "#e9f3ff"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            id: reopenMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.sidebarOpen = true
                                root.revealOsd()
                            }
                        }
                    }
                }
            }

            // Loading / Buffering / Error non-blocking status overlay
            Rectangle {
                id: statusOverlay
                objectName: "streamStatusOverlay"
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, 480)
                implicitHeight: statusCol.implicitHeight + 36
                radius: 16
                color: "#e60d1420"
                border.color: mpvPlayer.playbackState === "error" ? "#ef4444" : "#2b3b54"
                border.width: 1
                visible: mpvPlayer.playbackState === "resolving"
                         || mpvPlayer.playbackState === "loading"
                         || mpvPlayer.playbackState === "buffering"
                         || mpvPlayer.playbackState === "error"
                         || StreamResolverBridge.resolving
                         || (mpvPlayer.playbackState === "idle" && ChannelBridge.currentChannelId.length === 0)

                ColumnLayout {
                    id: statusCol
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: {
                            if (StreamResolverBridge.resolving || mpvPlayer.playbackState === "resolving")
                                return "Resmi Canlı Akış Çözümleniyor..."
                            if (mpvPlayer.playbackState === "error")
                                return "Yayın Şu Anda Alınamıyor"
                            if (mpvPlayer.playbackState === "buffering")
                                return "Ara Belleğe Alınıyor..."
                            if (mpvPlayer.playbackState === "loading")
                                return "Canlı Yayına Bağlanılıyor..."
                            return "Zethropol TV Hazır"
                        }
                        color: mpvPlayer.playbackState === "error" ? "#fca5a5" : "#e9f3ff"
                        font.pixelSize: 16
                        font.bold: true
                    }

                    Label {
                        objectName: "streamErrorDetailLabel"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: "#9aa7b8"
                        font.pixelSize: 13
                        text: {
                            if (StreamResolverBridge.resolving || mpvPlayer.playbackState === "resolving") {
                                return StreamResolverBridge.statusMessage.length > 0
                                    ? StreamResolverBridge.statusMessage
                                    : ChannelBridge.currentChannelName
                            }
                            if (mpvPlayer.playbackState === "error") {
                                return mpvPlayer.errorMessage.length > 0
                                    ? mpvPlayer.errorMessage
                                    : "Yayın şu anda alınamıyor / Yeniden deneniyor..."
                            }
                            if (mpvPlayer.playbackState === "loading" || mpvPlayer.playbackState === "buffering") {
                                return ChannelBridge.currentChannelName
                            }
                            return "Sağdaki kanal listesinden tek tıklamayla bir yayın başlatın."
                        }
                    }

                    BusyIndicator {
                        Layout.alignment: Qt.AlignHCenter
                        running: StreamResolverBridge.resolving
                                 || mpvPlayer.playbackState === "resolving"
                                 || mpvPlayer.playbackState === "loading"
                                 || mpvPlayer.playbackState === "buffering"
                                 || mpvPlayer.retrying
                        visible: running
                        implicitWidth: 32
                        implicitHeight: 32
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 10
                        visible: mpvPlayer.playbackState === "error" && !StreamResolverBridge.resolving

                        Rectangle {
                            radius: 8
                            color: retryMouse.containsMouse ? "#2596ff" : "#168cff"
                            implicitWidth: retryLbl.implicitWidth + 24
                            implicitHeight: 34

                            Label {
                                id: retryLbl
                                anchors.centerIn: parent
                                text: "Tekrar Dene"
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            MouseArea {
                                id: retryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (ChannelBridge.currentChannelStreamUrl.length > 0) {
                                        mpvPlayer.retry()
                                    } else {
                                        StreamResolverBridge.resolveChannel(
                                            "",
                                            ChannelBridge.currentChannelSourcePageUrl,
                                            ChannelBridge.currentChannelName,
                                            true,
                                            ChannelBridge.currentChannelFallbackUrls)
                                    }
                                }
                            }
                        }

                        Rectangle {
                            visible: ChannelBridge.currentChannelSourcePageUrl.length > 0
                                     && ChannelBridge.currentChannelStreamUrl.length > 0
                            radius: 8
                            color: pageResolveMouse.containsMouse ? "#25344d" : "#182336"
                            border.color: "#2b4670"
                            border.width: 1
                            implicitWidth: pageResolveLbl.implicitWidth + 20
                            implicitHeight: 34

                            Label {
                                id: pageResolveLbl
                                anchors.centerIn: parent
                                text: "Sayfadan Çöz"
                                color: "#93c5fd"
                                font.pixelSize: 12
                                font.bold: true
                            }
                            MouseArea {
                                id: pageResolveMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: StreamResolverBridge.resolveChannel(
                                    "",
                                    ChannelBridge.currentChannelSourcePageUrl,
                                    ChannelBridge.currentChannelName,
                                    true,
                                    ChannelBridge.currentChannelFallbackUrls)
                            }
                        }

                        Rectangle {
                            radius: 8
                            color: nextErrMouse.containsMouse ? "#253142" : "#182130"
                            border.color: "#334155"
                            border.width: 1
                            implicitWidth: nextErrLbl.implicitWidth + 22
                            implicitHeight: 34

                            Label {
                                id: nextErrLbl
                                anchors.centerIn: parent
                                text: "Sonraki Kanal"
                                color: "#e2e8f0"
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: nextErrMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: ChannelBridge.nextChannel()
                            }
                        }
                    }
                }
            }

            // Bottom stack: Auto-hide OSD bar + 2-hour live EPG timeline strip
            ColumnLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                spacing: 0

                VideoOsd {
                    id: videoOsd
                    objectName: "videoOsdBar"
                    Layout.fillWidth: true
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: epgBar.visible ? 8 : 14
                    player: mpvPlayer
                    sidebarOpen: root.sidebarOpen
                    epgPinned: root.epgPinned
                    isFullScreen: root.isFullScreen
                    osdVisible: root.osdActive || mpvPlayer.playbackState !== "playing"

                    onRequestToggleSidebar: {
                        root.sidebarOpen = !root.sidebarOpen
                        root.revealOsd()
                    }
                    onRequestToggleEpg: {
                        root.epgPinned = !root.epgPinned
                        root.revealOsd()
                    }
                    onRequestToggleFullScreen: root.toggleFullScreen()
                    onRequestOpenImport: importDialog.open()
                    onRequestPreviousChannel: ChannelBridge.previousChannel()
                    onRequestNextChannel: ChannelBridge.nextChannel()
                    onUserActivity: root.revealOsd()
                }

                EpgTimelineBar {
                    id: epgBar
                    objectName: "epgTimelineBar"
                    Layout.fillWidth: true
                    epgPinned: root.epgPinned
                    visible: root.epgPinned || root.osdActive
                    onTogglePinned: {
                        root.epgPinned = !root.epgPinned
                        root.revealOsd()
                    }
                    onRequestOpenImport: importDialog.open()
                    onUserActivity: root.revealOsd()
                }
            }
        }

        // Right Panel: Collapsible Interactive Channel List
        ChannelSidebar {
            id: channelSidebar
            objectName: "channelSidebar"
            Layout.fillHeight: true
            Layout.preferredWidth: root.sidebarOpen ? 340 : 0
            visible: Layout.preferredWidth > 0

            Behavior on Layout.preferredWidth {
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            onRequestCollapse: root.sidebarOpen = false
            onRequestOpenImport: importDialog.open()
        }
    }

    SourceImportDialog {
        id: importDialog
        objectName: "sourceImportDialog"
        anchors.centerIn: Overlay.overlay
    }
}
