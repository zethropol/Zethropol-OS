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
                text: "<b>" + pendingKillName + "</b> (PID: " + pendingKillPid + ") SIGTERM sinyaline yanıt vermedi ve hâlâ çalışıyor."
                textFormat: Text.RichText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Bu işlemi <b>SIGKILL</b> sinyali ile zorla sonlandırmak istiyor musunuz?"
                textFormat: Text.RichText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Uyarı: Zorla sonlandırılan süreçler kaydedilmemiş verilerini kaybedebilir."
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
                text: "<b>" + (selectedProcess ? selectedProcess.name : "") + "</b> (PID: " + selectedPid + ") sürecini hemen zorla sonlandırmak istediğinizden emin misiniz?"
                textFormat: Text.RichText
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Bu işlem SIGKILL sinyali gönderecektir ve geri alınamaz."
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

        // Table Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: rootWindow.palette.alternateBase
            radius: 4
            border.width: 1
            border.color: rootWindow.palette.mid

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Button {
                    text: "İşlem Adı " + (ProcessBridge.sortField === "name" ? (ProcessBridge.sortAscending ? "▲" : "▼") : "")
                    flat: true
                    Layout.preferredWidth: 260
                    onClicked: ProcessBridge.sortBy("name")
                }
                Button {
                    text: "PID " + (ProcessBridge.sortField === "pid" ? (ProcessBridge.sortAscending ? "▲" : "▼") : "")
                    flat: true
                    Layout.preferredWidth: 90
                    onClicked: ProcessBridge.sortBy("pid")
                }
                Button {
                    text: "CPU % " + (ProcessBridge.sortField === "cpu" ? (ProcessBridge.sortAscending ? "▲" : "▼") : "")
                    flat: true
                    Layout.preferredWidth: 90
                    onClicked: ProcessBridge.sortBy("cpu")
                }
                Button {
                    text: "RAM " + (ProcessBridge.sortField === "memory" ? (ProcessBridge.sortAscending ? "▲" : "▼") : "")
                    flat: true
                    Layout.preferredWidth: 110
                    onClicked: ProcessBridge.sortBy("memory")
                }
                Label {
                    text: "Kullanıcı"
                    font.bold: true
                    Layout.preferredWidth: 100
                }
                Label {
                    text: "Durum / Tür"
                    font.bold: true
                    Layout.fillWidth: true
                }
                Label {
                    text: "Eylem"
                    font.bold: true
                    Layout.preferredWidth: 110
                }
            }
        }

        // Process List View
        ListView {
            id: processListView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: processesPage.viewMode === "tree" ? ProcessBridge.treeProcesses : ProcessBridge.processes

            delegate: Rectangle {
                id: rowRect
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
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8

                    // Name column (with tree indent if in tree mode)
                    RowLayout {
                        Layout.preferredWidth: 260
                        spacing: 4

                        Item {
                            visible: processesPage.viewMode === "tree"
                            Layout.preferredWidth: modelData.depth ? (modelData.depth * 18) : 0
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
                            elide: Text.ElideRight
                            color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                        }
                    }

                    // PID
                    Label {
                        text: String(modelData.pid)
                        Layout.preferredWidth: 90
                        font.family: "monospace"
                        color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                    }

                    // CPU%
                    Label {
                        text: modelData.cpuPercent + "%"
                        Layout.preferredWidth: 90
                        font.bold: modelData.cpuRaw > 0.0
                        color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                    }

                    // RAM
                    Label {
                        text: modelData.memoryFormatted
                        Layout.preferredWidth: 110
                        color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                    }

                    // User
                    Label {
                        text: modelData.user
                        Layout.preferredWidth: 100
                        elide: Text.ElideRight
                        color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                    }

                    // State / Type & Badges
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            text: modelData.state
                            opacity: 0.8
                            elide: Text.ElideRight
                            color: (processesPage.selectedPid === modelData.pid) ? rootWindow.palette.highlightedText : rootWindow.palette.text
                        }

                        Rectangle {
                            visible: modelData.isCritical
                            Layout.preferredHeight: 20
                            Layout.preferredWidth: 70
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
                    Button {
                        text: "Kapat"
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: 28
                        enabled: modelData.canTerminate && !ProcessBridge.actionBusy
                        visible: modelData.canTerminate
                        onClicked: {
                            ProcessBridge.terminateProcess(modelData.pid, modelData.startTime)
                        }
                    }

                    Item {
                        visible: !modelData.canTerminate
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: 28
                    }
                }
            }

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
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
                        text: "<b>İşlem Detayları:</b> " + (selectedProcess ? selectedProcess.name : "") + " (PID: " + selectedPid + ")"
                        textFormat: Text.RichText
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
                    text: "<b>Komut:</b> " + (selectedProcess ? selectedProcess.cmdline : "")
                    textFormat: Text.RichText
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
