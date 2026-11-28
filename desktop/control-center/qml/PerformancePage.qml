import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: performancePage
    objectName: "performancePage"

    property string pendingProfileId: ""
    property string pendingProfileTitle: ""
    property string pendingProfileSummary: ""

    Dialog {
        id: profileConfirmDialog
        objectName: "profileConfirmDialog"
        title: "Performans Profili Değişikliği Onayı"
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 460
        anchors.centerIn: Overlay.overlay

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: "'" + pendingProfileTitle + "' performans profilini etkinleştirmek istediğinizden emin misiniz?"
                textFormat: Text.PlainText
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: pendingProfileSummary
                textFormat: Text.PlainText
                opacity: 0.8
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: "Değişiklik Polkit yetkilendirmesi ve anlık CPU/GPU termal sınır denetiminden geçirilerek uygulanır. Zethropol sessiz profil dayatması yapmaz."
                textFormat: Text.PlainText
                opacity: 0.7
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        onAccepted: {
            if (pendingProfileId !== "") {
                PerformanceBridge.setProfile(pendingProfileId, true)
                pendingProfileId = ""
            }
        }
    }

    Flickable {
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: perfContent.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: perfContent
            width: parent.width
            spacing: 14

            // Header
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        text: "Performance"
                        font.pixelSize: 32
                    }

                    Label {
                        text: "Performans profilleri, aktif oyun oturumu algılama, Vulkan/DXVK/VKD3D hazırlığı ve canlı donanım telemetrisi"
                        opacity: 0.7
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                BusyIndicator {
                    running: PerformanceBridge.busy
                    visible: running
                    implicitWidth: 26
                    implicitHeight: 26
                }

                Button {
                    text: "Yenile"
                    enabled: !PerformanceBridge.busy
                    onClicked: PerformanceBridge.refresh()
                }
            }

            // Status / Error Banner
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: statusBannerLabel.implicitHeight + 18
                radius: 6
                color: palette.alternateBase
                border.width: 1
                border.color: PerformanceBridge.lastError !== "" ? "#c0392b" : palette.mid

                Label {
                    id: statusBannerLabel
                    anchors.fill: parent
                    anchors.margins: 9
                    text: PerformanceBridge.statusMessage
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                }
            }

            // Thermal Guard Banner
            Rectangle {
                visible: PerformanceBridge.thermalGuardActive
                Layout.fillWidth: true
                implicitHeight: thermalBannerLabel.implicitHeight + 16
                radius: 6
                color: palette.window
                border.width: 1
                border.color: "#c0392b"

                Label {
                    id: thermalBannerLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    text: PerformanceBridge.thermalGuardReason
                    textFormat: Text.PlainText
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
            }

            // Section 1: 4 Performance Profiles
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: "Performans Profilleri"
                    font.pixelSize: 20
                    Layout.fillWidth: true
                }

                Label {
                    text: "CPU Governor: " + PerformanceBridge.cpuGovernor + " · GPU DPM: " + PerformanceBridge.gpuDpmLevel
                    textFormat: Text.PlainText
                    opacity: 0.7
                    font.pixelSize: 12
                }
            }

            GridLayout {
                columns: 2
                Layout.fillWidth: true
                rowSpacing: 10
                columnSpacing: 10

                Repeater {
                    model: PerformanceBridge.profiles

                    delegate: Rectangle {
                        objectName: "profileCard_" + modelData.id
                        color: palette.alternateBase
                        Layout.fillWidth: true
                        implicitHeight: profileCardLayout.implicitHeight + 24
                        radius: 8
                        border.width: PerformanceBridge.activeProfile === modelData.id ? 2 : 1
                        border.color: PerformanceBridge.activeProfile === modelData.id
                                      ? palette.highlight
                                      : (!modelData.allowedNow ? "#c0392b" : palette.mid)

                        ColumnLayout {
                            id: profileCardLayout
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Label {
                                    text: modelData.title
                                    textFormat: Text.PlainText
                                    font.pixelSize: 18
                                    font.bold: true
                                    Layout.fillWidth: true
                                }

                                Label {
                                    visible: PerformanceBridge.activeProfile === modelData.id
                                    text: "AKTİF"
                                    textFormat: Text.PlainText
                                    font.bold: true
                                    font.pixelSize: 12
                                }

                                Button {
                                    objectName: "applyProfile_" + modelData.id
                                    text: PerformanceBridge.activeProfile === modelData.id ? "Seçili" : "Uygula"
                                    enabled: PerformanceBridge.available
                                             && !PerformanceBridge.busy
                                             && PerformanceBridge.activeProfile !== modelData.id
                                             && modelData.allowedNow
                                    onClicked: {
                                        pendingProfileId = modelData.id
                                        pendingProfileTitle = modelData.title
                                        pendingProfileSummary = modelData.description
                                            + " (Hedef CPU governor: " + modelData.targetGovernor
                                            + ", GPU DPM: " + modelData.targetGpuDpm + ")"
                                        profileConfirmDialog.open()
                                    }
                                }
                            }

                            Label {
                                text: modelData.description
                                textFormat: Text.PlainText
                                opacity: 0.8
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Label {
                                text: "Hedef CPU: " + modelData.targetGovernor + " · Hedef GPU DPM: " + modelData.targetGpuDpm
                                      + (modelData.thermalGuardApplies ? " · Termal Korumalı" : "")
                                textFormat: Text.PlainText
                                opacity: 0.65
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Label {
                                visible: !modelData.allowedNow && modelData.blockedReason !== ""
                                text: modelData.blockedReason
                                textFormat: Text.PlainText
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            // Section 2: Active Gaming Session & Vulkan/DXVK/VKD3D Readiness Cards
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                rowSpacing: 12
                columnSpacing: 12

                Rectangle {
                    objectName: "gamingSessionCard"
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: sessionCol.implicitHeight + 24
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        id: sessionCol
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        Label {
                            text: "Aktif Oyun / Oturum Algılama"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Label {
                            text: PerformanceBridge.gamingSessionActive
                                  ? "Durum: Aktif oyun oturumu algılandı (" + PerformanceBridge.detectedGamingProcesses.join(", ") + ")"
                                  : "Durum: Aktif oyun oturumu algılanmadı"
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            text: "Önerilen Profil: " + PerformanceBridge.recommendedProfile.toUpperCase()
                                  + " · Sessiz Dayatma: " + (PerformanceBridge.silentAutoSwitchBlocked ? "Kapalı (Onay Gerekli)" : "Kapalı")
                            textFormat: Text.PlainText
                            opacity: 0.75
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            text: PerformanceBridge.gamingSessionAdvisory
                            textFormat: Text.PlainText
                            opacity: 0.7
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }

                Rectangle {
                    objectName: "gamingReadinessCard"
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: readinessCol.implicitHeight + 24
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        id: readinessCol
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        Label {
                            text: "Vulkan / DXVK / VKD3D Hazırlığı"
                            font.pixelSize: 18
                            font.bold: true
                        }

                        Label {
                            text: (PerformanceBridge.readiness.gpuName || "Bilinmiyor")
                                  + " (" + (PerformanceBridge.readiness.driverName || "none")
                                  + " · Vulkan " + (PerformanceBridge.readiness.vulkanApiVersion || "N/A") + ")"
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            text: "Donanım Sınıfı: " + (PerformanceBridge.readiness.tierLabel || "Bilinmiyor")
                            textFormat: Text.PlainText
                            font.bold: true
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            text: "DXVK: " + (PerformanceBridge.readiness.dxvkAvailable ? "Hazır" : "Eksik")
                                  + " · VKD3D: " + (PerformanceBridge.readiness.vkd3dAvailable ? "Hazır" : "Eksik")
                                  + " · Steam: " + (PerformanceBridge.readiness.steamAvailable ? "Hazır" : "Eksik")
                                  + " · Proton: " + (PerformanceBridge.readiness.protonAvailable ? "Hazır" : "Eksik")
                                  + "\nGamescope: " + (PerformanceBridge.readiness.gamescopeAvailable ? "Hazır" : "Eksik")
                                  + " · MangoHud: " + (PerformanceBridge.readiness.mangohudAvailable ? "Hazır" : "Eksik")
                                  + " · GameMode: " + (PerformanceBridge.readiness.gamemodeAvailable ? "Hazır" : "Eksik")
                            textFormat: Text.PlainText
                            opacity: 0.8
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            visible: PerformanceBridge.readiness.limitations && PerformanceBridge.readiness.limitations.length > 0
                            text: "Kısıtlar: " + (PerformanceBridge.readiness.limitations ? PerformanceBridge.readiness.limitations.join(" | ") : "")
                            textFormat: Text.PlainText
                            opacity: 0.75
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            visible: PerformanceBridge.readiness.missingComponents && PerformanceBridge.readiness.missingComponents.length > 0
                            text: "Eksik İsteğe Bağlı Bileşenler: " + (PerformanceBridge.readiness.missingComponents ? PerformanceBridge.readiness.missingComponents.join(", ") : "")
                            textFormat: Text.PlainText
                            opacity: 0.65
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            text: PerformanceBridge.readiness.anticheatNotice || ""
                            textFormat: Text.PlainText
                            opacity: 0.65
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // Section 3: Live Hardware Performance Telemetry (existing contract preserved)
            Label {
                text: "Canlı Donanım Performansı"
                font.pixelSize: 20
                Layout.topMargin: 4
            }

            GridLayout {
                columns: 2
                Layout.fillWidth: true
                rowSpacing: 12
                columnSpacing: 12

                Rectangle {
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 128
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 6

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
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 128
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 6

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
                            text: typeof MonitoringBridge.state.gpu_vram_used_gb === "number"
                                  && isFinite(MonitoringBridge.state.gpu_vram_used_gb)
                                  && typeof MonitoringBridge.state.gpu_vram_total_gb === "number"
                                  && isFinite(MonitoringBridge.state.gpu_vram_total_gb)
                                  ? "VRAM: " + MonitoringBridge.state.gpu_vram_used_gb.toFixed(2) + " / " + MonitoringBridge.state.gpu_vram_total_gb.toFixed(2) + " GB"
                                  : "VRAM: Unknown"
                            opacity: 0.65
                        }
                    }
                }

                Rectangle {
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 112
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 6

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
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 112
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 6

                        Label { text: "Storage"; font.pixelSize: 20 }
                        Label {
                            text: typeof MonitoringBridge.state.storage_used_gb === "number"
                                  && isFinite(MonitoringBridge.state.storage_used_gb)
                                  && typeof MonitoringBridge.state.storage_capacity_gb === "number"
                                  && isFinite(MonitoringBridge.state.storage_capacity_gb)
                                  ? "Usage: " + MonitoringBridge.state.storage_used_gb.toFixed(1) + " / " + MonitoringBridge.state.storage_capacity_gb.toFixed(1) + " GB"
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
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 128
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 6

                        Label { text: "Network"; font.pixelSize: 20 }
                        Label {
                            text: typeof MonitoringBridge.state.network_download_mbps === "number"
                                  && isFinite(MonitoringBridge.state.network_download_mbps)
                                  ? "Download: " + MonitoringBridge.state.network_download_mbps.toFixed(2) + " Mbps"
                                  : "Download: Unknown"
                            font.pixelSize: 16
                        }
                        Label {
                            text: typeof MonitoringBridge.state.network_upload_mbps === "number"
                                  && isFinite(MonitoringBridge.state.network_upload_mbps)
                                  ? "Upload: " + MonitoringBridge.state.network_upload_mbps.toFixed(2) + " Mbps"
                                  : "Upload: Unknown"
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

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }
    }
}
