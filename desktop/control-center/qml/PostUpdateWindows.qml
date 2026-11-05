import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Item {
    id: root
    required property var flow
    property bool updateBusy: false
    onUpdateBusyChanged: flow.setUpdateBusy(updateBusy)
    Component.onCompleted: { flow.setUpdateBusy(updateBusy); flow.start() }

    ApplicationWindow {
        id: cleanupWindow
        objectName: "cleanupResultWindow"
        property bool dismissed: false
        visible: root.flow.phase === "cleanup"
        title: root.flow.title
        flags: Qt.Window
        width: Math.min(820, Screen.desktopAvailableWidth)
        height: Math.min(620, Screen.desktopAvailableHeight)
        minimumWidth: Math.min(360, Screen.desktopAvailableWidth)
        minimumHeight: Math.min(240, Screen.desktopAvailableHeight)
        onClosing: dismissed = true
        onVisibleChanged: {
            if (visible) dismissed = false
            else if (dismissed) {
                // Advance only after this native window has actually hidden.
                Qt.callLater(function() { root.flow.acknowledgeCleanup() })
            }
        }
        ScrollView {
            objectName: "cleanupResultScroll"
            anchors.fill: parent
            anchors.margins: 16
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            TextArea {
                objectName: "cleanupResultText"
                text: root.flow.details
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "acknowledgeCleanupButton"
                text: "Tamam"
                onClicked: cleanupWindow.close()
            }
        }
    }

    ApplicationWindow {
        id: rebootWindow
        objectName: "postUpdateRebootWindow"
        visible: root.flow.phase === "reboot"
        title: "Yeniden başlatma onayı"
        flags: Qt.Window
        width: Math.min(720, Screen.desktopAvailableWidth)
        height: Math.min(460, Screen.desktopAvailableHeight)
        minimumWidth: Math.min(360, Screen.desktopAvailableWidth)
        minimumHeight: Math.min(240, Screen.desktopAvailableHeight)
        onClosing: function(close) {
            if (root.flow.busy) close.accepted = false
            else root.flow.deferReboot()
        }
        ScrollView {
            anchors.fill: parent
            anchors.margins: 16
            contentWidth: availableWidth
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            TextArea {
                text: "Kaydedilmemiş çalışmalarınızı kaydedin. Otomatik yeniden başlatma yapılmaz.\n\nİşlem: "
                    + root.flow.operationId + "\n\n" + root.flow.rebootReason
                    + "\n\n" + root.flow.statusText
                textFormat: TextEdit.PlainText
                readOnly: true
                wrapMode: TextEdit.Wrap
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "rebootLaterButton"
                text: "Daha sonra"
                enabled: !root.flow.busy
                onClicked: root.flow.deferReboot()
            }
            Button {
                objectName: "rebootNowButton"
                text: root.flow.busy ? "Doğrulanıyor..." : "Şimdi yeniden başlat"
                enabled: !root.flow.busy && root.flow.canReviewReboot
                onClicked: root.flow.rebootNow()
            }
        }
    }
}
