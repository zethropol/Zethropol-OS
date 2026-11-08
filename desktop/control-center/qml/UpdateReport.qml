import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: reportPage
    required property var scanner
    required property var installer
    readonly property color reportText: "#202124"

    Connections {
        target: installer
        function onChanged() {
            installationInput.clear()
            if (installer.busy)
                Qt.callLater(function() { installationInput.forceActiveFocus() })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 16

        RowLayout {
            Layout.fillWidth: true

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
                objectName: "startUpdateInstallation"
                text: installer.busy ? "Kurulum sürüyor…" : "Güncellemeleri kur…"
                enabled: !scanner.busy && !installer.busy && scanner.hasUpdates
                onClicked: installConfirmation.open()
            }
        }

        Label {
            color: reportPage.reportText
            text: "Resmî depolar · AUR · Flatpak"
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
                        color: "transparent"
                        border.width: 1
                        border.color: "#d9dce1"

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
                                        color: "#d9dce1"
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
            Layout.preferredHeight: installer.busy ? -1 : 220
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
                    color: "#f5f6f8"
                    border.color: "#d9dce1"
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
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhHiddenText | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
                maximumLength: 4096
                onAccepted: reportPage.sendInstallationInput(false)
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
        }

        Label {
            color: reportPage.reportText
            visible: installer.busy
            text: "Satır yanıtları için Gönder’i, tek tuş isteyen ekranlar için Tuş gönder’i kullanın. Devam etmek için alanı boş bırakabilirsiniz. Parola ve yanıtlar rapora kaydedilmez."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
        }

        Label {
            color: reportPage.reportText
            text: installer.busy ? "Kurulum tamamlanana kadar Control Center’ı açık tutun."
                                 : "Açılışta yalnızca tarama yapılır. Kurulum, onayınızla bu sekmede başlatılır."
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
            color: "white"
            border.color: "#d9dce1"
            radius: 8
        }
        header: Label {
            color: reportPage.reportText
            text: "Güncellemeleri kur"
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
                text: "Güncellemeler Control Center üzerinde yeniden kontrol edilerek uygulanacak. Resmî depoların tam sistem yükseltmesi, etkin AUR yardımcısı ve Flatpak güncellemeleri sırayla yürütülecek."
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                color: reportPage.reportText
                text: "Kurulum çıktısını bu sekmede izleyecek; paket seçimi, yönetici parolası ve bakım sorularını yanıt alanından cevaplayacaksınız. Ek temizlik işlemlerini kabul edebilir veya reddedebilirsiniz. İşlem bitince rapor otomatik yenilenir."
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
                enabled: !scanner.busy && !installer.busy && scanner.hasUpdates
                onClicked: {
                    installConfirmation.close()
                    installer.start()
                }
            }
        }
    }
}
