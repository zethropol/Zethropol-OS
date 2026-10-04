import QtQuick
import QtQuick.Controls

import org.zethropol.core 1.0

ApplicationWindow {
    visible: true
    width: 1100
    height: 700
    title: "Zethropol OS Control Center"

    property string activeOpId: ""
    property string opStatus: ""
    property string opMessage: ""
    property string serviceSearch: ""
    property string serviceStateFilter: "Tümü"
    property var visibleServices: servicesClient.servicesList

    function updateVisibleServices() {
        const search = serviceSearch.trim().toLowerCase()
        const state = serviceStateFilter
        visibleServices = servicesClient.servicesList.filter(function(service) {
            if (service.load_state === "not-found")
                return false

            if (search !== "" &&
                service.name.toLowerCase().indexOf(search) === -1 &&
                service.description.toLowerCase().indexOf(search) === -1)
                return false

            if (state === "Aktif" && service.active_state !== "active")
                return false

            if (state === "Pasif" && service.active_state !== "inactive")
                return false

            if (state === "Hatalı" && service.active_state !== "failed")
                return false

            return true
        })
    }

    ServicesClient {
        id: servicesClient

        onOperationInitiated: (operationId) => {
            activeOpId = operationId
            opStatus = "running"
            opMessage = "İşlem başlatıldı..."
            statusTimer.start()
        }

        onOperationStatusReceived: (operationId, status, message) => {
            if (activeOpId === operationId) {
                opStatus = status
                opMessage = message
                if (status === "ok" || status === "failed") {
                    statusTimer.stop()
                    if (status === "failed")
                        servicesClient.refreshServices()
                }
            }
        }

        onServicesChanged: {
            updateVisibleServices()
        }

        onErrorOccurred: (message) => {
            opStatus = "failed"
            opMessage = message
            statusTimer.stop()
        }
    }

    Timer {
        id: statusTimer
        interval: 500
        repeat: true
        onTriggered: {
            if (activeOpId !== "") {
                servicesClient.checkOperationStatus(activeOpId)
            }
        }
    }

    function stateText(state) {
        if (state === "active")
            return "Aktif"
        if (state === "inactive")
            return "Pasif"
        if (state === "failed")
            return "Hatalı"
        return state
    }

    function stateColor(state) {
        if (state === "active")
            return "#16803c"
        if (state === "failed")
            return "#b42318"
        return "#6b7280"
    }

    Item {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 24
        anchors.bottomMargin: 24
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        Text {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 32
            text: "Zethropol OS — Sistem Servisleri"
            font.pixelSize: 26
            font.bold: true
        }

        Text {
            anchors.top: parent.top
            anchors.topMargin: 38
            anchors.left: parent.left
            anchors.right: parent.right
            height: 24
            text: "systemd servislerinin durumu ve yönetimi"
            color: "#666666"
            font.pixelSize: 14
        }

        Rectangle {
            anchors.top: parent.top
            anchors.topMargin: 70
            anchors.left: parent.left
            anchors.right: parent.right
            height: activeOpId !== "" ? 58 : 0
            visible: activeOpId !== ""
            radius: 6
            color: opStatus === "running" ? "#fff3cd"
                  : opStatus === "ok" ? "#d1e7dd"
                  : "#f8d7da"

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 12
                verticalAlignment: Text.AlignVCenter
                text: opMessage
                color: "#222222"
                elide: Text.ElideRight
            }
        }

        GroupBox {
            anchors.top: parent.top
            anchors.topMargin: activeOpId !== "" ? 146 : 70
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            title: "Sistem Servisleri Kontrol Paneli"

            Column {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                Row {
                    width: parent.width
                    height: 38
                    spacing: 8

                    TextField {
                        width: parent.width - 170
                        height: parent.height
                        placeholderText: "Servis ara..."
                        text: serviceSearch
                        onTextChanged: {
                            serviceSearch = text
                            updateVisibleServices()
                        }
                    }

                    ComboBox {
                        width: 160
                        height: parent.height
                        model: ["Tümü", "Aktif", "Pasif", "Hatalı"]
                        currentIndex: 0
                        onCurrentTextChanged: {
                            serviceStateFilter = currentText
                            updateVisibleServices()
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 36
                    color: "#eeeeee"
                    radius: 4

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Text { text: "Servis"; width: 210; anchors.verticalCenter: parent.verticalCenter; font.bold: true }
                        Text { text: "Açıklama"; width: 260; font.bold: true }
                        Text { text: "Durum"; width: 90; font.bold: true }
                        Text { text: "Alt durum"; width: 90; font.bold: true }
                        Text { text: "Etkin"; width: 70; font.bold: true }
                        Text { text: "Islem"; width: 220; font.bold: true }
                        }
                }

                ListView {
                    id: servicesView
                    width: parent.width
                    height: Math.max(0, parent.height - 45)
                    clip: true
                    model: visibleServices

                    delegate: Rectangle {
                        width: servicesView.width
                        height: 64
                        color: index % 2 === 0 ? "#fafafa" : "#ffffff"
                          border.color: "#e5e7eb"

                          Row {
                              anchors.verticalCenter: parent.verticalCenter
                              spacing: 10

                            Column {
                                 width: 210
                                 anchors.verticalCenter: parent.verticalCenter
                                 Text {
                                    width: parent.width
                                    text: modelData.name
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    width: parent.width
                                    text: modelData.load_state
                                    color: "#777777"
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            Text {
                                width: 260
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.description
                                elide: Text.ElideRight
                            }

                            Text {
                                width: 90
                                anchors.verticalCenter: parent.verticalCenter
                                text: stateText(modelData.active_state)
                                color: stateColor(modelData.active_state)
                                font.bold: true
                            }

                            Text {
                                width: 90
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.sub_state
                                color: "#666666"
                            }

                            Text {
                                width: 70
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.enabled ? "Evet" : "Hayır"
                                color: modelData.enabled ? "#16803c" : "#6b7280"
                            }

                            Row {
                                width: 220
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Button {
                                    text: "Başlat"
                                    enabled: activeOpId === ""
                                    onClicked: servicesClient.manageService(modelData.name, "start")
                                }

                                Button {
                                    text: "Durdur"
                                    enabled: activeOpId === ""
                                    onClicked: servicesClient.manageService(modelData.name, "stop")
                                }

                                Button {
                                    text: "Yeniden başlat"
                                    enabled: activeOpId === ""
                                    onClicked: servicesClient.manageService(modelData.name, "restart")
                                }
                            }
                        }
                    }
                }
            }
        }

        Button {
            text: "Servis listesini yenile"
            enabled: activeOpId === ""
            onClicked: servicesClient.refreshServices()
        }
    }
}
