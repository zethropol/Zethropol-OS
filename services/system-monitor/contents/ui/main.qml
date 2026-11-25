import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import org.zethropol.monitoring 1.0

PlasmoidItem {
    id: root
    objectName: "zethropolSystemMonitorRoot"
    Plasmoid.title: "Zethropol System Monitor"
    Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground

    readonly property real refWidth: 948
    readonly property real refHeight: 180
    readonly property real refCardSize: 180
    readonly property real refCardRadius: 24
    readonly property real refCardSpacing: 12
    readonly property real minReadableScale: 0.55

    implicitWidth: 948
    implicitHeight: 180
    Layout.preferredWidth: 948
    Layout.preferredHeight: 180
    Layout.minimumWidth: 522
    Layout.minimumHeight: 99
    clip: true

    readonly property real fitScale: Math.min(
        (root.width > 0 ? root.width : root.refWidth) / root.refWidth,
        (root.height > 0 ? root.height : root.refHeight) / root.refHeight)
    readonly property bool belowMinimumReadableSize: root.fitScale < root.minReadableScale
    readonly property real uiScale: Math.max(root.minReadableScale, root.fitScale)

    readonly property real cardSize: root.refCardSize * root.uiScale
    readonly property real cardRadius: root.refCardRadius * root.uiScale
    readonly property real cardSpacing: root.refCardSpacing * root.uiScale
    readonly property real gaugeMargin: 14 * root.uiScale
    readonly property real titleTopMargin: 38 * root.uiScale
    readonly property real accentWidth: root.cardSize * 0.42
    readonly property real accentHeight: Math.max(1, 2 * root.uiScale)
    readonly property real accentRadius: root.accentHeight / 2
    readonly property real accentBottomMargin: 14 * root.uiScale
    readonly property real cardBorderWidth: Math.max(1, Math.round(root.uiScale))

    readonly property int configuredBackgroundOpacity: {
        if (!Plasmoid.configuration)
            return 60
        var raw = Plasmoid.configuration.backgroundOpacity
        if (raw === undefined || raw === null || raw === "" || typeof raw === "boolean")
            return 60
        var num = Number(raw)
        return Number.isFinite(num) ? Math.round(Math.max(0, Math.min(100, num))) : 60
    }
    readonly property real cardOpacity: root.configuredBackgroundOpacity / 100
    readonly property color cardBackgroundColor: Qt.rgba(23 / 255, 26 / 255, 31 / 255, root.cardOpacity)

    MonitoringBridge {
        id: monitoringBridge
        objectName: "systemMonitorBridge"
    }

    readonly property bool telemetryConnected: monitoringBridge.connected
    readonly property bool telemetryAvailable: monitoringBridge.connected && monitoringBridge.stateAvailable
    readonly property string telemetryReadiness: monitoringBridge.readiness
    readonly property string telemetryErrorCode: monitoringBridge.errorCode
    readonly property var activeMetrics: (root.telemetryAvailable && monitoringBridge.metrics)
        ? monitoringBridge.metrics
        : ({})

    function validNumber(metricsMap, key) {
        if (!metricsMap || metricsMap[key] === undefined || metricsMap[key] === null)
            return null
        var raw = metricsMap[key]
        if (typeof raw !== "number" || !Number.isFinite(raw))
            return null
        return raw === 0 ? 0 : raw
    }

    function validPercent(metricsMap, key) {
        var value = root.validNumber(metricsMap, key)
        if (value === null || value < 0 || value > 100)
            return null
        return value
    }

    function validNonNegative(metricsMap, key) {
        var value = root.validNumber(metricsMap, key)
        if (value === null || value < 0)
            return null
        return value
    }

    function validTemperature(metricsMap, key) {
        var value = root.validNumber(metricsMap, key)
        if (value === null || value < -100 || value > 250)
            return null
        return value
    }

    function scaledPx(basePx, minPx) {
        return Math.max(minPx, Math.round(basePx * root.uiScale))
    }

    function drawGaugeArc(ctx, canvasWidth, canvasHeight, scaleFactor, hasValue, valuePercent) {
        if (!ctx || canvasWidth <= 0 || canvasHeight <= 0)
            return
        ctx.reset()

        var s = Math.max(0.5, scaleFactor)
        var cx = canvasWidth / 2
        var cy = canvasHeight / 2
        var radius = Math.max(4, Math.min(canvasWidth, canvasHeight) / 2 - 10 * s)
        var start = Math.PI * 0.75
        var sweep = Math.PI * 1.5

        ctx.lineWidth = Math.max(1, 7 * s)
        ctx.lineCap = "round"
        ctx.strokeStyle = "#252b33"
        ctx.beginPath()
        ctx.arc(cx, cy, radius, start, start + sweep)
        ctx.stroke()

        ctx.lineWidth = Math.max(1, 2 * s)
        ctx.strokeStyle = "#303740"
        for (var i = 0; i <= 20; i++) {
            var a = start + sweep * (i / 20)
            var r1 = radius - 6 * s
            var r2 = radius - 2 * s
            ctx.beginPath()
            ctx.moveTo(cx + Math.cos(a) * r1, cy + Math.sin(a) * r1)
            ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2)
            ctx.stroke()
        }

        if (hasValue && typeof valuePercent === "number" && Number.isFinite(valuePercent)) {
            var clamped = Math.max(0, Math.min(100, valuePercent))
            ctx.lineWidth = Math.max(1, 7 * s)
            ctx.strokeStyle = "#168cff"
            ctx.beginPath()
            ctx.arc(cx, cy, radius, start, start + sweep * (clamped / 100))
            ctx.stroke()

            ctx.lineWidth = Math.max(1, 2 * s)
            ctx.strokeStyle = "#665cff"
            ctx.beginPath()
            ctx.arc(cx, cy, radius + 6 * s, start, start + sweep * (clamped / 100))
            ctx.stroke()
        }
    }

    // Pod 1 — CPU LOAD
    readonly property var cpuUsage: root.validPercent(root.activeMetrics, "cpu_usage_percent")
    readonly property bool hasCpuUsage: root.cpuUsage !== null
    readonly property var cpuTemperature: root.validTemperature(root.activeMetrics, "cpu_temperature_c")
    readonly property var cpuFrequencyGHz: root.validNonNegative(root.activeMetrics, "cpu_frequency_ghz")
    readonly property var cpuFrequencyMHz: root.cpuFrequencyGHz !== null ? Number(root.cpuFrequencyGHz) * 1000 : null

    // Pod 2 — RAM USAGE (values in GiB under legacy `_gb` keys)
    readonly property var ramUsedGiB: root.validNonNegative(root.activeMetrics, "memory_used_gb")
    readonly property var ramTotalGiB: root.validNonNegative(root.activeMetrics, "memory_total_gb")
    readonly property bool ramCapacityValid: root.ramTotalGiB !== null
        && Number(root.ramTotalGiB) > 0
        && (root.ramUsedGiB === null || Number(root.ramUsedGiB) <= Number(root.ramTotalGiB))
    readonly property bool ramPairValid: root.ramUsedGiB !== null
        && root.ramTotalGiB !== null
        && Number(root.ramTotalGiB) > 0
        && Number(root.ramUsedGiB) <= Number(root.ramTotalGiB)
    readonly property var ramUsage: root.ramPairValid
        ? Math.max(0, Math.min(100, (Number(root.ramUsedGiB) / Number(root.ramTotalGiB)) * 100))
        : null
    readonly property bool hasRamUsage: root.ramUsage !== null

    // Pod 3 — GPU LOAD (VRAM in GiB)
    readonly property var gpuUsage: root.validPercent(root.activeMetrics, "gpu_usage_percent")
    readonly property bool hasGpuUsage: root.gpuUsage !== null
    readonly property var gpuTemperature: root.validTemperature(root.activeMetrics, "gpu_temperature_c")
    readonly property var gpuCoreClockMHz: root.validNonNegative(root.activeMetrics, "gpu_core_clock_mhz")
    readonly property var gpuVramUsedGiB: root.validNonNegative(root.activeMetrics, "gpu_vram_used_gb")
    readonly property var gpuVramTotalGiB: root.validNonNegative(root.activeMetrics, "gpu_vram_total_gb")
    readonly property bool gpuVramValid: root.gpuVramUsedGiB !== null
        && root.gpuVramTotalGiB !== null
        && Number(root.gpuVramTotalGiB) > 0
        && Number(root.gpuVramUsedGiB) <= Number(root.gpuVramTotalGiB)

    // Pod 4 — NETWORK (decimal Mbps, optional ping ms)
    readonly property var networkDownloadMbps: root.validNonNegative(root.activeMetrics, "network_download_mbps")
    readonly property var networkUploadMbps: root.validNonNegative(root.activeMetrics, "network_upload_mbps")
    readonly property var networkPingMs: root.validNonNegative(root.activeMetrics, "network_ping_ms")

    // Pod 5 — STORAGE (direct filesystem usage % and decimal GB; never falls back to physical capacity)
    readonly property var storageUsage: root.validPercent(root.activeMetrics, "storage_filesystem_usage_percent")
    readonly property bool hasStorageUsage: root.storageUsage !== null
    readonly property var storageFilesystemUsedGB: root.validNonNegative(root.activeMetrics, "storage_filesystem_used_gb")
    readonly property var storageFilesystemTotalGB: root.validNonNegative(root.activeMetrics, "storage_filesystem_total_gb")
    readonly property bool storageFilesystemCapacityValid: root.storageFilesystemUsedGB !== null
        && root.storageFilesystemTotalGB !== null
        && Number(root.storageFilesystemTotalGB) > 0
        && Number(root.storageFilesystemUsedGB) <= Number(root.storageFilesystemTotalGB)
    readonly property var storageTemperature: root.validTemperature(root.activeMetrics, "storage_temperature_c")

    Row {
        objectName: "podsRow"
        anchors.centerIn: parent
        spacing: root.cardSpacing

        // Pod 1: CPU LOAD
        Rectangle {
            objectName: "cpuPod"
            width: root.cardSize
            height: root.cardSize
            radius: root.cardRadius
            color: root.cardBackgroundColor
            border.width: root.cardBorderWidth
            border.color: "#303740"

            Canvas {
                id: gauge
                objectName: "cpuGauge"
                anchors.fill: parent
                anchors.margins: root.gaugeMargin

                property int paintCount: 0
                property bool lastPaintedActive: false
                property real lastPaintedValue: -1

                onPaint: {
                    var active = root.hasCpuUsage
                    var val = active ? Number(root.cpuUsage) : -1
                    root.drawGaugeArc(getContext("2d"), width, height, root.uiScale, active, val)
                    lastPaintedActive = active
                    lastPaintedValue = active ? Math.max(0, Math.min(100, val)) : -1
                    paintCount += 1
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                Connections {
                    target: root
                    function onCpuUsageChanged() { gauge.requestPaint() }
                    function onHasCpuUsageChanged() { gauge.requestPaint() }
                }
            }

            Label {
                id: cpuTitle
                objectName: "cpuTitleLabel"
                anchors.top: parent.top
                anchors.topMargin: root.titleTopMargin
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.56
                horizontalAlignment: Text.AlignHCenter
                text: "CPU LOAD"
                color: "#8d98a5"
                font.pixelSize: root.scaledPx(10, 6)
                font.letterSpacing: 1.5 * root.uiScale
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 6
            }

            Column {
                anchors.top: cpuTitle.bottom
                anchors.topMargin: 1 * root.uiScale
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.68
                spacing: 1 * root.uiScale

                Label {
                    objectName: "cpuUsageLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.hasCpuUsage ? Number(root.cpuUsage).toFixed(1) : "—"
                    color: "#e9f3ff"
                    font.pixelSize: root.scaledPx(34, 16)
                    font.bold: true
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: root.scaledPx(18, 11)
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(13, 8)
                    font.bold: true
                }

                Label {
                    objectName: "cpuTempLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.cpuTemperature !== null ? Number(root.cpuTemperature).toFixed(0) + " °C" : "— °C"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(12, 7)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }

                Label {
                    objectName: "cpuFreqLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.cpuFrequencyMHz !== null ? Number(root.cpuFrequencyMHz).toFixed(0) + " MHz" : "— MHz"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(12, 7)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                objectName: "cpuAccentBar"
                width: root.accentWidth
                height: root.accentHeight
                radius: root.accentRadius
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: root.accentBottomMargin
                color: "#168cff"
            }
        }

        // Pod 2: RAM USAGE
        Rectangle {
            objectName: "ramPod"
            width: root.cardSize
            height: root.cardSize
            radius: root.cardRadius
            color: root.cardBackgroundColor
            border.width: root.cardBorderWidth
            border.color: "#303740"

            Canvas {
                id: ramGauge
                objectName: "ramGauge"
                anchors.fill: parent
                anchors.margins: root.gaugeMargin

                property int paintCount: 0
                property bool lastPaintedActive: false
                property real lastPaintedValue: -1

                onPaint: {
                    var active = root.hasRamUsage
                    var val = active ? Number(root.ramUsage) : -1
                    root.drawGaugeArc(getContext("2d"), width, height, root.uiScale, active, val)
                    lastPaintedActive = active
                    lastPaintedValue = active ? Math.max(0, Math.min(100, val)) : -1
                    paintCount += 1
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                Connections {
                    target: root
                    function onRamUsageChanged() { ramGauge.requestPaint() }
                    function onHasRamUsageChanged() { ramGauge.requestPaint() }
                }
            }

            Label {
                id: ramTitle
                objectName: "ramTitleLabel"
                anchors.top: parent.top
                anchors.topMargin: root.titleTopMargin
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.56
                horizontalAlignment: Text.AlignHCenter
                text: "RAM USAGE"
                color: "#8d98a5"
                font.pixelSize: root.scaledPx(10, 6)
                font.letterSpacing: 1.5 * root.uiScale
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 6
            }

            Column {
                anchors.top: ramTitle.bottom
                anchors.topMargin: 1 * root.uiScale
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.68
                spacing: 1 * root.uiScale

                Label {
                    objectName: "ramUsageLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.hasRamUsage ? Number(root.ramUsage).toFixed(1) : "—"
                    color: "#e9f3ff"
                    font.pixelSize: root.scaledPx(34, 16)
                    font.bold: true
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: root.scaledPx(18, 11)
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(13, 8)
                    font.bold: true
                }

                Label {
                    objectName: "ramTotalLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.ramCapacityValid ? Number(root.ramTotalGiB).toFixed(1) + " GiB RAM" : "— GiB RAM"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(12, 7)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                objectName: "ramAccentBar"
                width: root.accentWidth
                height: root.accentHeight
                radius: root.accentRadius
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: root.accentBottomMargin
                color: "#168cff"
            }
        }

        // Pod 3: GPU LOAD
        Rectangle {
            objectName: "gpuPod"
            width: root.cardSize
            height: root.cardSize
            radius: root.cardRadius
            color: root.cardBackgroundColor
            border.width: root.cardBorderWidth
            border.color: "#303740"

            Canvas {
                id: gpuGauge
                objectName: "gpuGauge"
                anchors.fill: parent
                anchors.margins: root.gaugeMargin

                property int paintCount: 0
                property bool lastPaintedActive: false
                property real lastPaintedValue: -1

                onPaint: {
                    var active = root.hasGpuUsage
                    var val = active ? Number(root.gpuUsage) : -1
                    root.drawGaugeArc(getContext("2d"), width, height, root.uiScale, active, val)
                    lastPaintedActive = active
                    lastPaintedValue = active ? Math.max(0, Math.min(100, val)) : -1
                    paintCount += 1
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                Connections {
                    target: root
                    function onGpuUsageChanged() { gpuGauge.requestPaint() }
                    function onHasGpuUsageChanged() { gpuGauge.requestPaint() }
                }
            }

            Label {
                id: gpuTitle
                objectName: "gpuTitleLabel"
                anchors.top: parent.top
                anchors.topMargin: root.titleTopMargin
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.56
                horizontalAlignment: Text.AlignHCenter
                text: "GPU LOAD"
                color: "#8d98a5"
                font.pixelSize: root.scaledPx(10, 6)
                font.letterSpacing: 1.5 * root.uiScale
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 6
            }

            Column {
                anchors.top: gpuTitle.bottom
                anchors.topMargin: 1 * root.uiScale
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.62
                spacing: 1 * root.uiScale

                Label {
                    objectName: "gpuUsageLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.hasGpuUsage ? Number(root.gpuUsage).toFixed(1) : "—"
                    color: "#e9f3ff"
                    font.pixelSize: root.scaledPx(34, 16)
                    font.bold: true
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: root.scaledPx(18, 11)
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(13, 8)
                    font.bold: true
                }

                Label {
                    objectName: "gpuMetaLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: (root.gpuTemperature !== null ? Number(root.gpuTemperature).toFixed(0) + " °C" : "— °C")
                          + "  •  "
                          + (root.gpuVramValid
                             ? Number(root.gpuVramUsedGiB).toFixed(1) + "/" + Number(root.gpuVramTotalGiB).toFixed(1) + " GiB"
                             : "—/— GiB")
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(10, 6)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }

                Label {
                    objectName: "gpuClockLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.gpuCoreClockMHz !== null ? Number(root.gpuCoreClockMHz).toFixed(0) + " MHz" : "— MHz"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(11, 6)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                objectName: "gpuAccentBar"
                width: root.accentWidth
                height: root.accentHeight
                radius: root.accentRadius
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: root.accentBottomMargin
                color: "#168cff"
            }
        }

        // Pod 4: NETWORK
        Rectangle {
            objectName: "networkPod"
            width: root.cardSize
            height: root.cardSize
            radius: root.cardRadius
            color: root.cardBackgroundColor
            border.color: "#303740"
            border.width: root.cardBorderWidth

            Label {
                id: networkTitle
                objectName: "networkTitleLabel"
                anchors.top: parent.top
                anchors.topMargin: root.titleTopMargin
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.72
                horizontalAlignment: Text.AlignHCenter
                text: "NETWORK"
                color: "#8d98a5"
                font.pixelSize: root.scaledPx(10, 6)
                font.letterSpacing: 1.5 * root.uiScale
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 6
            }

            Item {
                anchors.top: networkTitle.bottom
                anchors.topMargin: 4 * root.uiScale
                anchors.bottom: networkAccent.top
                anchors.bottomMargin: 4 * root.uiScale
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 12 * root.uiScale
                anchors.rightMargin: 12 * root.uiScale

                Column {
                    anchors.centerIn: parent
                    width: parent.width
                    spacing: 2 * root.uiScale

                    Label {
                        objectName: "networkDownloadTitleLabel"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "↓  DOWNLOAD"
                        color: "#168cff"
                        font.pixelSize: root.scaledPx(9, 6)
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 6
                    }

                    Label {
                        objectName: "networkDownloadLabel"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: root.networkDownloadMbps !== null
                            ? Number(root.networkDownloadMbps).toFixed(2) + " Mbps"
                            : "— Mbps"
                        color: "#f0f3f6"
                        font.pixelSize: root.scaledPx(18, 10)
                        font.bold: true
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: root.scaledPx(10, 7)
                        elide: Text.ElideRight
                    }

                    Label {
                        objectName: "networkUploadTitleLabel"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "↑  UPLOAD"
                        color: "#168cff"
                        font.pixelSize: root.scaledPx(9, 6)
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 6
                    }

                    Label {
                        objectName: "networkUploadLabel"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: root.networkUploadMbps !== null
                            ? Number(root.networkUploadMbps).toFixed(2) + " Mbps"
                            : "— Mbps"
                        color: "#f0f3f6"
                        font.pixelSize: root.scaledPx(18, 10)
                        font.bold: true
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: root.scaledPx(10, 7)
                        elide: Text.ElideRight
                    }

                    Label {
                        objectName: "networkPingLabel"
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: root.networkPingMs !== null
                            ? Number(root.networkPingMs).toFixed(1) + " ms  •  PING"
                            : "— ms  •  PING"
                        color: "#168cff"
                        font.pixelSize: root.scaledPx(10, 6)
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: 6
                        elide: Text.ElideRight
                    }
                }
            }

            Rectangle {
                id: networkAccent
                objectName: "networkAccentBar"
                width: root.accentWidth
                height: root.accentHeight
                radius: root.accentRadius
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: root.accentBottomMargin
                color: "#168cff"
            }
        }

        // Pod 5: STORAGE
        Rectangle {
            objectName: "storagePod"
            width: root.cardSize
            height: root.cardSize
            radius: root.cardRadius
            color: root.cardBackgroundColor
            border.color: "#303740"
            border.width: root.cardBorderWidth

            Canvas {
                id: storageGauge
                objectName: "storageGauge"
                anchors.fill: parent
                anchors.margins: root.gaugeMargin

                property int paintCount: 0
                property bool lastPaintedActive: false
                property real lastPaintedValue: -1

                onPaint: {
                    var active = root.hasStorageUsage
                    var val = active ? Number(root.storageUsage) : -1
                    root.drawGaugeArc(getContext("2d"), width, height, root.uiScale, active, val)
                    lastPaintedActive = active
                    lastPaintedValue = active ? Math.max(0, Math.min(100, val)) : -1
                    paintCount += 1
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                Connections {
                    target: root
                    function onStorageUsageChanged() { storageGauge.requestPaint() }
                    function onHasStorageUsageChanged() { storageGauge.requestPaint() }
                }
            }

            Label {
                id: storageTitle
                objectName: "storageTitleLabel"
                anchors.top: parent.top
                anchors.topMargin: root.titleTopMargin
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.56
                horizontalAlignment: Text.AlignHCenter
                text: "STORAGE"
                color: "#8d98a5"
                font.pixelSize: root.scaledPx(10, 6)
                font.letterSpacing: 1.5 * root.uiScale
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 6
            }

            Column {
                anchors.top: storageTitle.bottom
                anchors.topMargin: 1 * root.uiScale
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.cardSize * 0.62
                spacing: 1 * root.uiScale

                Label {
                    objectName: "storageUsageLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.hasStorageUsage ? Number(root.storageUsage).toFixed(1) : "—"
                    color: "#e9f3ff"
                    font.pixelSize: root.scaledPx(34, 16)
                    font.bold: true
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: root.scaledPx(18, 11)
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "%"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(13, 8)
                    font.bold: true
                }

                Label {
                    objectName: "storageCapacityLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.storageFilesystemCapacityValid
                        ? Number(root.storageFilesystemUsedGB).toFixed(1) + " / " + Number(root.storageFilesystemTotalGB).toFixed(1) + " GB"
                        : "— / — GB"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(10, 6)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }

                Label {
                    objectName: "storageTempLabel"
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.storageTemperature !== null
                        ? Number(root.storageTemperature).toFixed(0) + " °C"
                        : "— °C"
                    color: "#168cff"
                    font.pixelSize: root.scaledPx(12, 7)
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 6
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                objectName: "storageAccentBar"
                width: root.accentWidth
                height: root.accentHeight
                radius: root.accentRadius
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: root.accentBottomMargin
                color: "#168cff"
            }
        }
    }
}
