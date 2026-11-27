import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: compatibilityPage
    objectName: "compatibilityPage"

    property string pendingRemoveProfile: ""
    property string selectedTargetPath: ""
    property string selectedRunProfile: "default-win64"
    property string selectedRenderer: "auto"
    property bool sandboxEnabled: true

    function profileModelList() {
        var names = []
        var list = CompatibilityBridge.profiles || []
        for (var i = 0; i < list.length; ++i) {
            if (list[i] && list[i].profile)
                names.push(String(list[i].profile))
        }
        if (names.indexOf("default-win64") < 0)
            names.push("default-win64")
        if (names.indexOf("classic-win32") < 0)
            names.push("classic-win32")
        return names
    }

    FileDialog {
        id: targetFileDialog
        title: "Windows Uygulaması veya Kurulum Dosyası Seç"
        nameFilters: ["Windows Uygulamaları (*.exe *.msi *.lnk)", "Tüm Dosyalar (*)"]
        onAccepted: {
            selectedTargetPath = CompatibilityBridge.localPathFromUrl(selectedFile)
        }
    }

    Dialog {
        id: initProfileDialog
        title: "Yeni İzole Wine Profili Oluştur"
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 460
        anchors.centerIn: Overlay.overlay

        onOpened: {
            newProfileNameField.text = "default-win64"
            newTemplateBox.currentIndex = 0
            newRendererBox.currentIndex = 0
            newSandboxCheck.checked = true
        }

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: "Profil Adı (küçük harf, rakam, '-' ve '_'):"
                textFormat: Text.PlainText
            }

            TextField {
                id: newProfileNameField
                Layout.fillWidth: true
                placeholderText: "ör. default-win64, classic-win32, ofis"
            }

            Label {
                text: "Profil Şablonu:"
                textFormat: Text.PlainText
            }

            ComboBox {
                id: newTemplateBox
                Layout.fillWidth: true
                model: ["default-win64", "classic-win32"]
                onCurrentTextChanged: {
                    if (newProfileNameField.text === "default-win64" || newProfileNameField.text === "classic-win32" || newProfileNameField.text === "")
                        newProfileNameField.text = currentText
                    if (currentText === "classic-win32")
                        newRendererBox.currentIndex = 2
                    else
                        newRendererBox.currentIndex = 0
                }
            }

            Label {
                text: "Varsayılan Grafik Motoru:"
                textFormat: Text.PlainText
            }

            ComboBox {
                id: newRendererBox
                Layout.fillWidth: true
                model: ["auto", "dxvk", "wined3d"]
            }

            CheckBox {
                id: newSandboxCheck
                checked: true
                text: "bubblewrap (bwrap) yazma izolasyonu varsayılan aktif"
            }

            Label {
                text: "Oluşturulan profilde Z:\\ kök bağı kaldırılır, tr_TR.UTF-8 yerel ayarı ve Türkçe yazı tipi eşlemesi otomatik uygulanır."
                textFormat: Text.PlainText
                opacity: 0.7
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        onAccepted: {
            CompatibilityBridge.initProfile(
                newProfileNameField.text.trim(),
                newTemplateBox.currentText,
                newRendererBox.currentText,
                newSandboxCheck.checked,
                false
            )
        }
    }

    Dialog {
        id: removeProfileDialog
        title: "Profil Silme Onayı"
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 440
        anchors.centerIn: Overlay.overlay

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                text: "'" + pendingRemoveProfile + "' profilini kalıcı olarak silmek istediğinizden emin misiniz?"
                textFormat: Text.PlainText
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Label {
                text: "Bu işlem yalnızca bu profile ait yalıtılmış prefix dizinini ve Zethropol tarafından oluşturulmuş .desktop kısayollarını kaldırır. Diğer kullanıcı dosyalarınıza dokunulmaz."
                textFormat: Text.PlainText
                opacity: 0.7
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        onAccepted: {
            if (pendingRemoveProfile !== "") {
                CompatibilityBridge.removeProfile(pendingRemoveProfile, true)
                pendingRemoveProfile = ""
            }
        }
    }

    Flickable {
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: mainColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: mainColumn
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
                        text: "Windows Uyumluluğu"
                        font.pixelSize: 32
                    }

                    Label {
                        text: "Yalıtılmış Wine profilleri, Türkçe yerel ayar/yazı tipi ve güvenli .exe/.msi çalıştırıcı"
                        opacity: 0.7
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                BusyIndicator {
                    running: CompatibilityBridge.busy || CompatibilityBridge.launching
                    visible: running
                    implicitWidth: 28
                    implicitHeight: 28
                }

                Button {
                    text: "Yenile"
                    enabled: !CompatibilityBridge.busy
                    onClicked: CompatibilityBridge.refresh()
                }

                Button {
                    text: "+ Yeni Profil"
                    enabled: CompatibilityBridge.available && !CompatibilityBridge.busy
                    onClicked: initProfileDialog.open()
                }
            }

            // Status / error bar
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: statusLabel.implicitHeight + 18
                radius: 6
                color: palette.alternateBase
                border.width: 1
                border.color: CompatibilityBridge.lastError !== "" ? "#c0392b" : palette.mid

                Label {
                    id: statusLabel
                    anchors.fill: parent
                    anchors.margins: 9
                    text: CompatibilityBridge.statusMessage
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                }
            }

            // Graceful unavailable notice
            Rectangle {
                visible: !CompatibilityBridge.available
                Layout.fillWidth: true
                implicitHeight: unavailableCol.implicitHeight + 24
                radius: 8
                color: palette.alternateBase
                border.width: 1
                border.color: palette.mid

                ColumnLayout {
                    id: unavailableCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    Label {
                        text: "zethropol-compatibility kurulu değil"
                        font.pixelSize: 18
                        font.bold: true
                    }

                    Label {
                        text: "Windows uyumluluk araçları (zethropol-prefix-ctl ve zethropol-wine-run) bu sistemde bulunamadı. Paketi kurduktan sonra Yenile düğmesiyle etkinleştirebilirsiniz."
                        textFormat: Text.PlainText
                        opacity: 0.75
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            // Hardware / Runtime Summary Cards
            GridLayout {
                visible: CompatibilityBridge.available
                columns: 2
                Layout.fillWidth: true
                rowSpacing: 10
                columnSpacing: 10

                Rectangle {
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 96
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Label { text: "Wine Çalışma Zamanı"; font.pixelSize: 16; font.bold: true }
                        Label {
                            text: CompatibilityBridge.wineVersion + " (" + CompatibilityBridge.wineMode + ")"
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: CompatibilityBridge.wineMode === "wow64-only"
                                  ? "Saf WoW64: classic-win32 otomatik win64-wow64 kipinde çalışır"
                                  : "Çoklu mimari desteği algılandı"
                            textFormat: Text.PlainText
                            opacity: 0.65
                            font.pixelSize: 12
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                }

                Rectangle {
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 96
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Label { text: "Vulkan GPU"; font.pixelSize: 16; font.bold: true }
                        Label {
                            text: CompatibilityBridge.vulkanGpuName
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: "Vulkan " + CompatibilityBridge.vulkanApiVersion + " · "
                                  + (CompatibilityBridge.hardwareVulkan13 ? "Donanım 1.3+ Uygun" : "Eski/Yazılım -> WineD3D")
                            textFormat: Text.PlainText
                            opacity: 0.65
                            font.pixelSize: 12
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                }

                Rectangle {
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 96
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Label { text: "DXVK / VKD3D Katmanları"; font.pixelSize: 16; font.bold: true }
                        Label {
                            text: "DXVK: " + (CompatibilityBridge.dxvkAvailable ? "Hazır" : "Kurulu değil (WineD3D yedek)")
                                  + " · VKD3D: " + (CompatibilityBridge.vkd3dAvailable ? "Hazır" : "Kurulu değil")
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: "İsteğe bağlı paketler: dxvk-mingw-git, vkd3d-proton-mingw-git"
                            textFormat: Text.PlainText
                            opacity: 0.65
                            font.pixelSize: 12
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                }

                Rectangle {
                    color: palette.alternateBase
                    Layout.fillWidth: true
                    implicitHeight: 96
                    radius: 8
                    border.width: 1
                    border.color: palette.mid

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        Label { text: "Yazma İzolasyonu (bwrap)"; font.pixelSize: 16; font.bold: true }
                        Label {
                            text: CompatibilityBridge.bwrapAvailable
                                  ? "Aktif (" + CompatibilityBridge.bwrapPath + ")"
                                  : "Kurulu değil (yalnızca Z:\\ kaldırma)"
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            text: "Varsayılan olarak / ve $HOME salt-okunur/gizli tutulur"
                            textFormat: Text.PlainText
                            opacity: 0.65
                            font.pixelSize: 12
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            // Profiles Section
            Label {
                visible: CompatibilityBridge.available
                text: "İzole Wine Profilleri (" + CompatibilityBridge.profileCount + ")"
                font.pixelSize: 20
                Layout.topMargin: 6
            }

            Rectangle {
                visible: CompatibilityBridge.available && CompatibilityBridge.profileCount === 0
                Layout.fillWidth: true
                implicitHeight: 68
                radius: 8
                color: palette.alternateBase
                border.width: 1
                border.color: palette.mid

                Label {
                    anchors.centerIn: parent
                    text: "Henüz profil oluşturulmadı. '+ Yeni Profil' ile default-win64 veya classic-win32 başlatabilirsiniz."
                    textFormat: Text.PlainText
                    opacity: 0.7
                }
            }

            Repeater {
                model: CompatibilityBridge.available ? CompatibilityBridge.profiles : []

                delegate: Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: profileCardCol.implicitHeight + 24
                    radius: 8
                    color: palette.alternateBase
                    border.width: 1
                    border.color: modelData.auditPassed ? palette.mid : "#c0392b"

                    ColumnLayout {
                        id: profileCardCol
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Label {
                                text: modelData.profile
                                textFormat: Text.PlainText
                                font.pixelSize: 17
                                font.bold: true
                                Layout.fillWidth: true
                            }

                            Label {
                                text: modelData.auditPassed ? "Denetim: PASS" : "Denetim: UYARI"
                                font.bold: true
                            }

                            Button {
                                text: "Denetle / Onar"
                                enabled: !CompatibilityBridge.busy
                                onClicked: CompatibilityBridge.auditProfile(modelData.profile, true)
                            }

                            Button {
                                text: "Kısayolları Eşitle"
                                enabled: !CompatibilityBridge.busy
                                onClicked: CompatibilityBridge.syncShortcuts(modelData.profile)
                            }

                            Button {
                                text: "Sil"
                                enabled: !CompatibilityBridge.busy
                                onClicked: {
                                    pendingRemoveProfile = modelData.profile
                                    removeProfileDialog.open()
                                }
                            }
                        }

                        Label {
                            text: "Şablon: " + modelData.template
                                  + " · Mimari: " + modelData.effectiveArch
                                  + " (" + modelData.windowsVersion + ")"
                                  + " · Motor: " + modelData.renderer
                                  + " · Yerel Ayar: " + modelData.locale
                            textFormat: Text.PlainText
                            opacity: 0.75
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label {
                            text: "Rozetler: "
                                  + (modelData.zDriveIsolated ? "[OK] Z:\\ İzole" : "[UYARI] Z:\\ Açık")
                                  + "  ·  "
                                  + (modelData.homeIsolated ? "[OK] $HOME İzole" : "[UYARI] $HOME Bağlı")
                                  + "  ·  "
                                  + (modelData.turkishLocaleOk ? "[OK] Türkçe (tr-TR)" : "[UYARI] Yerel Ayar")
                                  + "  ·  "
                                  + (modelData.fontsOk ? "[OK] Yazı Tipi Eşleme" : "[UYARI] Yazı Tipi")
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            opacity: 0.85
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // Launch .exe / .msi / .lnk panel
            Label {
                visible: CompatibilityBridge.available
                text: "Windows Uygulaması / Kurulum Çalıştır (.exe, .msi, .lnk)"
                font.pixelSize: 20
                Layout.topMargin: 8
            }

            Rectangle {
                visible: CompatibilityBridge.available
                Layout.fillWidth: true
                implicitHeight: launchCol.implicitHeight + 24
                radius: 8
                color: palette.alternateBase
                border.width: 1
                border.color: palette.mid

                ColumnLayout {
                    id: launchCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        TextField {
                            id: targetPathField
                            Layout.fillWidth: true
                            text: selectedTargetPath
                            placeholderText: "Çalıştırılacak .exe, .msi veya .lnk dosya yolu..."
                            onTextChanged: selectedTargetPath = text
                        }

                        Button {
                            text: "Dosya Seç..."
                            onClicked: targetFileDialog.open()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Label { text: "Profil:" }
                        ComboBox {
                            id: runProfileBox
                            Layout.preferredWidth: 200
                            model: profileModelList()
                            onCurrentTextChanged: {
                                if (currentText !== "")
                                    selectedRunProfile = currentText
                            }
                        }

                        Label { text: "Grafik Motoru:" }
                        ComboBox {
                            id: runRendererBox
                            Layout.preferredWidth: 160
                            model: ["auto", "dxvk", "wined3d"]
                            onCurrentTextChanged: selectedRenderer = currentText
                        }

                        Item { Layout.fillWidth: true }
                    }

                    CheckBox {
                        id: runSandboxCheck
                        checked: sandboxEnabled
                        text: "bubblewrap (bwrap) yazma izolasyonu aktif (önerilen)"
                        onCheckedChanged: sandboxEnabled = checked
                    }

                    Rectangle {
                        visible: !sandboxEnabled
                        Layout.fillWidth: true
                        implicitHeight: noSandboxWarn.implicitHeight + 14
                        radius: 6
                        color: palette.window
                        border.width: 1
                        border.color: "#c0392b"

                        Label {
                            id: noSandboxWarn
                            anchors.fill: parent
                            anchors.margins: 7
                            text: "Uyarı (--no-sandbox): İşletim sistemi düzeyinde bubblewrap yazma izolasyonu kapatıldı. Prefix içi Z:\\ kaldırması devam eder ancak uygulama kullanıcı izinlerinizle yerel yollara erişebilir."
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Button {
                            text: "Önizle (Dry-Run)"
                            enabled: !CompatibilityBridge.busy && selectedTargetPath.trim() !== ""
                            onClicked: CompatibilityBridge.previewLaunch(
                                selectedTargetPath,
                                selectedRunProfile,
                                selectedRenderer,
                                sandboxEnabled
                            )
                        }

                        Button {
                            text: "Çalıştır"
                            highlighted: true
                            enabled: !CompatibilityBridge.busy && selectedTargetPath.trim() !== ""
                            onClicked: CompatibilityBridge.launchTarget(
                                selectedTargetPath,
                                selectedRunProfile,
                                selectedRenderer,
                                sandboxEnabled
                            )
                        }
                    }

                    Rectangle {
                        visible: CompatibilityBridge.lastDryRunPlan && CompatibilityBridge.lastDryRunPlan.targetWindowsPath !== undefined && CompatibilityBridge.lastDryRunPlan.targetWindowsPath !== ""
                        Layout.fillWidth: true
                        implicitHeight: dryRunLabel.implicitHeight + 16
                        radius: 6
                        color: palette.window
                        border.width: 1
                        border.color: palette.mid

                        Label {
                            id: dryRunLabel
                            anchors.fill: parent
                            anchors.margins: 8
                            text: "Önizleme — Hedef: " + (CompatibilityBridge.lastDryRunPlan.targetWindowsPath || "")
                                  + " (" + (CompatibilityBridge.lastDryRunPlan.targetKind || "") + ")"
                                  + " · Etkin Motor: " + (CompatibilityBridge.lastDryRunPlan.effectiveRenderer || "")
                                  + " · Sandbox: " + (CompatibilityBridge.lastDryRunPlan.sandboxEnabled ? "bwrap" : "kapalı")
                                  + "\nGerekçe: " + (CompatibilityBridge.lastDryRunPlan.rendererReason || "")
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
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
