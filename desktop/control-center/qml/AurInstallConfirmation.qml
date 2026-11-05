import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    required property var updateBridge
    property string receiptHash: ""
    property string previewDetails: ""

    title: "Confirm AUR Installation Request"
    modal: true

    onOpened: {
        aurConsentCheck.checked = false
    }

    onAccepted: {
        updateBridge.confirmAurInstall(receiptHash, aurConsentCheck.checked && receiptHash.length > 0 && updateBridge.aurInstallReady)
    }

    onRejected: {
        updateBridge.confirmAurInstall(receiptHash, false)
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: "AUR packages are built in an isolated sandbox and verified by an independent attestor. Installation is gated by the broker and requires separate confirmation."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        Label {
            text: "Verified Plan Hash: " + root.receiptHash
            font.bold: true
            font.family: "monospace"
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            clip: true
            TextArea {
                text: root.previewDetails
                readOnly: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
            }
        }

        CheckBox {
            id: aurConsentCheck
            objectName: "aurInstallConsentCheck"
            text: "I explicitly authorize submitting this verified AUR package to the broker."
            Layout.fillWidth: true
        }
    }

    footer: DialogButtonBox {
        standardButtons: Dialog.Cancel
        onAccepted: root.accept()
        onRejected: root.reject()

        Button {
            objectName: "confirmAurInstallButton"
            text: "Submit AUR Request"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: aurConsentCheck.checked && root.receiptHash.length > 0 && root.updateBridge.aurInstallReady
        }
    }
}
