import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

Controls.ApplicationWindow {
    id: win
    objectName: "zciChatMainWindow"
    visible: true
    width: 1020
    height: 700
    minimumWidth: 780
    minimumHeight: 540
    title: "Zethropol ZCI Chatbot"
    color: "#0b111d"

    readonly property bool bridgeReady: Boolean(typeof ChatBridge !== "undefined" && ChatBridge !== null)

    // Atmospheric Zethropol dark glass backdrop
    Rectangle {
        anchors.fill: parent
        color: "#0b111d"

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 220
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.16) }
                GradientStop { position: 0.6; color: Qt.rgba(14 / 255, 32 / 255, 62 / 255, 0.08) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        // 1. Top Glass Status & Security Badges Bar
        Rectangle {
            objectName: "zciChatHeaderBar"
            Layout.fillWidth: true
            implicitHeight: 62
            radius: 16
            color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, 0.86)
            border.width: 1
            border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.20)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 10

                ColumnLayout {
                    spacing: 2
                    Controls.Label {
                        objectName: "appTitleLabel"
                        text: "Zethropol ZCI Chatbot"
                        color: "#f8fafc"
                        font.pixelSize: 16
                        font.bold: true
                    }
                    Controls.Label {
                        objectName: "statusBannerLabel"
                        text: win.bridgeReady ? ChatBridge.statusBanner : "Yerel D-Bus bağlantısı bekleniyor"
                        color: "#94a3b8"
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                Item { Layout.fillWidth: true }

                // Runtime state badge (Standby / Active)
                Rectangle {
                    objectName: "runtimeStateBadge"
                    radius: 9
                    implicitHeight: 26
                    implicitWidth: runtimeBadgeLbl.implicitWidth + 16
                    color: (win.bridgeReady && ChatBridge.runtimeState === "Active")
                        ? Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.25)
                        : Qt.rgba(34 / 255, 197 / 255, 94 / 255, 0.18)
                    border.width: 1
                    border.color: (win.bridgeReady && ChatBridge.runtimeState === "Active")
                        ? Qt.rgba(56 / 255, 189 / 255, 248 / 255, 0.55)
                        : Qt.rgba(74 / 255, 222 / 255, 128 / 255, 0.45)

                    Controls.Label {
                        id: runtimeBadgeLbl
                        objectName: "runtimeStateLabel"
                        anchors.centerIn: parent
                        text: win.bridgeReady
                            ? (ChatBridge.runtimeState + " · " + ChatBridge.activeVramMb + " MiB VRAM")
                            : "Standby"
                        color: "#e2e8f0"
                        font.pixelSize: 11
                        font.bold: true
                    }
                }

                // Hardware profile badge (dev-baseline / ancha-production)
                Rectangle {
                    objectName: "hardwareProfileBadge"
                    radius: 9
                    implicitHeight: 26
                    implicitWidth: hwBadgeLbl.implicitWidth + 16
                    color: Qt.rgba(99 / 255, 102 / 255, 241 / 255, 0.20)
                    border.width: 1
                    border.color: Qt.rgba(129 / 255, 140 / 255, 248 / 255, 0.42)

                    Controls.Label {
                        id: hwBadgeLbl
                        objectName: "hardwareProfileLabel"
                        anchors.centerIn: parent
                        text: win.bridgeReady ? ChatBridge.hardwareProfile : "dev-baseline"
                        color: "#c7d2fe"
                        font.pixelSize: 11
                        font.bold: true
                    }
                }

                // Security posture badges
                Repeater {
                    model: [
                        { id: "loopback", label: "Loopback-Only" },
                        { id: "isolated", label: "İzole Veri" },
                        { id: "noshell", label: "Komut Kapalı" },
                        { id: "privacy", label: "Şifreli 0600 · Sıfır Telemetri" }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        objectName: "securityBadge_" + modelData.id
                        radius: 8
                        implicitHeight: 24
                        implicitWidth: secLbl.implicitWidth + 12
                        color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, 0.72)
                        border.width: 1
                        border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.22)

                        Controls.Label {
                            id: secLbl
                            anchors.centerIn: parent
                            text: modelData.label
                            color: "#93c5fd"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }
            }
        }

        // 2. Main Split Area: Left Session & Privacy Sidebar + Right Chat Surface
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Left Sidebar
            Rectangle {
                objectName: "chatSidebar"
                Layout.preferredWidth: 250
                Layout.fillHeight: true
                radius: 16
                color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, 0.80)
                border.width: 1
                border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.16)

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Controls.Button {
                        objectName: "newSessionButton"
                        Layout.fillWidth: true
                        text: "+ Yeni Yerel Sohbet"
                        onClicked: {
                            if (win.bridgeReady) {
                                ChatBridge.createSession("Yeni ZCI Sohbeti")
                            }
                        }
                        background: Rectangle {
                            radius: 10
                            color: parent.hovered ? "#2596ff" : "#168cff"
                        }
                        contentItem: Controls.Label {
                            text: parent.text
                            color: "#ffffff"
                            font.bold: true
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Controls.Label {
                        text: "OTURUMLAR"
                        color: "#7c91ab"
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 1.0
                    }

                    ListView {
                        objectName: "sessionsListView"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 6
                        model: win.bridgeReady ? ChatBridge.sessions : []

                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool selected: win.bridgeReady
                                && ChatBridge.activeSessionId === modelData.session_id
                            width: ListView.view ? ListView.view.width : 220
                            height: 44
                            radius: 10
                            color: selected
                                ? Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.24)
                                : Qt.rgba(30 / 255, 41 / 255, 59 / 255, 0.45)
                            border.width: 1
                            border.color: selected
                                ? Qt.rgba(56 / 255, 189 / 255, 248 / 255, 0.50)
                                : Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.12)

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 1
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData.title || "ZCI Sohbeti"
                                    color: "#f1f5f9"
                                    font.pixelSize: 12
                                    font.bold: selected
                                    elide: Text.ElideRight
                                }
                                Controls.Label {
                                    objectName: "sessionMessageCountLabel"
                                    Layout.fillWidth: true
                                    readonly property int msgCount: {
                                        if (modelData.message_count !== undefined && modelData.message_count !== null) {
                                            return Number(modelData.message_count)
                                        }
                                        if (modelData.messageCount !== undefined && modelData.messageCount !== null) {
                                            return Number(modelData.messageCount)
                                        }
                                        return 0
                                    }
                                    text: msgCount + " ileti"
                                    color: "#94a3b8"
                                    font.pixelSize: 10
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (win.bridgeReady) {
                                        ChatBridge.selectSession(modelData.session_id)
                                    }
                                }
                            }
                        }
                    }

                    // Opt-in ReadOnly System Context Switch (12.02)
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: ctxCol.implicitHeight + 16
                        radius: 10
                        color: Qt.rgba(11 / 255, 17 / 255, 29 / 255, 0.72)
                        border.width: 1
                        border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.14)

                        ColumnLayout {
                            id: ctxCol
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 4

                            Controls.CheckBox {
                                id: sysCtxCheck
                                objectName: "systemContextToggle"
                                Layout.fillWidth: true
                                text: "Salt-Okunur Sistem Bağlamı"
                                checked: win.bridgeReady ? ChatBridge.includeSystemContext : false
                                onToggled: {
                                    if (win.bridgeReady) {
                                        ChatBridge.includeSystemContext = checked
                                    }
                                }
                                contentItem: Controls.Label {
                                    leftPadding: sysCtxCheck.indicator.width + 6
                                    text: sysCtxCheck.text
                                    color: "#e2e8f0"
                                    font.pixelSize: 11
                                    font.bold: true
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                text: "Açık onayınızla donanım profili ve pasif/aktif durum bilgisi güvenilmeyen salt-veri zarfında eklenir."
                                color: "#94a3b8"
                                font.pixelSize: 10
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    // One-click encrypted history clear (12.06)
                    Controls.Button {
                        objectName: "clearHistoryButton"
                        Layout.fillWidth: true
                        text: "Geçmişi Temizle"
                        onClicked: {
                            if (win.bridgeReady) {
                                ChatBridge.clearActiveSession()
                            }
                        }
                        background: Rectangle {
                            radius: 10
                            color: parent.hovered
                                ? Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.28)
                                : Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.16)
                            border.width: 1
                            border.color: Qt.rgba(248 / 255, 113 / 255, 113 / 255, 0.42)
                        }
                        contentItem: Controls.Label {
                            text: parent.text
                            color: "#fecaca"
                            font.pixelSize: 12
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            // Right Chat Pane
            Rectangle {
                objectName: "chatMainPane"
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 16
                color: Qt.rgba(15 / 255, 23 / 255, 42 / 255, 0.80)
                border.width: 1
                border.color: Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.16)

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    // Error / Security block banner
                    Rectangle {
                        objectName: "errorBanner"
                        visible: Boolean(win.bridgeReady && ChatBridge.errorMessage.length > 0)
                        Layout.fillWidth: true
                        implicitHeight: Math.max(36, errRow.implicitHeight + 14)
                        radius: 10
                        color: Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.18)
                        border.width: 1
                        border.color: Qt.rgba(248 / 255, 113 / 255, 113 / 255, 0.45)

                        RowLayout {
                            id: errRow
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 8
                            spacing: 8

                            Controls.Label {
                                id: errBannerLbl
                                objectName: "errorBannerLabel"
                                Layout.fillWidth: true
                                text: win.bridgeReady ? ChatBridge.errorMessage : ""
                                color: "#fecaca"
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }

                            Controls.Button {
                                objectName: "dismissErrorButton"
                                implicitHeight: 24
                                text: "Kapat"
                                onClicked: {
                                    if (win.bridgeReady) {
                                        ChatBridge.clearError()
                                    }
                                }
                                background: Rectangle {
                                    radius: 6
                                    color: Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.28)
                                }
                                contentItem: Controls.Label {
                                    text: parent.text
                                    color: "#fee2e2"
                                    font.pixelSize: 10
                                    font.bold: true
                                    leftPadding: 8
                                    rightPadding: 8
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }

                    // Non-blocking async response waiting banner
                    Rectangle {
                        objectName: "sendingIndicatorBanner"
                        visible: Boolean(win.bridgeReady && ChatBridge.isSending)
                        Layout.fillWidth: true
                        implicitHeight: 34
                        radius: 10
                        color: Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.16)
                        border.width: 1
                        border.color: Qt.rgba(56 / 255, 189 / 255, 248 / 255, 0.45)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Controls.BusyIndicator {
                                running: Boolean(win.bridgeReady && ChatBridge.isSending)
                                implicitWidth: 18
                                implicitHeight: 18
                            }

                            Controls.Label {
                                objectName: "sendingStatusLabel"
                                Layout.fillWidth: true
                                text: (win.bridgeReady && ChatBridge.sendingStatusText.length > 0)
                                    ? ChatBridge.sendingStatusText
                                    : "Yerel model yanıt hazırlıyor..."
                                color: "#bae6fd"
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }
                    }

                    // Messages List
                    ListView {
                        id: messagesView
                        objectName: "messagesListView"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 10
                        model: win.bridgeReady ? ChatBridge.messages : []

                        onCountChanged: {
                            if (count > 0) {
                                positionViewAtEnd()
                            }
                        }

                        delegate: Item {
                            required property var modelData
                            readonly property bool isUser: modelData.role === "user"
                            readonly property bool isNotice: modelData.role === "notice"
                            width: messagesView.width
                            height: bubbleRect.implicitHeight + 4

                            Rectangle {
                                id: bubbleRect
                                width: Math.min(parent.width * 0.82, Math.max(240, msgCol.implicitWidth + 24))
                                anchors.right: isUser ? parent.right : undefined
                                anchors.left: isUser ? undefined : parent.left
                                implicitHeight: msgCol.implicitHeight + 20
                                radius: 14
                                color: isNotice
                                    ? Qt.rgba(239 / 255, 68 / 255, 68 / 255, 0.20)
                                    : (isUser
                                        ? Qt.rgba(22 / 255, 140 / 255, 255 / 255, 0.24)
                                        : Qt.rgba(11 / 255, 17 / 255, 29 / 255, 0.86))
                                border.width: 1
                                border.color: isNotice
                                    ? Qt.rgba(248 / 255, 113 / 255, 113 / 255, 0.55)
                                    : (isUser
                                        ? Qt.rgba(56 / 255, 189 / 255, 248 / 255, 0.42)
                                        : Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.18))

                                ColumnLayout {
                                    id: msgCol
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 4

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Controls.Label {
                                            text: isNotice
                                                ? (modelData.title || "Uyarı / Engel")
                                                : (isUser ? "Kullanıcı" : "ZCI Asistan")
                                            color: isNotice ? "#fca5a5" : (isUser ? "#7dd3fc" : "#93c5fd")
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                        Item { Layout.fillWidth: true }
                                        Controls.Label {
                                            text: "executable=false · authority=none"
                                            color: "#64748b"
                                            font.pixelSize: 9
                                        }
                                    }

                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.content || ""
                                        color: isNotice ? "#fee2e2" : "#f8fafc"
                                        font.pixelSize: 13
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }

                        // Empty state hint
                        Controls.Label {
                            anchors.centerIn: parent
                            visible: messagesView.count === 0
                            width: Math.min(parent.width - 40, 440)
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            color: "#7c91ab"
                            font.pixelSize: 13
                            text: "Zethropol ZCI Chatbot tamamen yerel çalışır (127.0.0.1 loopback, sıfır dış ağ ve sıfır telemetri).\nDoğrudan kabuk komutu çalıştırmaz; geçmişiniz ev dizininizde şifreli (0600) saklanır."
                        }
                    }

                    // Input Bar
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Controls.TextField {
                            id: promptInput
                            objectName: "promptInputField"
                            Layout.fillWidth: true
                            implicitHeight: 42
                            placeholderText: "Yerel ZCI asistana bir soru yazın..."
                            color: "#f8fafc"
                            placeholderTextColor: "#64748b"
                            font.pixelSize: 13
                            enabled: !win.bridgeReady || !ChatBridge.isSending
                            background: Rectangle {
                                radius: 11
                                color: Qt.rgba(11 / 255, 17 / 255, 29 / 255, 0.90)
                                border.width: 1
                                border.color: promptInput.activeFocus
                                    ? "#168cff"
                                    : Qt.rgba(148 / 255, 185 / 255, 255 / 255, 0.22)
                            }
                            onAccepted: {
                                if (win.bridgeReady && !ChatBridge.isSending && text.trim().length > 0) {
                                    const msg = text
                                    text = ""
                                    ChatBridge.sendMessage(msg, ChatBridge.maxOutputTokens > 0 ? ChatBridge.maxOutputTokens : 2048)
                                }
                            }
                        }

                        Controls.Button {
                            objectName: "sendButton"
                            implicitWidth: 124
                            implicitHeight: 42
                            text: (win.bridgeReady && ChatBridge.isSending) ? "Hazırlanıyor..." : "Gönder"
                            enabled: promptInput.text.trim().length > 0 && (!win.bridgeReady || !ChatBridge.isSending)
                            onClicked: {
                                if (win.bridgeReady && !ChatBridge.isSending && promptInput.text.trim().length > 0) {
                                    const msg = promptInput.text
                                    promptInput.text = ""
                                    ChatBridge.sendMessage(msg, ChatBridge.maxOutputTokens > 0 ? ChatBridge.maxOutputTokens : 2048)
                                }
                            }
                            background: Rectangle {
                                radius: 11
                                color: parent.enabled
                                    ? (parent.hovered ? "#2596ff" : "#168cff")
                                    : Qt.rgba(51 / 255, 65 / 255, 85 / 255, 0.6)
                            }
                            contentItem: Controls.Label {
                                text: parent.text
                                color: "#ffffff"
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
        }
    }
}
