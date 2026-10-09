import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material
import Zethropol.ControlCenter

ApplicationWindow {
    QtObject {
        id: notificationState

        property bool previousUpdatesAvailable: false
        property bool previousFirmwareAvailable: false
        property bool previousFailedServices: false
        property bool previousHardwareWarning: false

        Component.onCompleted: {
            previousUpdatesAvailable = UpdateBridge.available || UpdateBridge.aurAvailable || UpdateBridge.flatpakAvailable
            previousFirmwareAvailable = String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
            previousFailedServices = ServiceBridge.failed > 0
            previousHardwareWarning = String(SystemState.overallHealth).toUpperCase() === "WARNING"
        }
    }

    Connections {
        target: UpdateBridge
        property bool initialized: false

        function onStateChanged() {
            var current = UpdateBridge.available || UpdateBridge.aurAvailable || UpdateBridge.flatpakAvailable
            if (!initialized) {
                notificationState.previousUpdatesAvailable = current
                initialized = true
                return
            }
            if (current && !notificationState.previousUpdatesAvailable) {
                NotificationBridge.send(
                    "Software updates available",
                    UpdateBridge.count + " system · " + UpdateBridge.aurCount + " AUR · " + UpdateBridge.flatpakCount + " Flatpak"
                )
            }
            notificationState.previousUpdatesAvailable = current
        }
    }

    Connections {
        target: HardwareBridge
        property bool initialized: false

        function onStateChanged() {
            var currentFirmware = String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
            var currentWarning = String(SystemState.overallHealth).toUpperCase() === "WARNING"

            if (!initialized) {
                notificationState.previousFirmwareAvailable = currentFirmware
                notificationState.previousHardwareWarning = currentWarning
                initialized = true
                return
            }

            if (currentFirmware && !notificationState.previousFirmwareAvailable) {
                NotificationBridge.send(
                    "Firmware update available",
                    "A firmware update requires your attention. Open Diagnostics to review it."
                )
            }

            if (currentWarning && !notificationState.previousHardwareWarning) {
                NotificationBridge.send(
                    "Hardware health requires attention",
                    "Open Hardware to review the current hardware health status."
                )
            }

            notificationState.previousFirmwareAvailable = currentFirmware
            notificationState.previousHardwareWarning = currentWarning
        }
    }

    Connections {
        target: ServiceBridge
        property bool initialized: false

        function onStateChanged() {
            var current = ServiceBridge.failed > 0
            if (!initialized) {
                notificationState.previousFailedServices = current
                initialized = true
                return
            }
            if (current && !notificationState.previousFailedServices) {
                NotificationBridge.send(
                    "Failed service detected",
                    ServiceBridge.failed + " system service" + (ServiceBridge.failed === 1 ? "" : "s") + " require attention."
                )
            }
            notificationState.previousFailedServices = current
        }
    }

    visible: true
    width: 1280
    height: 800
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
                    model: ["Overview", "Hardware", "Diagnostics", "System", "Performance", "Power", "Updates", "Recovery", "Security", "Services", "Applications"]

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
                        Flickable {
                            anchors.fill: parent
                            clip: true
                            contentWidth: width
                            contentHeight: overviewContent.implicitHeight
                            boundsBehavior: Flickable.StopAtBounds

                            ColumnLayout {
                                id: overviewContent
                                width: parent.width
                                spacing: 14

                            Label {
                                text: "Overview"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Zethropol system overview"
                                opacity: 0.7
                            }

                            GridLayout {
                                columns: 2
                                Layout.fillWidth: true
                                rowSpacing: 12
                                columnSpacing: 12

                                Repeater {
                                    model: ["Overall Health", "Hardware Health", "System Activity", "Power & Updates"]

                                    delegate: Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 125
                                        radius: 8
                                        border.width: 1

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 12
                                            spacing: 5

                                            Label {
                                                text: modelData
                                                font.pixelSize: 18
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.overallHealth :
                                                      index === 1 ? SystemState.cpuStatus + " · " + SystemState.gpuStatus + " · " + SystemState.memoryStatus + " · " + SystemState.storageStatus :
                                                      index === 2 ? SystemState.performanceStatus :
                                                                    SystemState.powerAvailability + " · " + SystemState.updatesCount + " updates"
                                                opacity: 0.7
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
                                                maximumLineCount: 2
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: index === 0 ? SystemState.systemMessage :
                                                      index === 1 ? "Network: " + SystemState.networkStatus :
                                                      index === 2 ? SystemState.performanceMessage :
                                                                    SystemState.powerSource
                                                opacity: 0.5
                                                Layout.fillWidth: true
                                                wrapMode: Text.WordWrap
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

                            Item {
                                Layout.fillWidth: true
                                implicitHeight: childrenRect.height
                                visible: UpdateBridge.available || UpdateBridge.aurAvailable || UpdateBridge.flatpakAvailable || (RecoveryBridge.available && RecoveryBridge.snapshotCount === 0) || ServiceBridge.failed > 0 || String(SystemState.overallHealth).toUpperCase() === "WARNING" || String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    spacing: 10

                                    Label {
                                        text: "Recommendations"
                                        font.pixelSize: 22
                                    }

                                    Label {
                                        text: "Actions that may require your attention"
                                        opacity: 0.6
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 8

                                        Rectangle {
                                            visible: String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 12

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        text: "Firmware update recommended"
                                                        font.pixelSize: 16
                                                    }

                                                    Label {
                                                        text: SystemState.firmwareStatus
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                        elide: Text.ElideRight
                                                    }
                                                }

                                                Button {
                                                    text: "Diagnostics"
                                                    onClicked: contentStack.currentIndex = 2
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: UpdateBridge.available || UpdateBridge.aurAvailable || UpdateBridge.flatpakAvailable
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 12

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        text: "Software updates available"
                                                        font.pixelSize: 16
                                                    }

                                                    Label {
                                                        text: UpdateBridge.count + " system · " + UpdateBridge.aurCount + " AUR · " + UpdateBridge.flatpakCount + " Flatpak"
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                    }
                                                }

                                                Button {
                                                    text: "Updates"
                                                    onClicked: contentStack.currentIndex = 6
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: RecoveryBridge.available && RecoveryBridge.snapshotCount === 0
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 12

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        text: "No recovery point available"
                                                        font.pixelSize: 16
                                                    }

                                                    Label {
                                                        text: "Consider creating a recovery point before major changes."
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                        elide: Text.ElideRight
                                                    }
                                                }

                                                Button {
                                                    text: "Recovery"
                                                    onClicked: contentStack.currentIndex = 7
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: ServiceBridge.failed > 0
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 12

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        text: ServiceBridge.failed + " failed service" + (ServiceBridge.failed === 1 ? "" : "s")
                                                        font.pixelSize: 16
                                                    }

                                                    Label {
                                                        text: "A system service may require attention."
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                    }
                                                }

                                                Button {
                                                    text: "Services"
                                                    onClicked: contentStack.currentIndex = 9
                                                }
                                            }
                                        }

                                        Rectangle {
                                            visible: String(SystemState.overallHealth).toUpperCase() === "WARNING"
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 12
                                                spacing: 12

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        text: "Hardware health requires attention"
                                                        font.pixelSize: 16
                                                    }

                                                    Label {
                                                        text: SystemState.overallHealth
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                    }
                                                }

                                                Button {
                                                    text: "Hardware"
                                                    onClicked: contentStack.currentIndex = 1
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            }

                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded
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
                                    Layout.preferredHeight: 150
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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

                                        Label {
                                            text: "Health: " + SystemState.cpuStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 150
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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
                                            text: "Capability: " + SystemState.gpuMessage
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                            opacity: 0.65
                                        }

                                        Label {
                                            text: "Health: " + SystemState.gpuStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

                                        Label {
                                            text: "Memory"
                                            font.pixelSize: 18
                                        }

                                        Label {
                                            text: SystemState.memoryDetails
                                            opacity: 0.65
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }

                                        Label {
                                            text: "Health: " + SystemState.memoryStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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

                                        Label {
                                            text: "Health: " + SystemState.storageStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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

                                        Label {
                                            text: "Status: " + SystemState.networkStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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

                                        Label {
                                            text: "Status: " + SystemState.firmwareStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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

                                        Label {
                                            text: "Status: " + SystemState.systemStatus
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 140
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 6

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

                                        Label {
                                            text: "Status: " + SystemState.changesStatus
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
                                            text: MonitoringBridge.state.cpu_usage_percent !== undefined
                                                  ? "Usage: " + Number(MonitoringBridge.state.cpu_usage_percent).toFixed(1) + "%"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: MonitoringBridge.state.cpu_temperature_c !== null && MonitoringBridge.state.cpu_temperature_c !== undefined
                                                  ? "Temperature: " + Number(MonitoringBridge.state.cpu_temperature_c).toFixed(1) + " °C"
                                                  : "Temperature: Unknown"
                                            opacity: 0.65
                                        }
                                        Label {
                                            text: MonitoringBridge.state.cpu_frequency_ghz !== null && MonitoringBridge.state.cpu_frequency_ghz !== undefined
                                                  ? "Frequency: " + Number(MonitoringBridge.state.cpu_frequency_ghz).toFixed(2) + " GHz"
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
                                            text: MonitoringBridge.state.gpu_usage_percent !== null && MonitoringBridge.state.gpu_usage_percent !== undefined
                                                  ? "Usage: " + Number(MonitoringBridge.state.gpu_usage_percent).toFixed(1) + "%"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: MonitoringBridge.state.gpu_temperature_c !== null && MonitoringBridge.state.gpu_temperature_c !== undefined
                                                  ? "Temperature: " + Number(MonitoringBridge.state.gpu_temperature_c).toFixed(1) + " °C"
                                                  : "Temperature: Unknown"
                                            opacity: 0.65
                                        }
                                        Label {
                                            text: MonitoringBridge.state.gpu_vram_used_gb !== null && MonitoringBridge.state.gpu_vram_total_gb !== null
                                                  ? "VRAM: " + Number(MonitoringBridge.state.gpu_vram_used_gb).toFixed(2) + " / " + Number(MonitoringBridge.state.gpu_vram_total_gb).toFixed(2) + " GB"
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
                                            text: MonitoringBridge.state.memory_used_gb !== undefined && MonitoringBridge.state.memory_total_gb !== undefined
                                                  ? "Usage: " + Number(MonitoringBridge.state.memory_used_gb).toFixed(2) + " / " + Number(MonitoringBridge.state.memory_total_gb).toFixed(2) + " GB"
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
                                            text: MonitoringBridge.state.storage_used_gb !== null && MonitoringBridge.state.storage_capacity_gb !== null
                                                  ? "Usage: " + Number(MonitoringBridge.state.storage_used_gb).toFixed(1) + " / " + Number(MonitoringBridge.state.storage_capacity_gb).toFixed(1) + " GB"
                                                  : "Usage: Unknown"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: MonitoringBridge.state.storage_temperature_c !== null && MonitoringBridge.state.storage_temperature_c !== undefined
                                                  ? "Temperature: " + Number(MonitoringBridge.state.storage_temperature_c).toFixed(1) + " °C"
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
                                            text: "Download: " + Number(MonitoringBridge.state.network_download_mbps || 0).toFixed(2) + " Mbps"
                                            font.pixelSize: 16
                                        }
                                        Label {
                                            text: "Upload: " + Number(MonitoringBridge.state.network_upload_mbps || 0).toFixed(2) + " Mbps"
                                            opacity: 0.65
                                        }
                                        Label {
                                            text: MonitoringBridge.state.network_ping_ms !== null && MonitoringBridge.state.network_ping_ms !== undefined
                                                  ? "Ping: " + Number(MonitoringBridge.state.network_ping_ms).toFixed(1) + " ms"
                                                  : "Ping: Unknown"
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
                                text: "Power"
                                font.pixelSize: 28
                                Layout.bottomMargin: 8
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 16
                                rowSpacing: 16

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 12

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        spacing: 6

                                        Label {
                                            text: "Power Status"
                                            font.bold: true
                                        }

                                        Label {
                                            text: PowerBridge.state.available === true ? "Available" : (PowerBridge.state.available === false ? "Unavailable" : "Unknown")
                                            font.pixelSize: 20
                                        }

                                        Label {
                                            text: PowerBridge.state.battery_present === true ? "Battery" : (PowerBridge.state.available === true ? "No battery detected" : "Unknown")
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 12

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        spacing: 6

                                        Label {
                                            text: "Battery"
                                            font.bold: true
                                        }

                                        Label {
                                            text: PowerBridge.state.battery_percent !== undefined && PowerBridge.state.battery_percent !== null ? Number(PowerBridge.state.battery_percent).toFixed(1) + "%" : "N/A"
                                            font.pixelSize: 20
                                        }

                                        Label {
                                            text: (PowerBridge.state.battery_status || "N/A") + " · " + (PowerBridge.state.charging === true ? "Charging" : (PowerBridge.state.charging === false ? "Not charging" : "N/A"))
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 12

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        spacing: 6

                                        Label {
                                            text: "Power Usage"
                                            font.bold: true
                                        }

                                        Label {
                                            text: PowerBridge.state.power_w !== undefined && PowerBridge.state.power_w !== null ? Number(PowerBridge.state.power_w).toFixed(2) + " W" : "N/A"
                                            font.pixelSize: 20
                                        }

                                        Label {
                                            text: "Current system power"
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 12

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 18
                                        spacing: 6

                                        Label {
                                            text: "Power Management"
                                            font.bold: true
                                        }

                                        Label {
                                            text: PowerBridge.state.governor || "Not available"
                                            font.pixelSize: 20
                                        }

                                        Label {
                                            text: "Profile: " + (PowerBridge.state.profile || "Not available")
                                            opacity: 0.7
                                        }
                                    }
                                }
                            }

                            Item {
                                Layout.fillHeight: true
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 16

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    text: "Updates"
                                    font.pixelSize: 28
                                }

                                Item {
                                    Layout.fillWidth: true
                                }

                                Button {
                                    text: UpdateBridge.checking ? "Checking..." : "Check for Updates"
                                    enabled: !UpdateBridge.checking && !UpdateBridge.installing
                                    onClicked: UpdateBridge.check()
                                }

                                Button {
                                    text: UpdateBridge.installing ? "Installing..." : "Install Updates"
                                    enabled: UpdateBridge.available && !UpdateBridge.checking && !UpdateBridge.installing
                                    onClicked: UpdateBridge.install()
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 125
                                radius: 12

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 18
                                    spacing: 6

                                    Label {
                                        text: UpdateBridge.checking
                                              ? "Checking for updates..."
                                              : UpdateBridge.installing
                                                ? "Installing system updates..."
                                                : UpdateBridge.status
                                        font.pixelSize: 20
                                        font.bold: true
                                    }

                                    Label {
                                        text: UpdateBridge.installing
                                              ? "The system package manager is applying the available updates."
                                              : UpdateBridge.failed
                                                ? "The update process could not be completed."
                                                : "Package updates detected through the system package manager"
                                        opacity: 0.7
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                }
                            }

                            Label {
                                visible: ApplicationBridge.removeStatus !== ""
                                text: ApplicationBridge.removeStatus
                                opacity: 0.8
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Rectangle {
                                visible: UpdateBridge.aurAvailable
                                Layout.fillWidth: true
                                Layout.preferredHeight: 92
                                radius: 12

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Label {
                                                text: "AUR Updates"
                                                font.pixelSize: 18
                                                font.bold: true
                                            }

                                            Label {
                                                text: UpdateBridge.aurCount + " AUR package(s) available"
                                                opacity: 0.7
                                            }
                                        }
                                    }

                                    ListView {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(120, UpdateBridge.aurUpdates.length * 28)
                                        model: UpdateBridge.aurUpdates
                                        interactive: false

                                        delegate: Label {
                                            text: modelData.name + "  " + modelData.currentVersion
                                                  + " → " + modelData.newVersion
                                            opacity: 0.8
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                visible: UpdateBridge.flatpakAvailable
                                Layout.fillWidth: true
                                Layout.preferredHeight: 92
                                radius: 12

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Label {
                                                text: "Flatpak Updates"
                                                font.pixelSize: 18
                                                font.bold: true
                                            }

                                            Label {
                                                text: UpdateBridge.flatpakCount + " Flatpak update(s) available"
                                                opacity: 0.7
                                            }
                                        }
                                    }

                                    ListView {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(120, UpdateBridge.flatpakUpdates.length * 28)
                                        model: UpdateBridge.flatpakUpdates
                                        interactive: false

                                        delegate: Label {
                                            text: modelData.name
                                            opacity: 0.8
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                visible: UpdateBridge.orphanCount > 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: 92
                                radius: 12

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Label {
                                                text: "Orphan Packages"
                                                font.pixelSize: 18
                                                font.bold: true
                                            }

                                            Label {
                                                text: UpdateBridge.orphanCount + " orphan package(s) detected"
                                                opacity: 0.7
                                            }
                                        }

                                        Button {
                                            text: UpdateBridge.removingOrphans
                                                ? "Removing..."
                                                : "Remove Orphans"
                                            enabled: !UpdateBridge.removingOrphans
                                                && !UpdateBridge.installing
                                            onClicked: UpdateBridge.removeOrphans()
                                        }
                                    }
                                }
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 8
                                model: UpdateBridge.updates

                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 72
                                    radius: 10

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 12

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 3

                                            Label {
                                                text: modelData.name
                                                font.bold: true
                                            }

                                            Label {
                                                text: modelData.currentVersion + " → " + modelData.newVersion
                                                opacity: 0.7
                                            }
                                        }
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: !UpdateBridge.checking
                                             && !UpdateBridge.installing
                                             && UpdateBridge.updates.length === 0
                                    text: "No package updates available"
                                    opacity: 0.6
                                }
                            }

                            Item {
                                Layout.fillHeight: true
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 16

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    text: "Recovery"
                                    font.pixelSize: 28
                                }

                                Item {
                                    Layout.fillWidth: true
                                }

                                Button {
                                    text: RecoveryBridge.checking ? "Checking..." : "Refresh"
                                    enabled: !RecoveryBridge.checking
                                    onClicked: RecoveryBridge.refresh()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                TextField {
                                    id: recoveryDescriptionField
                                    Layout.fillWidth: true
                                    placeholderText: "Recovery point description"
                                    enabled: !RecoveryBridge.actionBusy
                                }

                                Button {
                                    text: RecoveryBridge.actionBusy
                                          ? "Creating..."
                                          : "Create Recovery Point"
                                    enabled: !RecoveryBridge.actionBusy
                                    onClicked: RecoveryBridge.createRecoveryPoint(
                                        recoveryDescriptionField.text)
                                }
                            }

                            Label {
                                visible: RecoveryBridge.actionStatus !== ""
                                text: RecoveryBridge.actionStatus
                                opacity: 0.8
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 125
                                radius: 12

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 18
                                    spacing: 6

                                    Label {
                                        text: RecoveryBridge.checking
                                              ? "Checking recovery information..."
                                              : (RecoveryBridge.permissionRequired
                                                 ? "Authorization required"
                                                 : (RecoveryBridge.available
                                                    ? "Recovery is available"
                                                    : "Recovery information unavailable"))
                                        font.pixelSize: 20
                                        font.bold: true
                                    }

                                    Label {
                                        text: RecoveryBridge.permissionRequired
                                              ? "Administrator permission is required to read Snapper snapshots"
                                              : RecoveryBridge.available
                                                ? RecoveryBridge.filesystem + " · "
                                                  + RecoveryBridge.snapshotTool + " · "
                                                  + RecoveryBridge.snapshotCount + " snapshots"
                                                : "No recovery snapshot information is currently available"
                                        opacity: 0.7
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 82
                                    radius: 10

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 3

                                        Label {
                                            text: "Snapshots"
                                            opacity: 0.7
                                        }

                                        Label {
                                            text: RecoveryBridge.available
                                                  ? RecoveryBridge.snapshotCount
                                                  : "N/A"
                                            font.pixelSize: 22
                                            font.bold: true
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 82
                                    radius: 10

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 3

                                        Label {
                                            text: "Current"
                                            opacity: 0.7
                                        }

                                        Label {
                                            text: RecoveryBridge.available
                                                  ? "#" + RecoveryBridge.currentSnapshotId
                                                  : "N/A"
                                            font.pixelSize: 22
                                            font.bold: true
                                        }
                                    }
                                }
                            }

                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                contentWidth: width
                                clip: true

                                ColumnLayout {
                                    width: parent.width
                                    spacing: 12

                                    Label {
                                        text: "Recovery Events"
                                        font.pixelSize: 18
                                        font.bold: true
                                    }

                                    ListView {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(contentHeight, 240)
                                        clip: true
                                        spacing: 8
                                        model: RecoveryBridge.recoveryEvents

                                        delegate: Rectangle {
                                            width: ListView.view.width
                                            height: 104
                                            radius: 10
                                            border.color: "red"
                                            border.width: 2

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 16
                                                spacing: 14

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    spacing: 4

                                                    RowLayout {
                                                        spacing: 8

                                                        Label {
                                                            text: "#" + modelData.preId + " → #" + modelData.postId
                                                            font.bold: true
                                                        }

                                                        Label {
                                                            visible: modelData.important
                                                            text: "Important"
                                                            opacity: 0.8
                                                        }

                                                        Label {
                                                            visible: modelData.zethropolTag !== ""
                                                            text: "Zethropol"
                                                            opacity: 0.8
                                                        }
                                                    }

                                                    Label {
                                                        text: modelData.description !== "" ? modelData.description : "Recovery event"
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }

                                                    Label {
                                                        text: modelData.date !== "" ? modelData.date : "Date unavailable"
                                                        opacity: 0.7
                                                    }
                                                }
                                            }
                                        }

                                        Label {
                                            anchors.centerIn: parent
                                            visible: RecoveryBridge.available && RecoveryBridge.recoveryEvents.length === 0
                                            text: "No recovery events found"
                                            opacity: 0.6
                                        }
                                    }

                                    Label {
                                        text: "Recovery Points"
                                        font.pixelSize: 18
                                        font.bold: true
                                    }

                                    ListView {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(contentHeight, 220)
                                        clip: true
                                        spacing: 8
                                        model: RecoveryBridge.recoveryPoints

                                        delegate: Rectangle {
                                            width: ListView.view.width
                                            height: 88
                                            radius: 10
                                            border.color: "green"
                                            border.width: 2

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 16
                                                spacing: 14

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    spacing: 4

                                                    RowLayout {
                                                        spacing: 8
                                                        Layout.fillWidth: true

                                                        Label {
                                                            text: "#" + modelData.id
                                                            font.bold: true
                                                        }

                                                        Label {
                                                            text: modelData.type
                                                            opacity: 0.7
                                                        }

                                                        Label {
                                                            visible: modelData.important
                                                            text: "Important"
                                                            opacity: 0.8
                                                        }

                                                        Label {
                                                            visible: modelData.zethropolTag !== ""
                                                            text: "Zethropol"
                                                            opacity: 0.8
                                                        }
                                                    }

                                                    Label {
                                                        text: modelData.description !== "" ? modelData.description : "Recovery point"
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }

                                                    Label {
                                                        text: modelData.date !== "" ? modelData.date : "Date unavailable"
                                                        opacity: 0.7
                                                    }
                                                }

                                                Button {
                                                    Layout.preferredWidth: 120
                                                    Layout.alignment: Qt.AlignVCenter
                                                    text: RecoveryBridge.actionBusy ? "Busy..." : "Delete"
                                                    enabled: !RecoveryBridge.actionBusy
                                                    onClicked: RecoveryBridge.deleteRecoveryPoint(modelData.id)
                                                }
                                            }
                                        }
                                        Label {
                                            anchors.centerIn: parent
                                            visible: RecoveryBridge.available && RecoveryBridge.recoveryPoints.length === 0
                                            text: "No standalone recovery points found"
                                            opacity: 0.6
                                        }
                                    }
                                }
                            }

                            Item {
                                Layout.fillHeight: true
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 16

                            Label {
                                text: "Security"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "System security status"
                                opacity: 0.7
                            }

                            GridLayout {
                                columns: 3
                                Layout.fillWidth: true
                                rowSpacing: 12
                                columnSpacing: 12

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Firewall"
                                            font.pixelSize: 18
                                            font.bold: true
                                        }

                                        Label {
                                            text: SecurityBridge.firewall
                                            font.pixelSize: 16
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Secure Boot"
                                            font.pixelSize: 18
                                            font.bold: true
                                        }

                                        Label {
                                            text: SecurityBridge.secureBoot
                                            font.pixelSize: 16
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Kernel Lockdown"
                                            font.pixelSize: 18
                                            font.bold: true
                                        }

                                        Label {
                                            text: SecurityBridge.kernelLockdown
                                            font.pixelSize: 16
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "AppArmor"
                                            font.pixelSize: 18
                                            font.bold: true
                                        }

                                        Label {
                                            text: SecurityBridge.appArmor
                                            font.pixelSize: 16
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "Failed Services"
                                            font.pixelSize: 18
                                            font.bold: true
                                        }

                                        Label {
                                            text: SecurityBridge.failedServices
                                            font.pixelSize: 16
                                            opacity: 0.7
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 6

                                        Label {
                                            text: "LSM"
                                            font.pixelSize: 18
                                            font.bold: true
                                        }

                                        Label {
                                            text: SecurityBridge.lsm.join(", ")
                                            font.pixelSize: 14
                                            opacity: 0.7
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }

                            Item {
                                Layout.fillHeight: true
                            }
                        }
                    }

                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 14

                            Label {
                                text: "Services"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "System services and runtime state"
                                opacity: 0.7
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 4

                                        Label {
                                            text: "Total Services"
                                            font.pixelSize: 16
                                        }

                                        Label {
                                            text: ServiceBridge.total
                                            font.pixelSize: 24
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 4

                                        Label {
                                            text: "Active"
                                            font.pixelSize: 16
                                        }

                                        Label {
                                            text: ServiceBridge.active
                                            font.pixelSize: 24
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    radius: 8
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.centerIn: parent
                                        spacing: 4

                                        Label {
                                            text: "Failed"
                                            font.pixelSize: 16
                                        }

                                        Label {
                                            text: ServiceBridge.failed
                                            font.pixelSize: 24
                                        }
                                    }
                                }
                            }

                            Label {
                                visible: ServiceBridge.actionStatus !== ""
                                text: ServiceBridge.actionStatus
                                opacity: 0.8
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 8
                                model: ServiceBridge.services

                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 108
                                    radius: 8
                                    border.width: 1

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 16

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Label {
                                                text: modelData.name
                                                font.pixelSize: 16
                                                font.bold: true
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: modelData.description
                                                opacity: 0.6
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: modelData.active + " / " + modelData.sub + "  •  " + modelData.enabled
                                                opacity: 0.6
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                        }

                                        RowLayout {
                                            Layout.preferredWidth: 410
                                            spacing: 6

                                            Button {
                                                text: "Start"
                                                enabled: !ServiceBridge.busy && modelData.active !== "active"
                                                onClicked: ServiceBridge.startService(modelData.name)
                                            }

                                            Button {
                                                text: "Stop"
                                                enabled: !ServiceBridge.busy && modelData.active === "active"
                                                onClicked: ServiceBridge.stopService(modelData.name)
                                            }

                                            Button {
                                                text: "Restart"
                                                enabled: !ServiceBridge.busy && modelData.active === "active"
                                                onClicked: ServiceBridge.restartService(modelData.name)
                                            }

                                            Button {
                                                text: "Enable"
                                                enabled: !ServiceBridge.busy && modelData.canEnable
                                                onClicked: ServiceBridge.enableService(modelData.name)
                                            }

                                            Button {
                                                text: "Disable"
                                                enabled: !ServiceBridge.busy && modelData.canDisable
                                                onClicked: ServiceBridge.disableService(modelData.name)
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
                                text: "Applications"
                                font.pixelSize: 32
                            }

                            Label {
                                text: "Installed desktop applications"
                                opacity: 0.7
                            }

                            Label {
                                text: ApplicationBridge.total + " applications detected"
                                opacity: 0.6
                            }


                            Label {
                                visible: ApplicationBridge.removeStatus !== ""
                                text: ApplicationBridge.removeStatus
                                opacity: 0.8
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 8
                                model: ApplicationBridge.applications

                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 78
                                    radius: 8
                                    border.width: 1

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 16

                                        Button {
                                            Layout.preferredWidth: 48
                                            Layout.preferredHeight: 48
                                            flat: true
                                            focusPolicy: Qt.NoFocus
                                            icon.name: modelData.icon
                                            icon.width: 32
                                            icon.height: 32
                                            icon.color: "transparent"
                                            onClicked: {}
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Label {
                                                text: modelData.name
                                                font.pixelSize: 16
                                                font.bold: true
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                text: modelData.genericName !== undefined && modelData.genericName !== "" ? modelData.genericName : modelData.description
                                                opacity: 0.6
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                        }

                                        Label {
                                            text: modelData.launchable ? "Launchable" : "Unavailable"
                                            opacity: 0.6
                                        }

                                        Button {
                                            visible: modelData.removable
                                            text: "Remove"
                                            enabled: !ApplicationBridge.removing
                                            onClicked: removeDialog.open()

                                            Dialog {
                                                id: removeDialog
                                                title: "Remove Application"
                                                modal: true
                                                standardButtons: Dialog.Cancel | Dialog.Ok
                                                width: 400
                                                anchors.centerIn: Overlay.overlay

                                                contentItem: ColumnLayout {
                                                    spacing: 12

                                                    Label {
                                                        text: "Remove <b>" + modelData.name + "</b>?"
                                                        textFormat: Text.RichText
                                                        wrapMode: Text.WordWrap
                                                        Layout.fillWidth: true
                                                    }

                                                    Label {
                                                        text: "Package: " + modelData.package
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                        wrapMode: Text.WordWrap
                                                    }

                                                    Label {
                                                        text: "This will uninstall the package from the system."
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                        wrapMode: Text.WordWrap
                                                    }
                                                }

                                                onAccepted: ApplicationBridge.removeApplication(modelData.package)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
