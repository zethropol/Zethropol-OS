import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: reportPage
    objectName: "updateReportPage"
    required property var scanner
    required property var installer
    readonly property color reportText: reportPalette.text
    property string pendingInstallScope: "all"

    SystemPalette {
        id: reportPalette
        colorGroup: SystemPalette.Active
    }

    Connections {
        target: installer
        function onChanged() {
            installationInput.clear()
            if (installer.busy) {
                Qt.callLater(function() { installationInput.forceActiveFocus() })
            } else if ((installer.status === "error" || installer.status === "unknown" || installer.status === "blocked")
                       && installer.output.length > 0) {
                showInstallationOutput.checked = true
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                color: reportPage.reportText
                text: "Güncelleme raporu"
                font.pixelSize: 28
                Layout.fillWidth: true
            }

            Button {
                text: scanner.busy ? "Taranıyor…" : "Taramayı yenile"
                enabled: !scanner.busy && !installer.busy
                onClicked: scanner.scan()
            }

            Button {
                objectName: "startOfficialUpdateInstallation"
                text: "Resmî Depolar (-Syu)…"
                visible: installer.officialUpdatesAvailable
                enabled: !scanner.busy && !installer.busy && installer.officialRepoReady && installer.officialUpdatesAvailable
                onClicked: {
                    reportPage.pendingInstallScope = "official"
                    installConfirmation.open()
                }
            }

            Button {
                objectName: "startAurUpdateInstallation"
                text: "AUR…"
                visible: installer.aurUpdatesAvailable
                enabled: !scanner.busy && !installer.busy && installer.officialRepoReady && !installer.officialUpdatesAvailable && installer.aurUpdatesAvailable
                onClicked: {
                    reportPage.pendingInstallScope = "aur"
                    installConfirmation.open()
                }
            }

            Button {
                objectName: "startFlatpakUpdateInstallation"
                text: "Flatpak…"
                visible: installer.flatpakUpdatesAvailable
                enabled: !scanner.busy && !installer.busy && installer.flatpakUpdatesAvailable
                onClicked: {
                    reportPage.pendingInstallScope = "flatpak"
                    installConfirmation.open()
                }
            }

            Button {
                objectName: "startUpdateInstallation"
                text: installer.busy ? "Kurulum sürüyor…" : "Güncellemeleri kur…"
                enabled: !scanner.busy && !installer.busy && scanner.hasUpdates && installer.officialRepoReady
                onClicked: {
                    reportPage.pendingInstallScope = "all"
                    installConfirmation.open()
                }
            }
        }

        Label {
            color: reportPage.reportText
            text: "Resmî depolar (pacman/libalpm) · AUR (ayrı) · Flatpak (ayrı) · Yetim paket tespiti (salt-okunur)"
            opacity: 0.65
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true

            BusyIndicator {
                running: scanner.busy
                visible: running
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }

            Label {
                color: reportPage.reportText
                objectName: "updateReportSummary"
                text: scanner.summary
                font.pixelSize: 18
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        Label {
            color: reportPage.reportText
            text: scanner.completedAt.length ? "Son tarama: " + scanner.completedAt
                                            : "Control Center açılışında otomatik tarama"
            opacity: 0.6
        }

        Label {
            objectName: "updateInstallationStatus"
            color: reportPage.reportText
            text: installer.message
            visible: text.length > 0
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        Label {
            objectName: "officialRepoGateNotice"
            color: reportPage.reportText
            text: "Resmî depo taraması tamamlanmadan kısmi veya kör sistem yükseltmesi başlatılamaz. Taramayı yenileyin."
            visible: !scanner.busy && scanner.hasUpdates && !installer.officialRepoReady
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.85
        }

        ColumnLayout {
            objectName: "postUpdateAssessmentPanel"
            visible: !installer.busy && installer.healthStatus.length > 0
            Layout.fillWidth: true
            spacing: 4

            Label {
                objectName: "postUpdateHealthSummary"
                color: reportPage.reportText
                text: "Sistem sağlığı (" + ({"healthy": "Sağlıklı", "degraded": "Bozulmuş",
                                           "unknown": "Doğrulanamadı", "blocked": "Engelli"}[installer.healthStatus] || installer.healthStatus)
                      + "): " + installer.healthSummary
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.85
            }

            Label {
                objectName: "postUpdateCleanupSummary"
                color: reportPage.reportText
                text: "Temizlik durumu (" + installer.cleanupStatus + "): " + installer.cleanupSummary
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.75
            }

            Label {
                objectName: "postUpdateRecoveryGuidance"
                color: reportPage.reportText
                visible: installer.recoveryGuidance.length > 0
                text: "Kurtarma rehberliği: " + installer.recoveryGuidance
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.85
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    objectName: "postUpdateRebootNotice"
                    color: reportPage.reportText
                    text: "Yeniden başlatma (" + installer.rebootStatus + "): " + installer.rebootSafetyNotice
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.85
                }

                Button {
                    objectName: "requestUpdateRebootButton"
                    text: "Yeniden Başlat…"
                    visible: !installer.busy && installer.rebootRequired && installer.healthStatus === "healthy"
                    onClicked: rebootConfirmation.open()
                }
            }
        }

        ScrollView {
            id: reportScroll
            visible: !installer.busy
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            clip: true

            ColumnLayout {
                width: reportScroll.availableWidth
                spacing: 16

                Repeater {
                    model: scanner.reports

                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: sourceReport.implicitHeight + 32
                        radius: 8
                        color: reportPalette.alternateBase
                        border.width: 1
                        border.color: reportPalette.mid

                        ColumnLayout {
                            id: sourceReport
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 16
                            spacing: 10

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    color: reportPage.reportText
                                    text: modelData.title
                                    font.pixelSize: 20
                                    font.bold: true
                                    Layout.fillWidth: true
                                }

                                Label {
                                    color: reportPage.reportText
                                    text: ({"pending": "Bekliyor", "scanning": "Taranıyor",
                                            "complete": "Tamamlandı", "disabled": "Kapalı",
                                            "error": "Tamamlanamadı", "warning": "Uyarı"})[modelData.state] || ""
                                    font.bold: true
                                    opacity: 0.7
                                }
                            }

                            Label {
                                color: reportPage.reportText
                                text: modelData.detail
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                wrapMode: Text.WrapAnywhere
                                opacity: 0.75
                            }

                            Repeater {
                                model: modelData.packages

                                delegate: ColumnLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 4

                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: 1
                                        color: reportPalette.mid
                                        opacity: 0.4
                                    }

                                    Label {
                                        color: reportPage.reportText
                                        text: modelData.name
                                        textFormat: Text.PlainText
                                        font.bold: true
                                        Layout.fillWidth: true
                                        wrapMode: Text.WrapAnywhere
                                    }

                                    Label {
                                        color: reportPage.reportText
                                        text: modelData.currentVersion.length
                                              ? modelData.currentVersion + " → " + modelData.newVersion
                                              : modelData.newVersion.length ? "Yeni sürüm: " + modelData.newVersion
                                                                            : "Sürüm bilgisi bildirilmedi."
                                        textFormat: Text.PlainText
                                        Layout.fillWidth: true
                                        wrapMode: Text.WrapAnywhere
                                        opacity: 0.7
                                    }

                                    Label {
                                        color: reportPage.reportText
                                        text: modelData.identifier
                                        visible: text.length > 0
                                        textFormat: Text.PlainText
                                        Layout.fillWidth: true
                                        wrapMode: Text.WrapAnywhere
                                        opacity: 0.55
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    objectName: "orphanPackagesPanel"
                    visible: scanner.orphanCount > 0
                    Layout.fillWidth: true
                    implicitHeight: orphanColumn.implicitHeight + 32
                    radius: 8
                    color: reportPalette.alternateBase
                    border.width: 1
                    border.color: reportPalette.mid

                    ColumnLayout {
                        id: orphanColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        spacing: 8

                        Label {
                            color: reportPage.reportText
                            text: "Yetim paketler (" + scanner.orphanCount + " — Yalnızca tespit)"
                            textFormat: Text.PlainText
                            font.pixelSize: 18
                            font.bold: true
                            Layout.fillWidth: true
                        }

                        Label {
                            color: reportPage.reportText
                            text: "Yetim paketler otomatik kaldırılmaz. Kaldırma işlemi ayrı açık onay ve yönetici yetkisi gerektirir."
                            textFormat: Text.PlainText
                            opacity: 0.75
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            color: reportPage.reportText
                            text: scanner.orphanPackages.join(", ")
                            textFormat: Text.PlainText
                            font.family: "monospace"
                            Layout.fillWidth: true
                            wrapMode: Text.WrapAnywhere
                            opacity: 0.85
                        }
                    }
                }
            }
        }

        CheckBox {
            id: showInstallationOutput
            text: "Kurulum çıktısını göster"
            visible: !installer.busy && installer.output.length > 0
        }

        ScrollView {
            id: installationScroll
            visible: installer.busy || showInstallationOutput.checked
            Layout.fillWidth: true
            Layout.fillHeight: installer.busy
            Layout.preferredHeight: installer.busy ? -1 : 240
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            clip: true

            TextArea {
                objectName: "updateInstallationOutput"
                text: installer.output
                color: reportPage.reportText
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WrapAnywhere
                font.family: "monospace"
                width: installationScroll.availableWidth
                onTextChanged: cursorPosition = length
                background: Rectangle {
                    color: reportPalette.base
                    border.color: reportPalette.mid
                    radius: 6
                }
            }
        }

        RowLayout {
            visible: installer.busy
            Layout.fillWidth: true

            TextField {
                id: installationInput
                objectName: "updateInstallationInput"
                Layout.fillWidth: true
                placeholderText: "Yanıt veya parola"
                echoMode: maskInputToggle.checked ? TextInput.Password : TextInput.Normal
                inputMethodHints: Qt.ImhHiddenText | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
                maximumLength: 4096
                onAccepted: reportPage.sendInstallationInput(false)
            }

            CheckBox {
                id: maskInputToggle
                text: "Gizle"
                checked: true
            }

            Button {
                objectName: "sendUpdateInstallationInput"
                text: "Gönder"
                onClicked: reportPage.sendInstallationInput(false)
            }

            Button {
                text: "Tuş gönder"
                enabled: installationInput.text.length === 1
                onClicked: reportPage.sendInstallationInput(true)
            }

            Button {
                text: "Esc"
                onClicked: {
                    installationInput.clear()
                    installer.sendKey("\u001b")
                    installationInput.forceActiveFocus()
                }
            }

            Button {
                objectName: "cancelRunningUpdateButton"
                text: "Durdur"
                onClicked: installer.cancel()
            }
        }

        Label {
            color: reportPage.reportText
            visible: installer.busy
            text: "Satır yanıtları için Gönder’i, tek tuş isteyen ekranlar için Tuş gönder’i kullanın. Parola ve yanıtlar rapora kaydedilmez."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
        }

        Label {
            color: reportPage.reportText
            text: installer.busy ? "Kurulum tamamlanana kadar Control Center’ı açık tutun."
                                 : "Açılışta yalnızca tarama yapılır. Resmî depo, AUR ve Flatpak işlemleri ayrı onaylarla yürütülür."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
        }
    }

    function sendInstallationInput(singleKey) {
        const answer = installationInput.text
        installationInput.clear()
        if (singleKey)
            installer.sendKey(answer)
        else
            installer.sendInput(answer)
        installationInput.forceActiveFocus()
    }

    Dialog {
        id: installConfirmation
        objectName: "updateInstallationConfirmation"
        anchors.centerIn: parent
        width: Math.min(reportPage.width - 24, 680)
        modal: true
        background: Rectangle {
            color: reportPalette.window
            border.color: reportPalette.mid
            radius: 8
        }
        header: Label {
            color: reportPage.reportText
            text: reportPage.pendingInstallScope === "official"
                  ? "Resmî depo tam yükseltmesi (pacman -Syu)"
                  : (reportPage.pendingInstallScope === "aur"
                     ? "AUR güncellemelerini kur"
                     : (reportPage.pendingInstallScope === "flatpak"
                        ? "Flatpak güncellemelerini kur"
                        : "Güncellemeleri kur"))
            font.pixelSize: 22
            padding: 16
        }

        contentItem: ColumnLayout {
            spacing: 16
            Label {
                color: reportPage.reportText
                text: scanner.summary
                textFormat: Text.PlainText
                font.bold: true
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                color: reportPage.reportText
                text: reportPage.pendingInstallScope === "official"
                      ? "Yalnızca resmî depolar için tam sistem yükseltmesi (pacman -Syu) Recovery ön snapshot kapısından geçirilerek yürütülecek. AUR, Flatpak ve yetim paket kaldırma bu oturumda çalıştırılmaz."
                      : (reportPage.pendingInstallScope === "aur"
                         ? "Yalnızca AUR paketleri ayrı oturumda güncellenecek (resmî depolar güncel doğrulandı)."
                         : (reportPage.pendingInstallScope === "flatpak"
                            ? "Yalnızca Flatpak uygulamaları yönetici yetkisi olmadan ayrı oturumda güncellenecek."
                            : "Güncellemeler Recovery güvenlik ön-kapısı doğrulanarak Control Center üzerinde yürütülecek."))
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                color: reportPage.reportText
                text: "Yetim paketler otomatik kaldırılmaz ve sistem otomatik yeniden başlatılmaz. İşlem bitince rapor otomatik yenilenir."
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        footer: DialogButtonBox {
            Button {
                objectName: "cancelUpdateInstallation"
                text: "Vazgeç"
                onClicked: installConfirmation.close()
            }
            Button {
                objectName: "confirmUpdateInstallation"
                text: "Kurulumu başlat"
                enabled: !scanner.busy && !installer.busy
                         && (reportPage.pendingInstallScope === "flatpak"
                             ? installer.flatpakUpdatesAvailable
                             : (scanner.hasUpdates && installer.officialRepoReady))
                onClicked: {
                    installConfirmation.close()
                    if (reportPage.pendingInstallScope === "all")
                        installer.start()
                    else
                        installer.startScoped(reportPage.pendingInstallScope)
                }
            }
        }
    }

    Dialog {
        id: rebootConfirmation
        objectName: "updateRebootConfirmation"
        anchors.centerIn: parent
        width: Math.min(reportPage.width - 24, 520)
        modal: true
        background: Rectangle {
            color: reportPalette.window
            border.color: reportPalette.mid
            radius: 8
        }
        header: Label {
            color: reportPage.reportText
            text: "Yeniden Başlatma Onayı"
            font.pixelSize: 20
            padding: 16
        }
        contentItem: Label {
            color: reportPage.reportText
            text: "Güncelleme sonrası sağlık kontrolleri geçti. Sistemi şimdi yeniden başlatmak istediğinizi onaylayın (yönetici yetkilendirmesi gerekir)."
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        footer: DialogButtonBox {
            Button {
                text: "Vazgeç"
                onClicked: rebootConfirmation.close()
            }
            Button {
                objectName: "confirmUpdateRebootButton"
                text: "Yeniden Başlat"
                onClicked: {
                    rebootConfirmation.close()
                    if (typeof PowerBridge !== "undefined" && PowerBridge)
                        PowerBridge.reboot()
                }
            }
        }
    }
}
