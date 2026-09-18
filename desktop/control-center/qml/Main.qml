import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    visible: true
    width: 1100
    height: 700
    title: "Zethropol Control Center"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.preferredWidth: 240
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 8

                Label {
                    text: "Zethropol"
                    font.pixelSize: 24
                    Layout.bottomMargin: 16
                }

                Repeater {
                    model: ["Overview", "Hardware", "System", "Performance", "Power", "Updates", "Recovery", "Security", "Applications"]

                    delegate: Button {
                        text: modelData
                        Layout.fillWidth: true
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 16

                Label {
                    text: "Overview"
                    font.pixelSize: 32
                }

                Label {
                    text: "Zethropol system overview"
                    opacity: 0.7
                }

                Item { Layout.fillHeight: true }
            }
        }
    }
} 
