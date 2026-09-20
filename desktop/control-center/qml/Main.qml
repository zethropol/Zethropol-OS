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
                    model: ["Overview", "Hardware", "Diagnostics", "System", "Performance", "Power", "Updates", "Recovery", "Security", "Applications"]

                    delegate: Button {
                        text: modelData
                        Layout.fillWidth: true
                        onClicked: contentStack.currentIndex = index
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
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
                            spacing: 14

                            Label {
                                text: "Overview"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Zethropol system overview"
                                opacity: 0.7
                            }

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
                                            width: parent.width - 24
                                            spacing: 8

                                            Label {
                                                text: modelData
                                                font.pixelSize: 18
                                                Layout.alignment: Qt.AlignHCenter
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.systemStatus :
                                                      index === 1 ? SystemState.hardwareSummary :
                                                                    SystemState.performanceStatus
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
                                                text: index === 0 ? SystemState.cpuStatus :
                                                      index === 1 ? SystemState.gpuModel :
                                                      index === 2 ? SystemState.memoryStatus :
                                                      index === 3 ? SystemState.storageStatus :
                                                      index === 4 ? SystemState.networkStatus :
                                                                    SystemState.overallHealth
                                                opacity: 0.6
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.cpuMessage :
                                                      index === 1 ? SystemState.gpuDetails :
                                                      index === 2 ? SystemState.memoryMessage :
                                                      index === 3 ? SystemState.storageMessage :
                                                      index === 4 ? SystemState.networkMessage :
                                                                    "System health state"
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

                                        Label {
                                            text: "CPU"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.cpuMessage
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }

                                        Label {
                                            text: SystemState.cpuDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                        }
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

                                        Label {
                                            text: "GPU"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: "Model: " + SystemState.gpuModel
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }

                                        Label {
                                            text: "Family: " + SystemState.gpuFamily
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            maximumLineCount: 2
                                            elide: Text.ElideRight
                                            opacity: 0.65
                                        }

                                        Label {
                                            text: "Status: " + SystemState.gpuStatus
                                            opacity: 0.65
                                        }
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

                                        Label {
                                            text: "Memory"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.memoryDetails
                                            opacity: 0.65
                                        }
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

                                        Label {
                                            text: "Storage"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.storageDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
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

                                        Label {
                                            text: "Network"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.networkDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
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

                                        Label {
                                            text: "Firmware"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.firmwareDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
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

                                        Label {
                                            text: "Capabilities"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.capabilitiesDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
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

                                        Label {
                                            text: "Hardware Changes"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.changesDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
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
                                text: "Diagnostics"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Storage health and NVMe diagnostic information"
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
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Label {
                                            text: "SMART Health"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.diagnosticsHealthStatus
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }

                                        Label {
                                            text: "Temperature: " + SystemState.diagnosticsTemperature
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Label {
                                            text: "Drive Usage"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: "Used: " + SystemState.diagnosticsPercentageUsed
                                            Layout.fillWidth: true
                                        }

                                        Label {
                                            text: "Available spare: " + SystemState.diagnosticsAvailableSpare
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Label {
                                            text: "Data Integrity"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: "Media errors: " + SystemState.diagnosticsMediaErrors
                                            Layout.fillWidth: true
                                        }

                                        Label {
                                            text: "Self-test: " + SystemState.diagnosticsSelfTestStatus
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Label {
                                            text: "Shutdown History"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.diagnosticsUnsafeShutdowns + " unsafe shutdowns"
                                            Layout.fillWidth: true
                                        }

                                        Label {
                                            text: SystemState.diagnosticsUnsafeShutdownsStatus
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Label {
                                            text: "NVMe Error Log"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.diagnosticsErrorLogEntries + " entries"
                                            Layout.fillWidth: true
                                        }

                                        Label {
                                            text: SystemState.diagnosticsErrorLogStatus
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
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
                                text: "System"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Operating system and runtime information"
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
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Zethropol OS"
                                            font.pixelSize: 19
                                        }

                                        Label {
                                            text: SystemState.osName
                                            font.pixelSize: 16
                                        }

                                        Label {
                                            text: "Version: " + SystemState.osVersion
                                            opacity: 0.65
                                        }

                                        Label {
                                            text: "Base: " + SystemState.baseSystem
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Architecture"
                                            font.pixelSize: 19
                                        }

                                        Label {
                                            text: SystemState.architecture
                                            font.pixelSize: 16
                                        }

                                        Label {
                                            text: "Kernel: " + SystemState.kernelVersion
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Desktop"
                                            font.pixelSize: 19
                                        }

                                        Label {
                                            text: SystemState.desktopName
                                            font.pixelSize: 16
                                        }

                                        Label {
                                            text: "Version: " + SystemState.desktopVersion
                                            opacity: 0.65
                                        }

                                        Label {
                                            text: "Session: " + SystemState.sessionType
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Runtime"
                                            font.pixelSize: 19
                                        }

                                        Label {
                                            text: "Hostname: " + SystemState.hostname
                                            font.pixelSize: 16
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }

                                        Label {
                                            text: "Uptime: " + SystemState.uptimeDetails
                                            opacity: 0.65
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 16

                            Label {
                                text: "Performance"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Live hardware performance"
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
                                    Layout.fillHeight: true
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 8

                                        Label { text: "CPU"; font.pixelSize: 20 }
                                        Label {
                                            text: HardwareBridge.performance.cpu_usage_percent !== undefined
                                                  ? "Usage: " + Number(HardwareBridge.performance.cpu_usage_percent).toFixed(1) + "%"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: HardwareBridge.performance.cpu_temperature_c !== null && HardwareBridge.performance.cpu_temperature_c !== undefined
                                                  ? "Temperature: " + Number(HardwareBridge.performance.cpu_temperature_c).toFixed(1) + " °C"
                                                  : "Temperature: Unknown"
                                            opacity: 0.65
                                        }
                                        Label {
                                            text: HardwareBridge.performance.cpu_frequency_ghz !== null && HardwareBridge.performance.cpu_frequency_ghz !== undefined
                                                  ? "Frequency: " + Number(HardwareBridge.performance.cpu_frequency_ghz).toFixed(2) + " GHz"
                                                  : "Frequency: Unknown"
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 8

                                        Label { text: "GPU"; font.pixelSize: 20 }
                                        Label {
                                            text: HardwareBridge.performance.gpu_usage_percent !== null && HardwareBridge.performance.gpu_usage_percent !== undefined
                                                  ? "Usage: " + Number(HardwareBridge.performance.gpu_usage_percent).toFixed(1) + "%"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: HardwareBridge.performance.gpu_temperature_c !== null && HardwareBridge.performance.gpu_temperature_c !== undefined
                                                  ? "Temperature: " + Number(HardwareBridge.performance.gpu_temperature_c).toFixed(1) + " °C"
                                                  : "Temperature: Unknown"
                                            opacity: 0.65
                                        }
                                        Label {
                                            text: HardwareBridge.performance.gpu_vram_used_gb !== null && HardwareBridge.performance.gpu_vram_total_gb !== null
                                                  ? "VRAM: " + Number(HardwareBridge.performance.gpu_vram_used_gb).toFixed(2) + " / " + Number(HardwareBridge.performance.gpu_vram_total_gb).toFixed(2) + " GB"
                                                  : "VRAM: Unknown"
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 8

                                        Label { text: "Memory"; font.pixelSize: 20 }
                                        Label {
                                            text: HardwareBridge.performance.memory_used_gb !== undefined && HardwareBridge.performance.memory_total_gb !== undefined
                                                  ? "Usage: " + Number(HardwareBridge.performance.memory_used_gb).toFixed(2) + " / " + Number(HardwareBridge.performance.memory_total_gb).toFixed(2) + " GB"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 8

                                        Label { text: "Storage"; font.pixelSize: 20 }
                                        Label {
                                            text: HardwareBridge.performance.storage_used_gb !== null && HardwareBridge.performance.storage_capacity_gb !== null
                                                  ? "Usage: " + Number(HardwareBridge.performance.storage_used_gb).toFixed(1) + " / " + Number(HardwareBridge.performance.storage_capacity_gb).toFixed(1) + " GB"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: HardwareBridge.performance.storage_temperature_c !== null && HardwareBridge.performance.storage_temperature_c !== undefined
                                                  ? "Temperature: " + Number(HardwareBridge.performance.storage_temperature_c).toFixed(1) + " °C"
                                                  : "Temperature: Unknown"
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 8

                                        Label { text: "Network"; font.pixelSize: 20 }
                                        Label {
                                            text: "Download: " + Number(HardwareBridge.performance.network_download_mbps || 0).toFixed(2) + " Mbps"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: "Upload: " + Number(HardwareBridge.performance.network_upload_mbps || 0).toFixed(2) + " Mbps"
                                            opacity: 0.65
                                        }
                                        Label {
                                            text: HardwareBridge.performance.network_ping_ms !== null && HardwareBridge.performance.network_ping_ms !== undefined
                                                  ? "Ping: " + Number(HardwareBridge.performance.network_ping_ms).toFixed(1) + " ms"
                                                  : "Ping: Unknown"
                                            opacity: 0.65
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Item {
                        Label {
                            anchors.centerIn: parent
                            text: "Power"
                            font.pixelSize: 28
                            opacity: 0.7
                        }
                    }

                    Item {
                        Label {
                            anchors.centerIn: parent
                            text: "Updates"
                            font.pixelSize: 28
                            opacity: 0.7
                        }
                    }

                    Item {
                        Label {
                            anchors.centerIn: parent
                            text: "Recovery"
                            font.pixelSize: 28
                            opacity: 0.7
                        }
                    }

                    Item {
                        Label {
                            anchors.centerIn: parent
                            text: "Security"
                            font.pixelSize: 28
                            opacity: 0.7
                        }
                    }

                    Item {
                        Label {
                            anchors.centerIn: parent
                            text: "Applications"
                            font.pixelSize: 28
                            opacity: 0.7
                        }
                    }
                }
            }
        }
    }
}
