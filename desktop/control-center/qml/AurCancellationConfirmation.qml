import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    required property var updateBridge
    property string operationId: ""

    title: "Cancel AUR Broker Operation"
    modal: true

    onAccepted: {
        updateBridge.confirmAurCancellation(operationId, operationId.length > 0)
    }

    onRejected: {
        updateBridge.confirmAurCancellation(operationId, false)
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: "Are you sure you want to cancel the in-flight AUR broker operation?"
            font.bold: true
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            text: "Operation ID: " + root.operationId
            font.family: "monospace"
            Layout.fillWidth: true
        }

        Label {
            text: "Notice: Cancellation stops pending worker steps. A transaction that has already committed cannot be rolled back by cancellation."
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
            objectName: "confirmAurCancellationButton"
            text: "Cancel Operation"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: root.operationId.length > 0
        }
    }
}
