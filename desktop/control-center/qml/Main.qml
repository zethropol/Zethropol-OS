import Zethropol.ControlCenter
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: rootWindow
    onClosing: function(close) {
        if (UpdateInstallBridge.busy) {
            close.accepted = false
            installationCloseNotice.open()
        }
    }

    Connections {
        target: UpdateInstallBridge
        function onChanged() {
            if (!UpdateInstallBridge.busy)
                installationCloseNotice.close()
        }
    }

    Dialog {
        id: installationCloseNotice
        anchors.centerIn: parent
        title: "Kurulum devam ediyor"
        modal: true
        standardButtons: Dialog.Ok
        width: Math.min(rootWindow.width - 48, 560)
        Label {
            width: parent.width
            text: "Paket işlemi tamamlanana kadar Control Center kapatılamaz. İlerlemeyi ve bekleyen soruları Updates sekmesinden takip edin."
            wrapMode: Text.WordWrap
        }
    }








    QtObject {
        id: notificationState
        property bool previousFirmwareAvailable: false
        property bool previousFailedServices: false
        property bool previousHardwareWarning: false

        Component.onCompleted: {
            previousFirmwareAvailable = String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
            previousFailedServices = ServiceBridge.failed > 0
            previousHardwareWarning = String(SystemState.overallHealth).toUpperCase() === "WARNING"
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
    color: palette.window

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "controlCenterSidebar"
            color: rootWindow.palette.window
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
                    model: ["Overview", "Hardware", "Diagnostics", "System", "Performance", "Power", "Updates", "Recovery", "Security", "Services", "Applications", "Processes", "Compatibility"]

                    delegate: Button {
                        text: modelData
                        Layout.fillWidth: true
                        checkable: true
                        autoExclusive: true
                        checked: contentStack.currentIndex === index
                        highlighted: checked
                        font.bold: checked
                        onClicked: contentStack.currentIndex = index
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }

        Rectangle {
            objectName: "controlCenterContent"
            color: rootWindow.palette.base
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 16

                StackLayout {
                    id: contentStack
                    objectName: "controlCenterPages"
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
                                    model: ["Overall Health", "Hardware Health", "System Activity", "Power"]

                                    delegate: Rectangle {
                                        color: rootWindow.palette.alternateBase
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 125
                                        radius: 8
                                        border.width: 1
                                        border.color: rootWindow.palette.mid

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
                                                                    SystemState.powerAvailability
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
                                        color: rootWindow.palette.alternateBase
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 110
                                        radius: 6
                                        border.width: 1
                                        border.color: rootWindow.palette.mid

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
                                visible: (RecoveryBridge.available && RecoveryBridge.snapshotCount === 0) || ServiceBridge.failed > 0 || String(SystemState.overallHealth).toUpperCase() === "WARNING" || String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
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
                                            color: rootWindow.palette.alternateBase
                                            visible: String(SystemState.firmwareStatus).toLowerCase().indexOf("firmware update available") >= 0
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1
                                            border.color: rootWindow.palette.mid

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
                                            color: rootWindow.palette.alternateBase
                                            visible: RecoveryBridge.available && RecoveryBridge.snapshotCount === 0
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1
                                            border.color: rootWindow.palette.mid

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
                                            color: rootWindow.palette.alternateBase
                                            visible: ServiceBridge.failed > 0
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1
                                            border.color: rootWindow.palette.mid

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
                                            color: rootWindow.palette.alternateBase
                                            visible: String(SystemState.overallHealth).toUpperCase() === "WARNING"
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 72
                                            radius: 8
                                            border.width: 1
                                            border.color: rootWindow.palette.mid

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

                    HardwarePage {}

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 125
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                            text: (SystemState.isKdePlasma ? "Plasma Version: " : "Version: ") + SystemState.desktopVersion
                                            opacity: 0.65
                                        }

                                        Label {
                                            text: "Session: " + SystemState.sessionType
                                            opacity: 0.65
                                        }
                                    }
                                }

                                Rectangle {
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 145
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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

                    PerformancePage {}

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
                                    color: rootWindow.palette.alternateBase
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
                                    color: rootWindow.palette.alternateBase
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
                                    color: rootWindow.palette.alternateBase
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
                                    color: rootWindow.palette.alternateBase
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

                    UpdateReport {
                        scanner: UpdateBridge
                        installer: UpdateInstallBridge
                        onOpenRecoveryRequested: contentStack.currentIndex = 7
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
                                    objectName: "recoveryRefreshButton"
                                    text: RecoveryBridge.checking ? "Checking..." : "Refresh"
                                    enabled: !RecoveryBridge.checking && !RecoveryBridge.actionBusy
                                    onClicked: RecoveryBridge.refresh()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                TextField {
                                    id: recoveryDescriptionField
                                    objectName: "recoveryDescriptionInput"
                                    Layout.fillWidth: true
                                    placeholderText: "Recovery point description"
                                    enabled: !RecoveryBridge.actionBusy && !RecoveryBridge.checking
                                }

                                Button {
                                    objectName: "createRecoveryPointButton"
                                    text: RecoveryBridge.actionBusy
                                          ? "Creating..."
                                          : "Create Recovery Point"
                                    enabled: !RecoveryBridge.actionBusy && !RecoveryBridge.checking
                                    onClicked: RecoveryBridge.requestCreateRecoveryPoint(
                                        recoveryDescriptionField.text)
                                }

                                Button {
                                    objectName: "cancelRecoveryOperationButton"
                                    visible: RecoveryBridge.actionBusy
                                    text: "Cancel"
                                    onClicked: RecoveryBridge.cancelActiveOperation()
                                }
                            }

                            Label {
                                objectName: "recoveryActionStatusLabel"
                                visible: RecoveryBridge.actionStatus !== ""
                                text: RecoveryBridge.actionStatus
                                opacity: 0.8
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }

                            Dialog {
                                id: recoveryCreateConfirmDialog
                                objectName: "recoveryCreateConfirmation"
                                title: "Create Recovery Point"
                                modal: true
                                visible: RecoveryBridge.confirmationPending
                                anchors.centerIn: Overlay.overlay
                                width: 440
                                closePolicy: Popup.NoAutoClose

                                contentItem: ColumnLayout {
                                    spacing: 10

                                    Label {
                                        text: "Create a new Btrfs recovery snapshot for configuration '"
                                              + (RecoveryBridge.configName !== "" ? RecoveryBridge.configName : "root")
                                              + "' (" + RecoveryBridge.filesystem + ")?"
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: "Description: " + RecoveryBridge.pendingDescription
                                        font.bold: true
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: "Administrator authorization (Polkit) is required to create a system snapshot."
                                        opacity: 0.7
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                }

                                footer: DialogButtonBox {
                                    Button {
                                        objectName: "cancelCreateRecoveryPoint"
                                        text: "Cancel"
                                        onClicked: RecoveryBridge.cancelCreateRecoveryPoint()
                                    }

                                    Button {
                                        objectName: "confirmCreateRecoveryPoint"
                                        text: "Create Recovery Point"
                                        enabled: RecoveryBridge.confirmationPending && !RecoveryBridge.actionBusy
                                        onClicked: {
                                            RecoveryBridge.confirmCreateRecoveryPoint()
                                            recoveryDescriptionField.text = ""
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                color: rootWindow.palette.alternateBase
                                Layout.fillWidth: true
                                Layout.preferredHeight: 125
                                radius: 12

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 18
                                    spacing: 6

                                    Label {
                                        objectName: "recoveryStatusTitle"
                                        text: RecoveryBridge.checking
                                              ? "Checking recovery information..."
                                              : (RecoveryBridge.statusTitle !== ""
                                                 ? RecoveryBridge.statusTitle
                                                 : (RecoveryBridge.permissionRequired
                                                    ? "Authorization required"
                                                    : (RecoveryBridge.available
                                                       ? "Recovery is available"
                                                       : "Recovery information unavailable")))
                                        font.pixelSize: 20
                                        font.bold: true
                                    }

                                    Label {
                                        objectName: "recoveryStatusMessage"
                                        text: RecoveryBridge.permissionRequired
                                              ? "Administrator permission is required to read Snapper snapshots"
                                              : RecoveryBridge.available
                                                ? RecoveryBridge.filesystem + " · "
                                                  + RecoveryBridge.snapshotTool + " · "
                                                  + RecoveryBridge.snapshotCount + " snapshots"
                                                : (RecoveryBridge.statusMessage !== ""
                                                   ? RecoveryBridge.statusMessage
                                                   : "No recovery snapshot information is currently available")
                                        opacity: 0.7
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Rectangle {
                                    color: rootWindow.palette.alternateBase
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
                                    color: rootWindow.palette.alternateBase
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
                                        objectName: "recoveryListLoadingLabel"
                                        visible: RecoveryBridge.checking
                                        text: "Loading snapshots..."
                                        opacity: 0.7
                                    }

                                    Label {
                                        objectName: "recoveryListErrorLabel"
                                        visible: !RecoveryBridge.checking && !RecoveryBridge.available
                                        text: (RecoveryBridge.statusTitle !== "" ? RecoveryBridge.statusTitle : "Recovery snapshots unavailable")
                                              + (RecoveryBridge.statusMessage !== "" ? " — " + RecoveryBridge.statusMessage : "")
                                        opacity: 0.75
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: "Recovery Events"
                                        font.pixelSize: 18
                                        font.bold: true
                                    }

                                    Label {
                                        objectName: "recoveryEventsEmptyLabel"
                                        visible: !RecoveryBridge.checking && RecoveryBridge.available && RecoveryBridge.recoveryEvents.length === 0
                                        text: "No recovery events found"
                                        opacity: 0.6
                                    }

                                    ListView {
                                        objectName: "recoveryEventsList"
                                        visible: RecoveryBridge.available && RecoveryBridge.recoveryEvents.length > 0
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(contentHeight, 240)
                                        clip: true
                                        spacing: 8
                                        model: RecoveryBridge.recoveryEvents

                                        delegate: Rectangle {
                                            color: rootWindow.palette.alternateBase
                                            width: ListView.view.width
                                            height: 104
                                            radius: 10
                                            border.color: rootWindow.palette.mid
                                            border.width: 1

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

                                    Label {
                                        objectName: "recoveryPointsEmptyLabel"
                                        visible: !RecoveryBridge.checking && RecoveryBridge.available && RecoveryBridge.recoveryPoints.length === 0
                                        text: "No standalone recovery points found"
                                        opacity: 0.6
                                    }

                                    ListView {
                                        objectName: "recoveryPointsList"
                                        visible: RecoveryBridge.available && RecoveryBridge.recoveryPoints.length > 0
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(Math.max(contentHeight, 88), 220)
                                        clip: true
                                        spacing: 8
                                        model: RecoveryBridge.recoveryPoints

                                        delegate: Rectangle {
                                            color: rootWindow.palette.alternateBase
                                            width: ListView.view.width
                                            height: 88
                                            radius: 10
                                            border.color: rootWindow.palette.mid
                                            border.width: 1

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
                                                    objectName: "preflightRecoveryPointButton"
                                                    Layout.preferredWidth: 110
                                                    Layout.alignment: Qt.AlignVCenter
                                                    text: RecoveryBridge.preflightBusy && RecoveryBridge.selectedRollbackPointId === modelData.id ? "Checking..." : "Preflight"
                                                    enabled: !RecoveryBridge.preflightBusy && !RecoveryBridge.actionBusy
                                                    onClicked: RecoveryBridge.preflightRollback(modelData.id)
                                                }

                                                Button {
                                                    objectName: "deleteRecoveryPointButton"
                                                    Layout.preferredWidth: 120
                                                    Layout.alignment: Qt.AlignVCenter
                                                    text: modelData.deletable === false
                                                          ? "Protected"
                                                          : (RecoveryBridge.actionBusy ? "Busy..." : "Delete")
                                                    enabled: !RecoveryBridge.actionBusy && modelData.deletable !== false
                                                    onClicked: RecoveryBridge.requestDeleteRecoveryPoint(modelData.id)
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

                                    Rectangle {
                                        objectName: "recoveryDeleteConfirmPanel"
                                        visible: RecoveryBridge.deleteConfirmationPending && RecoveryBridge.pendingDeleteSnapshotId > 0
                                        color: rootWindow.palette.alternateBase
                                        Layout.fillWidth: true
                                        implicitHeight: deleteConfirmColumn.implicitHeight + 24
                                        radius: 10
                                        border.width: 1
                                        border.color: rootWindow.palette.mid

                                        ColumnLayout {
                                            id: deleteConfirmColumn
                                            anchors.fill: parent
                                            anchors.margins: 12
                                            spacing: 8

                                            Label {
                                                text: "Confirm deletion of recovery point #" + RecoveryBridge.pendingDeleteSnapshotId
                                                textFormat: Text.PlainText
                                                font.bold: true
                                                wrapMode: Text.WordWrap
                                                Layout.fillWidth: true
                                            }

                                            Label {
                                                text: RecoveryBridge.pendingDeleteDescription !== ""
                                                      ? RecoveryBridge.pendingDeleteDescription
                                                      : "This standalone manual recovery snapshot will be permanently deleted after administrator authorization."
                                                textFormat: Text.PlainText
                                                opacity: 0.8
                                                wrapMode: Text.WordWrap
                                                Layout.fillWidth: true
                                            }

                                            RowLayout {
                                                spacing: 8
                                                Button {
                                                    objectName: "confirmDeleteRecoveryPointButton"
                                                    text: "Confirm Delete"
                                                    enabled: !RecoveryBridge.actionBusy
                                                    onClicked: RecoveryBridge.confirmDeleteRecoveryPoint()
                                                }
                                                Button {
                                                    objectName: "cancelDeleteRecoveryPointButton"
                                                    text: "Cancel"
                                                    enabled: !RecoveryBridge.actionBusy
                                                    onClicked: RecoveryBridge.cancelDeleteRecoveryPoint()
                                                }
                                            }
                                        }
                                    }

                                    Rectangle {
                                        objectName: "recoveryPreflightPanel"
                                        visible: RecoveryBridge.preflightBusy || RecoveryBridge.selectedRollbackPointId > 0
                                        color: rootWindow.palette.alternateBase
                                        Layout.fillWidth: true
                                        implicitHeight: preflightColumn.implicitHeight + 28
                                        radius: 10
                                        border.width: 1
                                        border.color: rootWindow.palette.mid

                                        ColumnLayout {
                                            id: preflightColumn
                                            anchors.fill: parent
                                            anchors.margins: 14
                                            spacing: 8

                                            Label {
                                                objectName: "recoveryPreflightStatusLabel"
                                                font.pixelSize: 16
                                                font.bold: true
                                                text: RecoveryBridge.preflightBusy
                                                      ? "Running rollback preflight for #" + RecoveryBridge.selectedRollbackPointId + "..."
                                                      : ("Rollback Preflight (#" + RecoveryBridge.selectedRollbackPointId + "): "
                                                         + (RecoveryBridge.preflightResult.overallStatus === "ready"
                                                            ? "Ready (Preflight Only)"
                                                            : (RecoveryBridge.preflightResult.overallStatus === "unknown"
                                                               ? "Incomplete / Unknown"
                                                               : "Blocked")))
                                            }

                                            Label {
                                                objectName: "recoveryPreflightSummaryLabel"
                                                text: RecoveryBridge.preflightResult.summary || ""
                                                wrapMode: Text.Wrap
                                                Layout.fillWidth: true
                                                opacity: 0.85
                                            }

                                            Label {
                                                visible: !RecoveryBridge.preflightBusy
                                                text: "Rollback execution is disabled in this verification stage. Explicit confirmation and administrator authorization are required."
                                                wrapMode: Text.Wrap
                                                Layout.fillWidth: true
                                                opacity: 0.65
                                                font.pixelSize: 12
                                            }

                                            Repeater {
                                                model: RecoveryBridge.preflightResult.checks || []
                                                delegate: ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        text: "[" + String(modelData.status || "").toUpperCase() + "] " + (modelData.title || "")
                                                        font.bold: true
                                                        wrapMode: Text.Wrap
                                                        Layout.fillWidth: true
                                                    }

                                                    Label {
                                                        text: (modelData.reason || "")
                                                              + (modelData.status !== "pass" && modelData.remediation ? " — " + modelData.remediation : "")
                                                        opacity: 0.75
                                                        wrapMode: Text.Wrap
                                                        Layout.fillWidth: true
                                                        font.pixelSize: 12
                                                    }
                                                }
                                            }
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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 120
                                    radius: 10
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    text: "Services"
                                    font.pixelSize: 32
                                    Layout.fillWidth: true
                                }

                                Button {
                                    text: "Yenile"
                                    enabled: !ServiceBridge.busy && !ServiceBridge.failureDetailsBusy
                                    onClicked: ServiceBridge.refresh()
                                }
                            }

                            Label {
                                text: "System services and runtime state"
                                opacity: 0.7
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Rectangle {
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                    color: rootWindow.palette.alternateBase
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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

                            Frame {
                                objectName: "failedServicesPanel"
                                visible: ServiceBridge.failed > 0
                                Layout.fillWidth: true
                                padding: 12

                                background: Rectangle {
                                    color: rootWindow.palette.alternateBase
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.highlight
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: 8

                                    Label {
                                        text: "Hata veren servisler"
                                        font.bold: true
                                        Layout.fillWidth: true
                                    }

                                    ListView {
                                        objectName: "failedServicesList"
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(ServiceBridge.failed * 112, 224)
                                        clip: true
                                        spacing: 8
                                        model: ServiceBridge.failedServices
                                        ScrollBar.vertical: ScrollBar { }

                                        delegate: Column {
                                            width: ListView.view.width
                                            spacing: 2

                                            Label {
                                                objectName: "failedServiceName"
                                                text: modelData.name
                                                width: parent.width
                                                font.bold: true
                                                wrapMode: Text.WrapAnywhere
                                            }

                                            Label {
                                                text: modelData.active + " / " + modelData.sub
                                                width: parent.width
                                                wrapMode: Text.WrapAnywhere
                                            }

                                            Label {
                                                text: modelData.description
                                                visible: text.length > 0
                                                width: parent.width
                                                opacity: 0.7
                                                wrapMode: Text.WordWrap
                                            }

                                            Button {
                                                objectName: "showServiceFailureDetails"
                                                text: "Hata ayrıntıları"
                                                enabled: !ServiceBridge.failureDetailsBusy
                                                onClicked: ServiceBridge.loadFailureDetails(modelData.name)
                                            }
                                        }
                                    }
                                }
                            }

                            Frame {
                                objectName: "serviceFailureDetailsPanel"
                                visible: ServiceBridge.failureDetailsName.length > 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.min(240, rootWindow.height * 0.3)
                                padding: 12

                                background: Rectangle {
                                    color: rootWindow.palette.alternateBase
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: 6

                                    RowLayout {
                                        Layout.fillWidth: true

                                        Label {
                                            text: "Hata ayrıntıları — " + ServiceBridge.failureDetailsName
                                            font.bold: true
                                            Layout.fillWidth: true
                                            wrapMode: Text.WrapAnywhere
                                        }

                                        BusyIndicator {
                                            running: ServiceBridge.failureDetailsBusy
                                            visible: running
                                            Layout.preferredWidth: 24
                                            Layout.preferredHeight: 24
                                        }
                                    }

                                    ScrollView {
                                        id: serviceFailureScroll
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                                        TextArea {
                                            objectName: "serviceFailureDetailsText"
                                            text: ServiceBridge.failureDetailsText
                                            textFormat: TextEdit.PlainText
                                            readOnly: true
                                            selectByMouse: true
                                            wrapMode: TextEdit.WrapAnywhere
                                            font.family: "monospace"
                                            color: rootWindow.palette.text
                                            width: serviceFailureScroll.availableWidth
                                            background: null
                                        }
                                    }
                                }
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 8
                                model: ServiceBridge.services

                                delegate: Rectangle {
                                    color: rootWindow.palette.alternateBase
                                    width: ListView.view.width
                                    height: 108
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                                enabled: !ServiceBridge.busy && (modelData.canStart !== undefined ? modelData.canStart : modelData.active !== "active")
                                                onClicked: ServiceBridge.startService(modelData.name)
                                            }

                                            Button {
                                                text: "Stop"
                                                enabled: !ServiceBridge.busy && (modelData.canStop !== undefined ? modelData.canStop : modelData.active === "active")
                                                onClicked: ServiceBridge.stopService(modelData.name)
                                            }

                                            Button {
                                                text: "Restart"
                                                enabled: !ServiceBridge.busy && (modelData.canRestart !== undefined ? modelData.canRestart : modelData.active === "active")
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
                                    color: rootWindow.palette.alternateBase
                                    width: ListView.view.width
                                    height: 78
                                    radius: 8
                                    border.width: 1
                                    border.color: rootWindow.palette.mid

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
                                            visible: modelData.removable && !modelData.protected
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
                                                        text: "Remove " + modelData.name + "?"
                                                        textFormat: Text.PlainText
                                                        font.bold: true
                                                        wrapMode: Text.WordWrap
                                                        Layout.fillWidth: true
                                                    }

                                                    Label {
                                                        text: "Package: " + modelData.package
                                                        textFormat: Text.PlainText
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                        wrapMode: Text.WordWrap
                                                    }

                                                    Label {
                                                        text: "This will uninstall the package from the system."
                                                        textFormat: Text.PlainText
                                                        opacity: 0.6
                                                        Layout.fillWidth: true
                                                        wrapMode: Text.WordWrap
                                                    }
                                                }

                                                onAccepted: ApplicationBridge.removeApplication(modelData.package, true)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    ProcessesPage {}

                    CompatibilityPage {}
                }
            }
        }
    }
}
