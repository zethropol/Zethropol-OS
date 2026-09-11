import QtQuick
import QtQuick.Controls
import org.kde.plasma.plasmoid
import org.kde.plasma.plasma5support as Plasma5Support
PlasmoidItem {
    id: root
    Plasmoid.title: "Zethropol System Monitor"
    implicitWidth: 948
    implicitHeight: 180

    // CPU frequency is provided by KSystemStats
    Plasma5Support.DataSource {
        id: hardwareSource
        engine: "executable"
        interval: 1000
        connectedSources: ["/home/Zevor/.local/share/zethropol/bin/zethropol-hardware --monitor"]
    }


    property var hardwareData: {
        var source = hardwareSource.data["/home/Zevor/.local/share/zethropol/bin/zethropol-hardware --monitor"]
        if (!source || source["stdout"] === undefined)
            return []
        return String(source["stdout"]).trim().split(/\s+/)
    }

    property real ramUsage: hardwareData.length > 11 && parseFloat(hardwareData[11]) > 0 ? Math.max(0, Math.min(100, parseFloat(hardwareData[10]) / parseFloat(hardwareData[11]) * 100)) : 0

    property real networkDownload: hardwareData.length > 16 ? parseFloat(hardwareData[16]) || 0 : 0
    property real networkUpload: hardwareData.length > 17 ? parseFloat(hardwareData[17]) || 0 : 0
    property real networkPing: hardwareData.length > 18 ? parseFloat(hardwareData[18]) || 0 : 0


    property real hardwareCpuFrequency: hardwareData.length > 2 ? parseFloat(hardwareData[2]) || 0 : 0

    property real gpuUsage: hardwareData.length > 3 ? parseFloat(hardwareData[3]) || 0 : 0
    property real gpuTemperature: hardwareData.length > 8 ? parseFloat(hardwareData[8]) || 0 : 0
    property real gpuVramUsed: hardwareData.length > 6 ? parseFloat(hardwareData[6]) || 0 : 0
    property real gpuVramTotal: hardwareData.length > 7 ? parseFloat(hardwareData[7]) || 0 : 0
    property real gpuFrequency: hardwareData.length > 5 ? parseFloat(hardwareData[5]) || 0 : 0

    property real cpuUsage: hardwareData.length > 0 ? Math.max(0, Math.min(100, parseFloat(hardwareData[0]) || 0)) : 0
    property real cpuTemperature: hardwareData.length > 1 ? parseFloat(hardwareData[1]) || 0 : 0
    property real storageUsed: hardwareData.length > 12 ? parseFloat(hardwareData[12]) || 0 : 0
    property real storageCapacity: hardwareData.length > 13 ? parseFloat(hardwareData[13]) || 0 : 0
    property real storageAvailable: hardwareData.length > 14 ? parseFloat(hardwareData[14]) || 0 : 0
    property real storageTemperature: hardwareData.length > 15 ? parseFloat(hardwareData[15]) || 0 : 0
    property real storageUsage: storageCapacity > 0 ? Math.max(0, Math.min(100, storageUsed / storageCapacity * 100)) : 0

    Row {
        anchors.centerIn: parent
        spacing: 12

        Rectangle {
            width: 180
            height: 180
            radius: 24
        color: "#171a1f"
        border.width: 1
        border.color: "#303740"

        Canvas {
            id: gauge
            anchors.fill: parent
            anchors.margins: 14

            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()

                var cx = width / 2
                var cy = height / 2
                var radius = Math.min(width, height) / 2 - 10
                var start = Math.PI * 0.75
                var sweep = Math.PI * 1.5

                ctx.lineWidth = 7
                ctx.lineCap = "round"
                ctx.strokeStyle = "#252b33"
                ctx.beginPath()
                ctx.arc(cx, cy, radius, start, start + sweep)
                ctx.stroke()

                ctx.lineWidth = 2
                ctx.strokeStyle = "#303740"
                for (var i = 0; i <= 20; i++) {
                    var a = start + sweep * (i / 20)
                    var r1 = radius - 6
                    var r2 = radius - 2
                    ctx.beginPath()
                    ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
                    ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2)
                    ctx.stroke()
                }

                ctx.lineWidth = 7
                ctx.strokeStyle = "#168cff"
                ctx.beginPath()
                ctx.arc(cx, cy, radius, start, start + sweep * (root.cpuUsage / 100))
                ctx.stroke()

                ctx.lineWidth = 2
                ctx.strokeStyle = "#665cff"
                ctx.beginPath()
                ctx.arc(cx, cy, radius + 6, start, start + sweep * (root.cpuUsage / 100))
                ctx.stroke()
            }

            Connections {
                target: root
                function onCpuUsageChanged() { gauge.requestPaint() }
            }
        }

        Column {
            anchors.centerIn: parent
            spacing: 1

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "CPU LOAD"
                color: "#8d98a5"
                font.pixelSize: 10
                font.letterSpacing: 1.5
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.cpuUsage.toFixed(1)
                color: "#e9f3ff"
                font.pixelSize: 34
                font.bold: true
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "%"
                color: "#168cff"
                font.pixelSize: 13
                font.bold: true
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.cpuTemperature.toFixed(0) + " °C"
                color: "#168cff"
                font.pixelSize: 12
            }

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: hardwareCpuFrequency > 0 ? hardwareCpuFrequency.toFixed(0) + " MHz" : "— MHz"
                color: "#168cff"
                font.pixelSize: 12
            }
        }

        Rectangle {
            width: parent.width * 0.42
            height: 2
            radius: 1
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            color: "#168cff"
        }
        }


        Rectangle {
            width: 180
            height: 180
            radius: 24
            color: "#171a1f"
            border.width: 1
            border.color: "#303740"

            Canvas {
                id: ramGauge
                anchors.fill: parent
                anchors.margins: 14

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()

                    var cx = width / 2
                    var cy = height / 2
                    var radius = Math.min(width, height) / 2 - 10
                    var start = Math.PI * 0.75
                    var sweep = Math.PI * 1.5

                    ctx.lineWidth = 7
                    ctx.lineCap = "round"
                    ctx.strokeStyle = "#252b33"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, start + sweep)
                    ctx.stroke()

                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#303740"
                    for (var i = 0; i <= 20; i++) {
                        var a = start + sweep * (i / 20)
                        var r1 = radius - 6
                        var r2 = radius - 2
                        ctx.beginPath()
                        ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
                        ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2)
                        ctx.stroke()
                    }

                    ctx.lineWidth = 7
                    ctx.strokeStyle = "#168cff"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, start + sweep * (root.ramUsage / 100))
                    ctx.stroke()

                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#665cff"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius + 6, start, start + sweep * (root.ramUsage / 100))
                    ctx.stroke()
                }

                Connections {
                    target: root
                    function onRamUsageChanged() { ramGauge.requestPaint() }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 1

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "RAM USAGE"
                    color: "#8d98a5"
                    font.pixelSize: 10
                    font.letterSpacing: 1.5
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.ramUsage.toFixed(1)
                    color: "#e9f3ff"
                    font.pixelSize: 34
                    font.bold: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: 13
                    font.bold: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: hardwareData.length > 11 ? parseFloat(hardwareData[11]).toFixed(1) + " GB RAM" : "— GB RAM"
                    color: "#168cff"
                    font.pixelSize: 12
                }
            }

            Rectangle {
                width: parent.width * 0.42
                height: 2
                radius: 1
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 14
                color: "#168cff"
            }
        }

        Rectangle {
            width: 180
            height: 180
            radius: 24
            color: "#171a1f"
            border.width: 1
            border.color: "#303740"

            Canvas {
                id: gpuGauge
                anchors.fill: parent
                anchors.margins: 14

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()

                    var cx = width / 2
                    var cy = height / 2
                    var radius = Math.min(width, height) / 2 - 10
                    var start = Math.PI * 0.75
                    var sweep = Math.PI * 1.5

                    ctx.lineWidth = 7
                    ctx.lineCap = "round"
                    ctx.strokeStyle = "#252b33"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, start + sweep)
                    ctx.stroke()

                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#303740"
                    for (var i = 0; i <= 20; i++) {
                        var a = start + sweep * (i / 20)
                        var r1 = radius - 6
                        var r2 = radius - 2
                        ctx.beginPath()
                        ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
                        ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2)
                        ctx.stroke()
                    }

                    ctx.lineWidth = 7
                    ctx.strokeStyle = "#168cff"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, start + sweep * (root.gpuUsage / 100))
                    ctx.stroke()

                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#665cff"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius + 6, start, start + sweep * (root.gpuUsage / 100))
                    ctx.stroke()
                }

                Connections {
                    target: root
                    function onGpuUsageChanged() { gpuGauge.requestPaint() }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 1

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "GPU LOAD"
                    color: "#8d98a5"
                    font.pixelSize: 10
                    font.letterSpacing: 1.5
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.gpuUsage.toFixed(1)
                    color: "#e9f3ff"
                    font.pixelSize: 34
                    font.bold: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: 13
                    font.bold: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.gpuTemperature.toFixed(0) + " °C  •  " + root.gpuVramUsed.toFixed(1) + "/" + root.gpuVramTotal.toFixed(1) + " GB"
                    color: "#168cff"
                    font.pixelSize: 10
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.gpuFrequency.toFixed(0) + " MHz"
                    color: "#168cff"
                    font.pixelSize: 11
                }
            }

        }

        Rectangle {
            width: 180
            height: 180
            radius: 24
            color: "#171a1f"
            border.color: "#303740"
            border.width: 1

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 20
                text: "NETWORK"
                    color: "#f0f3f6"
                    font.pixelSize: 12
                    font.bold: true
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 5

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "↓  DOWNLOAD"
                        color: "#168cff"
                        font.pixelSize: 9
                    }

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.networkDownload.toFixed(2) + " MB/s"
                        color: "#f0f3f6"
                        font.pixelSize: 20
                        font.bold: true
                    }

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "↑  UPLOAD"
                        color: "#168cff"
                        font.pixelSize: 9
                    }

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.networkUpload.toFixed(2) + " MB/s"
                        color: "#f0f3f6"
                        font.pixelSize: 20
                        font.bold: true
                    }

                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: root.networkPing.toFixed(1) + " ms  •  PING"
                        color: "#168cff"
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    width: parent.width * 0.42
                    height: 2
                    radius: 1
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 14
                    color: "#168cff"
                }
            }

        Rectangle {
            width: 180
            height: 180
            radius: 24
            color: "#171a1f"
            border.color: "#303740"
            border.width: 1

            Canvas {
                id: storageGauge
                anchors.fill: parent
                anchors.margins: 14

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var cx = width / 2
                    var cy = height / 2
                    var radius = Math.min(width, height) / 2 - 10
                    var start = Math.PI * 0.75
                    var sweep = Math.PI * 1.5

                    ctx.lineWidth = 7
                    ctx.lineCap = "round"
                    ctx.strokeStyle = "#252b33"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, start + sweep)
                    ctx.stroke()

                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#303740"
                    for (var i = 0; i <= 20; i++) {
                        var a = start + sweep * (i / 20)
                        var r1 = radius - 6
                        var r2 = radius - 2
                        ctx.beginPath()
                        ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
                        ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2)
                        ctx.stroke()
                    }

                    ctx.lineWidth = 7
                    ctx.strokeStyle = "#168cff"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius, start, start + sweep * (root.storageUsage / 100))
                    ctx.stroke()

                    ctx.lineWidth = 2
                    ctx.strokeStyle = "#665cff"
                    ctx.beginPath()
                    ctx.arc(cx, cy, radius + 6, start, start + sweep * (root.storageUsage / 100))
                    ctx.stroke()
                }

                Connections {
                    target: root
                    function onStorageUsageChanged() { storageGauge.requestPaint() }
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 1

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "STORAGE"
                    color: "#8d98a5"
                    font.pixelSize: 10
                    font.letterSpacing: 1.5
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.storageUsage.toFixed(1)
                    color: "#e9f3ff"
                    font.pixelSize: 34
                    font.bold: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: 13
                    font.bold: true
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.storageUsed.toFixed(1) + " / " + root.storageCapacity.toFixed(1) + " GB"
                    color: "#168cff"
                    font.pixelSize: 10
                }

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.storageTemperature.toFixed(0) + " °C"
                    color: "#168cff"
                    font.pixelSize: 12
                }
            }

            Rectangle {
                width: parent.width * 0.42
                height: 2
                radius: 1
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 14
                color: "#168cff"
            }
        }
    }
}
