import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Dialog {
    id: dlg
    title: "Yayın Kaynağı ve EPG Ekle"
    modal: true
    width: Math.min(parent ? parent.width - 48 : 560, 580)
    standardButtons: Dialog.Close

    background: Rectangle {
        radius: 14
        color: "#0f1623"
        border.color: "#2b3b54"
        border.width: 1
    }

    header: Rectangle {
        implicitHeight: 50
        color: "#131d2e"
        radius: 14

        Label {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 18
            text: dlg.title
            color: "#f0f6fc"
            font.pixelSize: 16
            font.bold: true
        }
    }

    FileDialog {
        id: m3uFileDialog
        title: "M3U / M3U8 Kanal Listesi Seç"
        nameFilters: ["M3U Listeleri (*.m3u *.m3u8)", "Tüm Dosyalar (*)"]
        onAccepted: {
            ChannelBridge.importM3uFromFile(selectedFile, replaceCheck.checked)
        }
    }

    FileDialog {
        id: xmltvFileDialog
        title: "XMLTV EPG Dosyası Seç"
        nameFilters: ["XMLTV Dosyaları (*.xml *.xmltv *.xml.gz *.gz)", "Tüm Dosyalar (*)"]
        onAccepted: {
            EpgBridge.importXmltvFromFile(selectedFile)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        // Security policy badge
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: secLbl.implicitHeight + 16
            radius: 9
            color: "#142238"
            border.color: "#24436e"
            border.width: 1

            Label {
                id: secLbl
                anchors.fill: parent
                anchors.margins: 8
                wrapMode: Text.WordWrap
                color: "#93c5fd"
                font.pixelSize: 11
                text: "Güvenlik politikası: Yalnızca http:// ve https:// akış adreslerine izin verilir. file://, exec://, lua:// gibi şemalar ve mpv betikleri otomatik olarak engellenir (M3U maks. 4 MiB, XMLTV maks. 16 MiB, zaman aşımı 15 sn)."
            }
        }

        // Section 1: M3U Playlist Import
        Label {
            text: "1. M3U / M3U8 Kanal Listesi"
            color: "#e9f3ff"
            font.pixelSize: 13
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: m3uUrlField
                objectName: "m3uUrlInput"
                Layout.fillWidth: true
                placeholderText: "https://ornek.com/kanallar.m3u8"
                placeholderTextColor: "#64748b"
                color: "#f0f6fc"
                background: Rectangle {
                    radius: 8
                    color: "#0b111c"
                    border.color: m3uUrlField.activeFocus ? "#168cff" : "#27354a"
                    border.width: 1
                }
            }

            Button {
                text: ChannelBridge.loading ? "İndiriliyor..." : "URL Ekle"
                enabled: !ChannelBridge.loading && m3uUrlField.text.trim().length > 0
                onClicked: ChannelBridge.importM3uFromUrl(m3uUrlField.text, replaceCheck.checked)
            }

            Button {
                text: "Dosya Seç"
                onClicked: m3uFileDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            CheckBox {
                id: replaceCheck
                text: "Mevcut listeyi değiştir"
                contentItem: Label {
                    text: replaceCheck.text
                    color: "#cbd5e1"
                    font.pixelSize: 12
                    leftPadding: replaceCheck.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Varsayılana Dön"
                onClicked: ChannelBridge.resetToDefaultChannels()
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: ChannelBridge.statusMessage.length > 0
            text: ChannelBridge.statusMessage
            color: "#94a3b8"
            font.pixelSize: 11
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#223047"
        }

        // Section 2: Single Channel or Official Live Broadcast Page (StreamResolver)
        Label {
            text: "2. Tekil Kanal veya Resmi Canlı Yayın Sayfası"
            color: "#e9f3ff"
            font.pixelSize: 13
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: singleNameField
                objectName: "singleChannelNameInput"
                Layout.preferredWidth: 165
                placeholderText: "Kanal adı (opsiyonel)"
                placeholderTextColor: "#64748b"
                color: "#f0f6fc"
                background: Rectangle {
                    radius: 8
                    color: "#0b111c"
                    border.color: singleNameField.activeFocus ? "#168cff" : "#27354a"
                    border.width: 1
                }
            }

            TextField {
                id: singleGroupField
                objectName: "singleChannelGroupInput"
                Layout.preferredWidth: 115
                placeholderText: "Kategori"
                placeholderTextColor: "#64748b"
                color: "#f0f6fc"
                background: Rectangle {
                    radius: 8
                    color: "#0b111c"
                    border.color: singleGroupField.activeFocus ? "#168cff" : "#27354a"
                    border.width: 1
                }
            }

            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: singleUrlField
                objectName: "singleChannelUrlInput"
                Layout.fillWidth: true
                placeholderText: "https://resmi-yayinci.com/canli veya .m3u8 / resolve://https://..."
                placeholderTextColor: "#64748b"
                color: "#f0f6fc"
                background: Rectangle {
                    radius: 8
                    color: "#0b111c"
                    border.color: singleUrlField.activeFocus ? "#168cff" : "#27354a"
                    border.width: 1
                }
            }

            Button {
                text: "Kanal Ekle"
                enabled: singleUrlField.text.trim().length > 0
                onClicked: {
                    if (ChannelBridge.addSingleChannel(singleNameField.text, singleUrlField.text, singleGroupField.text)) {
                        singleNameField.text = ""
                        singleGroupField.text = ""
                        singleUrlField.text = ""
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#223047"
        }

        // Section 3: XMLTV EPG Import (.xml and .xml.gz)
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "3. XMLTV Yayın Akışı (EPG — .xml / .xml.gz)"
                color: "#e9f3ff"
                font.pixelSize: 13
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Açık TR Önerisi"
                onClicked: epgUrlField.text = EpgBridge.recommendedXmltvUrl
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: epgUrlField
                objectName: "epgUrlInput"
                Layout.fillWidth: true
                placeholderText: "https://ornek.com/epg.xml veya epg.xml.gz"
                placeholderTextColor: "#64748b"
                color: "#f0f6fc"
                background: Rectangle {
                    radius: 8
                    color: "#0b111c"
                    border.color: epgUrlField.activeFocus ? "#168cff" : "#27354a"
                    border.width: 1
                }
            }

            Button {
                text: EpgBridge.loading ? "İndiriliyor..." : "EPG İndir"
                enabled: !EpgBridge.loading && epgUrlField.text.trim().length > 0
                onClicked: EpgBridge.importXmltvFromUrl(epgUrlField.text)
            }

            Button {
                text: "XML Seç"
                onClicked: xmltvFileDialog.open()
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: EpgBridge.statusMessage.length > 0
            text: EpgBridge.statusMessage
            color: "#94a3b8"
            font.pixelSize: 11
        }
    }
}
