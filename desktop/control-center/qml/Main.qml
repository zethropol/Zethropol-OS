import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material
import Zethropol.ControlCenter

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
                        onClicked: contentStack.currentIndex = index
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

                StackLayout {
                    id: contentStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            Label { text: "Overview"; font.pixelSize: 32 }
                            Label { text: "Zethropol system overview"; opacity: 0.7 }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 16
                                Repeater {
                                    model: ["System Status", "Hardware", "Performance"]
                                    delegate: Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 140
                                        radius: 8
                                        border.width: 1
                                        ColumnLayout {
                                            anchors.centerIn: parent
                                            Label { text: modelData; font.pixelSize: 18 }
                                            Label { text: "Not connected"; opacity: 0.6 }
                                        }
                                    }
                                }
                            }

                            Label {
                                text: "Hardware Health"
                                font.pixelSize: 22
                                Layout.topMargin: 16
                            }

                            GridLayout {
                                columns: 3
                                Layout.fillWidth: true
                                rowSpacing: 12
                                columnSpacing: 12

                                Repeater {
                                    model: ["CPU", "GPU", "Memory", "Storage", "Network", "Overall Health"]
                                    delegate: Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 110
                                        radius: 6
                                        border.width: 1

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 12

                                            Label {
                                                text: modelData
                                                font.pixelSize: 16
                                            }

                                            Item { Layout.fillWidth: true }

                                            Label {
                                                text: index === 0 ? SystemState.cpuStatus : index === 1 ? SystemState.gpuStatus : index === 2 ? SystemState.memoryStatus : index === 3 ? SystemState.storageStatus : index === 4 ? SystemState.networkStatus : SystemState.overallHealth
                                                opacity: 0.6
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.cpuMessage : index === 1 ? SystemState.gpuMessage : index === 2 ? SystemState.memoryMessage : index === 3 ? SystemState.storageMessage : index === 4 ? SystemState.networkMessage : "System health state"
                                                opacity: 0.45
                                                wrapMode: Text.WordWrap
                                                Layout.preferredWidth: 180
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Repeater {
                        model: 8
                        delegate: Item {
                            Label {
                                anchors.centerIn: parent
                                text: ["Hardware", "System", "Performance", "Power", "Updates", "Recovery", "Security", "Applications"][index]
                                font.pixelSize: 28
                                opacity: 0.7
                            }
                        }
                    }
                }
            }
        }
    }
} 
