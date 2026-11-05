import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    property string errorTitle: ""
    property string errorDetails: ""

    title: errorTitle.length > 0 ? errorTitle : "Update Operation Notice"
    modal: true

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            text: root.errorTitle
            font.bold: true
            font.pixelSize: 16
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(200, Math.max(60, errorText.implicitHeight + 20))
            clip: true
            TextArea {
                id: errorText
                text: root.errorDetails
                readOnly: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
            }
        }
    }

    footer: DialogButtonBox {
        standardButtons: Dialog.Close
        onRejected: root.close()
    }
}
