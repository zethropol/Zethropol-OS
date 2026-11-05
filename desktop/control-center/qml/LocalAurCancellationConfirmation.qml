import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    required property var updateBridge
    property string executionId: ""

    title: "Cancel Local AUR Sandbox Build"
    modal: true

    onAccepted: {
        updateBridge.confirmLocalAurCancellation(executionId, executionId.length > 0)
    }

    onRejected: {
        updateBridge.confirmLocalAurCancellation(executionId, false)
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: "Are you sure you want to terminate the local unprivileged build?"
            font.bold: true
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            text: "Execution ID: " + root.executionId
            font.family: "monospace"
            Layout.fillWidth: true
        }

        Label {
            text: "Notice: The build supervisor process will be terminated, and scratch build artifacts will be reconciled in the local journal."
            opacity: 0.8
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    footer: DialogButtonBox {
        standardButtons: Dialog.Cancel
        onAccepted: root.accept()
        onRejected: root.reject()

        Button {
            objectName: "confirmLocalAurCancellationButton"
            text: "Terminate Build"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: root.executionId.length > 0
        }
    }
}
