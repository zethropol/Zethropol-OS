import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: processesPage
    objectName: "processesPage"

    property string viewMode: "flat" // "flat" or "tree"
    property int selectedPid: -1
    property var selectedProcess: null
    property int pendingKillPid: -1
    property string pendingKillName: ""
    property var pendingKillStartTime: 0

    readonly property int colNameWidth: 250
    readonly property int colPidWidth: 80
    readonly property int colCpuWidth: 80
    readonly property int colRamWidth: 100
    readonly property int colUserWidth: 100
    readonly property int colStatusMinWidth: 230
    readonly property int colActionWidth: 100
    readonly property int colSpacing: 8
    readonly property int rowSideMargin: 12
    readonly property int scrollBarReserve: processVerticalScrollBar.visible
                                            ? Math.ceil(processVerticalScrollBar.width)
                                            : 0
    readonly property int tableMinWidth: rowSideMargin * 2 + colNameWidth + colPidWidth + colCpuWidth
                                         + colRamWidth + colUserWidth + colStatusMinWidth + colActionWidth
                                         + colSpacing * 6 + scrollBarReserve

    Connections {
        target: ProcessBridge
        function onProcessRequiresForceKill(pid, name, startTime) {
            pendingKillPid = pid
            pendingKillName = name
            pendingKillStartTime = startTime
            forceKillDialog.open()
        }
        function onStateChanged() {
            if (selectedPid > 0) {
                var details = ProcessBridge.getProcessDetails(selectedPid)
                if (details && details.pid === selectedPid) {
                    selectedProcess = details
                } else {
                    selectedPid = -1
                    selectedProcess = null
                }
            }
        }
    }

    Dialog {
        id: forceKillDialog
        title: "İşlem Sonlanmadı — Zorla Kapatılsın mı?"
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 440
        anchors.centerIn: Overlay.overlay

        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: pendingKillName + " (PID: " + pendingKillPid + ") SIGTERM sinyaline yanıt vermedi ve hâlâ çalışıyor."
                textFormat: Text.PlainText
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Bu işlemi SIGKILL sinyali ile zorla sonlandırmak istiyor musunuz?"
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Uyarı: Zorla sonlandırılan süreçler kaydedilmemiş verilerini kaybedebilir."
                textFormat: Text.PlainText
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        onAccepted: {
            ProcessBridge.forceKillProcess(pendingKillPid, pendingKillStartTime, true)
        }
    }

    Dialog {
        id: directKillConfirmDialog
        title: "Zorla Kapatma Onayı (SIGKILL)"
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 420
        anchors.centerIn: Overlay.overlay

        contentItem: ColumnLayout {
            spacing: 12
            Label {
                text: (selectedProcess ? selectedProcess.name : "") + " (PID: " + selectedPid + ") sürecini hemen zorla sonlandırmak istediğinizden emin misiniz?"
                textFormat: Text.PlainText
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Bu işlem SIGKILL sinyali gönderecektir ve geri alınamaz."
                textFormat: Text.PlainText
                opacity: 0.7
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        onAccepted: {
            if (selectedProcess) {
                ProcessBridge.forceKillProcess(selectedPid, selectedProcess.startTime, true)
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        // Header
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 2
                Label {
                    text: "Processes"
                    font.pixelSize: 32
                    font.bold: true
                }
                Label {
                    text: "Çalışan işlemler, arka plan servisleri ve kaynak yönetimi"
                    opacity: 0.7
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "Yenile"
                icon.name: "view-refresh"
                enabled: !ProcessBridge.actionBusy
                onClicked: ProcessBridge.refresh()
            }
        }

        // Summary Metric Cards
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                color: rootWindow.palette.alternateBase
                radius: 8
                border.width: 1
                border.color: rootWindow.palette.mid

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    Label { text: "Toplam İşlem"; opacity: 0.7; font.pixelSize: 12 }
                    Label { text: String(ProcessBridge.totalProcesses); font.bold: true; font.pixelSize: 18 }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                color: rootWindow.palette.alternateBase
                radius: 8
                border.width: 1
                border.color: rootWindow.palette.mid

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    Label { text: "Kullanıcı / Arka Plan"; opacity: 0.7; font.pixelSize: 12 }
                    Label { text: ProcessBridge.userProcesses + " / " + ProcessBridge.backgroundProcesses; font.bold: true; font.pixelSize: 18 }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                color: rootWindow.palette.alternateBase
                radius: 8
                border.width: 1
                border.color: rootWindow.palette.mid

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    Label { text: "Toplam CPU Tüketimi"; opacity: 0.7; font.pixelSize: 12 }
                    Label { text: ProcessBridge.totalCpuPercent.toFixed(1) + "%"; font.bold: true; font.pixelSize: 18 }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                color: rootWindow.palette.alternateBase
                radius: 8
                border.width: 1
                border.color: rootWindow.palette.mid

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    Label { text: "Fiziksel RAM (RSS)"; opacity: 0.7; font.pixelSize: 12 }
                    Label { text: ProcessBridge.usedMemoryFormatted; font.bold: true; font.pixelSize: 18 }
                }
            }
        }

        // Toolbar
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            TextField {
                id: searchInput
                placeholderText: "İşlem adı, PID veya komut satırı ara..."
                Layout.fillWidth: true
                text: ProcessBridge.searchQuery
                onTextChanged: ProcessBridge.searchQuery = text
            }

            // Filter pills
            ButtonGroup { id: filterGroup }
            Button {
                text: "Tümü"
                checkable: true
                autoExclusive: true
                checked: ProcessBridge.filterType === "all"
                ButtonGroup.group: filterGroup
                onClicked: ProcessBridge.filterType = "all"
            }
            Button {
                text: "Uygulamalar"
                checkable: true
                autoExclusive: true
                checked: ProcessBridge.filterType === "apps"
                ButtonGroup.group: filterGroup
                onClicked: ProcessBridge.filterType = "apps"
            }
            Button {
                text: "Arka Plan"
                checkable: true
                autoExclusive: true
                checked: ProcessBridge.filterType === "background"
                ButtonGroup.group: filterGroup
                onClicked: ProcessBridge.filterType = "background"
            }

            // View Mode
            ButtonGroup { id: viewGroup }
            Button {
                text: "Düz Liste"
                checkable: true
                autoExclusive: true
                checked: processesPage.viewMode === "flat"
                ButtonGroup.group: viewGroup
                onClicked: processesPage.viewMode = "flat"
            }
            Button {
                text: "Ağaç (Tree)"
                checkable: true
                autoExclusive: true
                checked: processesPage.viewMode === "tree"
                ButtonGroup.group: viewGroup
                onClicked: processesPage.viewMode = "tree"
            }

            // Auto-refresh control
            Label { text: "Oto-Yenile:"; opacity: 0.7 }
            Switch {
                checked: ProcessBridge.autoRefresh
                onToggled: ProcessBridge.autoRefresh = checked
            }
        }

        // Status banner
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            color: rootWindow.palette.alternateBase
            radius: 4
            border.width: 1
            border.color: rootWindow.palette.mid
            visible: ProcessBridge.statusMessage !== ""

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                Label {
                    text: ProcessBridge.statusMessage
                    font.pixelSize: 12
                    font.bold: true
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
        }

        // Synchronized Table Area (horizontal scroll on narrow windows prevents column drift)
        Flickable {
            id: tableHorizontalScroll
            objectName: "processTableHorizontalScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: Math.max(width, processesPage.tableMinWidth)
            contentHeight: height
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                width: tableHorizontalScroll.contentWidth
                height: tableHorizontalScroll.height
                spacing: 6

                // Table Header
                Rectangle {
                    id: tableHeader
                    objectName: "processTableHeader"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    color: rootWindow.palette.alternateBase
                    radius: 4
                    border.width: 1
                    border.color: rootWindow.palette.mid

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: processesPage.rowSideMargin
                        anchors.rightMargin: processesPage.rowSideMargin + processesPage.scrollBarReserve
                        spacing: processesPage.colSpacing

                        Button {
                            objectName: "headerColName"
                            text: "İşlem Adı" + (ProcessBridge.sortField === "name" ? (ProcessBridge.sortAscending ? " ▲" : " ▼") : "")
                            flat: true
                            leftPadding: 0
                            rightPadding: 0
                            Layout.preferredWidth: processesPage.colNameWidth
                            Layout.minimumWidth: processesPage.colNameWidth
                            Layout.maximumWidth: processesPage.colNameWidth
                            contentItem: Label {
                                text: parent.text
                                font.bold: true
                                horizontalAlignment: Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                color: rootWindow.palette.text
                            }
                            onClicked: ProcessBridge.sortBy("name")
                        }
                        Button {
                            objectName: "headerColPid"
                            text: "PID" + (ProcessBridge.sortField === "pid" ? (ProcessBridge.sortAscending ? " ▲" : " ▼") : "")
                            flat: true
                            leftPadding: 0
                            rightPadding: 0
                            Layout.preferredWidth: processesPage.colPidWidth
                            Layout.minimumWidth: processesPage.colPidWidth
                            Layout.maximumWidth: processesPage.colPidWidth
                            contentItem: Label {
                                text: parent.text
                                font.bold: true
                                horizontalAlignment: Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                color: rootWindow.palette.text
                            }
                            onClicked: ProcessBridge.sortBy("pid")
                        }
                        Button {
                            objectName: "headerColCpu"
                            text: "CPU %" + (ProcessBridge.sortField === "cpu" ? (ProcessBridge.sortAscending ? " ▲" : " ▼") : "")
                            flat: true
                            leftPadding: 0
                            rightPadding: 0
                            Layout.preferredWidth: processesPage.colCpuWidth
                            Layout.minimumWidth: processesPage.colCpuWidth
                            Layout.maximumWidth: processesPage.colCpuWidth
                            contentItem: Label {
                                text: parent.text
                                font.bold: true
                                horizontalAlignment: Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                color: rootWindow.palette.text
                            }
                            onClicked: ProcessBridge.sortBy("cpu")
                        }
                        Button {
                            objectName: "headerColRam"
                            text: "RAM" + (ProcessBridge.sortField === "memory" ? (ProcessBridge.sortAscending ? " ▲" : " ▼") : "")
                            flat: true
                            leftPadding: 0
                            rightPadding: 0
                            Layout.preferredWidth: processesPage.colRamWidth
                            Layout.minimumWidth: processesPage.colRamWidth
                            Layout.maximumWidth: processesPage.colRamWidth
                            contentItem: Label {
                                text: parent.text
                                font.bold: true
                                horizontalAlignment: Text.AlignLeft
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                color: rootWindow.palette.text
                            }
                            onClicked: ProcessBridge.sortBy("memory")
                        }
                        Label {
                            objectName: "headerColUser"
                            text: "Kullanıcı"
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.preferredWidth: processesPage.colUserWidth
                            Layout.minimumWidth: processesPage.colUserWidth
                            Layout.maximumWidth: processesPage.colUserWidth
                        }
                        Label {
                            objectName: "headerColStatus"
                            text: "Durum / Tür"
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.minimumWidth: processesPage.colStatusMinWidth
                        }
                        Label {
                            objectName: "headerColAction"
                            text: "Eylem"
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.preferredWidth: processesPage.colActionWidth
                            Layout.minimumWidth: processesPage.colActionWidth
                            Layout.maximumWidth: processesPage.colActionWidth
                        }
                    }
                }

                // Process List View
                ListView {
                    id: processListView
                    objectName: "processListView"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: processesPage.viewMode === "tree" ? ProcessBridge.treeProcesses : ProcessBridge.processes

                    delegate: Rectangle {
                        id: rowRect
                        objectName: "processRowDelegate"
                        width: processListView.width
                        height: 44
                        radius: 4
                        color: (processesPage.selectedPid === modelData.pid) ?
                               rootWindow.palette.highlight :
                               (index % 2 === 0 ? rootWindow.palette.base : rootWindow.palette.alternateBase)
                        border.width: (processesPage.selectedPid === modelData.pid) ? 1 : 0
                        border.color: rootWindow.palette.highlight

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                processesPage.selectedPid = modelData.pid
                                processesPage.selectedProcess = ProcessBridge.getProcessDetails(modelData.pid)
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: processesPage.rowSideMargin
                            anchors.rightMargin: processesPage.rowSideMargin + processesPage.scrollBarReserve
                            spacing: processesPage.colSpacing

                            // Name column (with tree indent if in tree mode)
                            RowLayout {
                                objectName: "rowColName"
                                Layout.preferredWidth: processesPage.colNameWidth
                                Layout.minimumWidth: processesPage.colNameWidth
                                Layout.maximumWidth: processesPage.colNameWidth
                                clip: true
                                spacing: 4

                                Item {
                                    visible: processesPage.viewMode === "tree"
                                    Layout.preferredWidth: modelData.depth ? Math.min(modelData.depth * 18, 90) : 0
                                    Layout.preferredHeight: 1
                                }

                                Label {
                                    visible: processesPage.viewMode === "tree" && modelData.depth > 0
                                    text: "└─ "
                                    opacity: 0.5
                                    font.family: "monospace"
                                }

                                Label {
                                    text: modelData.name
                                    font.bold: true
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    elide: Text.ElideRight
                                    color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                                }
                            }

                            // PID
                            Label {
                                objectName: "rowColPid"
                                text: String(modelData.pid)
                                Layout.preferredWidth: processesPage.colPidWidth
                                Layout.minimumWidth: processesPage.colPidWidth
                                Layout.maximumWidth: processesPage.colPidWidth
                                elide: Text.ElideRight
                                font.family: "monospace"
                                color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                            }

                            // CPU%
                            Label {
                                objectName: "rowColCpu"
                                text: modelData.cpuPercent + "%"
                                Layout.preferredWidth: processesPage.colCpuWidth
                                Layout.minimumWidth: processesPage.colCpuWidth
                                Layout.maximumWidth: processesPage.colCpuWidth
                                elide: Text.ElideRight
                                font.bold: modelData.cpuRaw > 0.0
                                color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                            }

                            // RAM
                            Label {
                                objectName: "rowColRam"
                                text: modelData.memoryFormatted
                                Layout.preferredWidth: processesPage.colRamWidth
                                Layout.minimumWidth: processesPage.colRamWidth
                                Layout.maximumWidth: processesPage.colRamWidth
                                elide: Text.ElideRight
                                color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                            }

                            // User
                            Label {
                                objectName: "rowColUser"
                                text: modelData.user
                                Layout.preferredWidth: processesPage.colUserWidth
                                Layout.minimumWidth: processesPage.colUserWidth
                                Layout.maximumWidth: processesPage.colUserWidth
                                elide: Text.ElideRight
                                color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                            }

                            // State / Type & Badges
                            RowLayout {
                                objectName: "rowColStatus"
                                Layout.fillWidth: true
                                Layout.minimumWidth: processesPage.colStatusMinWidth
                                clip: true
                                spacing: 6

                                Label {
                                    text: modelData.state
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 40
                                    opacity: 0.8
                                    elide: Text.ElideRight
                                    color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                                }

                                Rectangle {
                                    visible: modelData.isCritical
                                    Layout.preferredHeight: 20
                                    Layout.preferredWidth: 70
                                    Layout.minimumWidth: 70
                                    radius: 3
                                    color: "#d9534f"
                                    Label {
                                        anchors.centerIn: parent
                                        text: "🛡️ Kritik"
                                        font.pixelSize: 10
                                        color: "white"
                                        font.bold: true
                                    }
                                }

                                Rectangle {
                                    visible: !modelData.canTerminate && !modelData.isCritical
                                    Layout.preferredHeight: 20
                                    Layout.preferredWidth: 75
                                    Layout.minimumWidth: 75
                                    radius: 3
                                    color: "#6c757d"
                                    Label {
                                        anchors.centerIn: parent
                                        text: "🔒 Yetkisiz"
                                        font.pixelSize: 10
                                        color: "white"
                                    }
                                }

                                Rectangle {
                                    visible: modelData.isBackground && !modelData.isCritical
                                    Layout.preferredHeight: 20
                                    Layout.preferredWidth: 70
                                    Layout.minimumWidth: 70
                                    radius: 3
                                    color: "#3a4a58"
                                    Label {
                                        anchors.centerIn: parent
                                        text: "Arka Plan"
                                        font.pixelSize: 10
                                        color: "white"
                                    }
                                }
                            }

                            // Action Button (End Process)
                            Item {
                                objectName: "rowColAction"
                                Layout.preferredWidth: processesPage.colActionWidth
                                Layout.minimumWidth: processesPage.colActionWidth
                                Layout.maximumWidth: processesPage.colActionWidth
                                Layout.preferredHeight: 28

                                Button {
                                    anchors.fill: parent
                                    text: "Kapat"
                                    enabled: modelData.canTerminate && !ProcessBridge.actionBusy
                                    visible: modelData.canTerminate
                                    onClicked: {
                                        ProcessBridge.terminateProcess(modelData.pid, modelData.startTime)
                                    }
                                }
                            }
                        }
                    }

                    ScrollBar.vertical: ScrollBar {
                        id: processVerticalScrollBar
                        objectName: "processVerticalScrollBar"
                        policy: ScrollBar.AsNeeded
                    }
                }
            }
        }

        // Selected Process Inspector & Subprocesses
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            color: rootWindow.palette.alternateBase
            radius: 8
            border.width: 1
            border.color: rootWindow.palette.mid
            visible: processesPage.selectedProcess !== null

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "İşlem Detayları: " + (selectedProcess ? selectedProcess.name : "") + " (PID: " + selectedPid + ")"
                        textFormat: Text.PlainText
                        font.bold: true
                        font.pixelSize: 14
                    }

                    Rectangle {
                        visible: selectedProcess && selectedProcess.isCritical
                        Layout.preferredHeight: 20
                        Layout.preferredWidth: 160
                        radius: 3
                        color: "#d9534f"
                        Label {
                            anchors.centerIn: parent
                            text: "🛡️ Kritik Sistem Süreci (Korumalı)"
                            font.pixelSize: 10
                            color: "white"
                            font.bold: true
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "Sonlandır (SIGTERM)"
                        enabled: selectedProcess && selectedProcess.canTerminate && !ProcessBridge.actionBusy
                        onClicked: {
                            ProcessBridge.terminateProcess(selectedPid, selectedProcess.startTime)
                        }
                    }

                    Button {
                        text: "Zorla Kapat (SIGKILL)"
                        enabled: selectedProcess && selectedProcess.canTerminate && !ProcessBridge.actionBusy
                        onClicked: {
                            directKillConfirmDialog.open()
                        }
                    }
                }

                Label {
                    text: "Komut: " + (selectedProcess ? selectedProcess.cmdline : "")
                    textFormat: Text.PlainText
                    font.family: "monospace"
                    font.pixelSize: 11
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    opacity: 0.8
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    Label {
                        text: "Ebeveyn: " + (selectedProcess ? (selectedProcess.parentName + " (PPID: " + selectedProcess.ppid + ")") : "")
                        font.pixelSize: 12
                        opacity: 0.75
                    }

                    Label {
                        text: "Kullanıcı: " + (selectedProcess ? selectedProcess.user : "") + " (UID: " + (selectedProcess ? selectedProcess.uid : "") + ")"
                        font.pixelSize: 12
                        opacity: 0.75
                    }

                    Label {
                        text: "İş Parçacıkları: " + (selectedProcess ? selectedProcess.numThreads : "")
                        font.pixelSize: 12
                        opacity: 0.75
                    }

                    Label {
                        text: "Alt Süreçler (" + (selectedProcess && selectedProcess.children ? selectedProcess.children.length : 0) + "): " +
                              (selectedProcess && selectedProcess.children && selectedProcess.children.length > 0 ?
                               selectedProcess.children.map(function(c) { return c.name + " (" + c.pid + ")" }).join(", ") : "Yok")
                        font.pixelSize: 12
                        opacity: 0.75
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
