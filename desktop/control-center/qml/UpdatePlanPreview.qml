import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: root

    required property var updateBridge

    implicitWidth: previewButton.implicitWidth
    implicitHeight: previewButton.implicitHeight

    Button {
        id: previewButton
        objectName: "previewPlanButton"
        anchors.fill: parent
        text: root.updateBridge.previewing ? "Preparing Plan..." : "Preview Plan"
        enabled: !root.updateBridge.checking && !root.updateBridge.installing && !root.updateBridge.previewing
        onClicked: {
            root.updateBridge.previewPlan()
            updatePlanWindow.show()
            updatePlanWindow.raise()
            updatePlanWindow.requestActivate()
        }
    }

    ApplicationWindow {
        id: updatePlanWindow
        objectName: "updatePlanWindow"
        title: "Update Transaction Plan"
        visible: false
        modality: Qt.NonModal
        flags: Qt.Window
        width: Math.min(900, Screen.desktopAvailableWidth)
        height: Math.min(700, Screen.desktopAvailableHeight)
        minimumWidth: Math.min(360, Screen.desktopAvailableWidth)
        minimumHeight: Math.min(240, Screen.desktopAvailableHeight)

        ScrollView {
            objectName: "previewPlanScroll"
            anchors.fill: parent
            anchors.margins: 16
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            TextArea {
                objectName: "previewPlanText"
                readOnly: true
                selectByMouse: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                text: root.updateBridge.planPreview
            }
        }

        footer: DialogButtonBox {
            objectName: "previewPlanCloseButtons"
            standardButtons: DialogButtonBox.Close
            onRejected: updatePlanWindow.close()
        }
    }
}
