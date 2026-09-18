import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    visible: true
    width: 1100
    height: 700
    title: "Zethropol Control Center"

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12

        Label {
            text: "Zethropol Control Center"
            font.pixelSize: 32
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: "Desktop Experience"
            opacity: 0.7
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
