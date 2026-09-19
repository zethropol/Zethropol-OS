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
                                            Label {
    text: index === 0 ? SystemState.systemStatus : index === 1 ? SystemState.hardwareSummary : SystemState.performanceStatus
    opacity: 0.6
    Layout.fillWidth: true
    wrapMode: Text.WordWrap
    horizontalAlignment: Text.AlignHCenter
    maximumLineCount: 2
    elide: Text.ElideRight
}
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

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 12
                                            spacing: 4

                                            Label {
                                                text: modelData
                                                font.pixelSize: 16
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.cpuStatus : index === 1 ? SystemState.gpuModel : index === 2 ? SystemState.memoryStatus : index === 3 ? SystemState.storageStatus : index === 4 ? SystemState.networkStatus : SystemState.overallHealth
                                                opacity: 0.6
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.cpuMessage : index === 1 ? SystemState.gpuDetails : index === 2 ? SystemState.memoryMessage : index === 3 ? SystemState.storageMessage : index === 4 ? SystemState.networkMessage : "System health state"
                                                opacity: 0.45
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
                                                maximumLineCount: 2
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 14

                            Label {
                                text: "Hardware"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Detected hardware and system capabilities"
                                opacity: 0.7
                            }

                            GridLayout {
                                columns: 2
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                rowSpacing: 12
                                columnSpacing: 12

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 130
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "CPU"; font.pixelSize: 18 }
                                        Label { text: SystemState.cpuMessage; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                        Label { text: SystemState.cpuDetails; opacity: 0.65; Layout.fillWidth: true }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 130
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "GPU"; font.pixelSize: 18 }
                                        Label { text: "Model: " + SystemState.gpuModel; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                      Label { text: "Family: " + SystemState.gpuFamily; Layout.fillWidth: true; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight; opacity: 0.65 }
                                        Label { text: "Status: " + SystemState.gpuStatus; opacity: 0.65 }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "Memory"; font.pixelSize: 18 }
                                        Label { text: SystemState.memoryDetails; opacity: 0.65 }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "Storage"; font.pixelSize: 18 }
                                        Label { text: SystemState.storageDetails; opacity: 0.65; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "Network"; font.pixelSize: 18 }
                                        Label { text: SystemState.networkDetails; opacity: 0.65; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "Firmware"; font.pixelSize: 18 }
                                        Label { text: SystemState.firmwareDetails; opacity: 0.65; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "Capabilities"; font.pixelSize: 18 }
                                        Label { text: SystemState.capabilitiesDetails; opacity: 0.65; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 110
                                    radius: 8
                                    border.width: 1
                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        Label { text: "Hardware Changes"; font.pixelSize: 18 }
                                        Label { text: SystemState.changesDetails; opacity: 0.65; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                    }
                                }
                            }
                        }
                    }

                    Repeater {
                        model: 7
                        delegate: Item {
                            Label {
                                anchors.centerIn: parent
                                text: ["System", "Performance", "Power", "Updates", "Recovery", "Security", "Applications"][index]
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
