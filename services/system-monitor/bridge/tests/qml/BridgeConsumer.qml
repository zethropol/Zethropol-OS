import QtQuick
import org.zethropol.monitoring 1.0

Item {
    id: root
    objectName: "bridgeConsumer"

    MonitoringBridge {
        id: bridge
        objectName: "monitoringBridgePrimary"
    }

    // Second QML instance verifies process-local shared backend inside the QML plugin
    // (must NOT create duplicate D-Bus subscriptions or extra GetState requests).
    MonitoringBridge {
        id: bridgeSecondary
        objectName: "monitoringBridgeSecondary"
    }

    readonly property bool connected: bridge.connected
    readonly property bool stateAvailable: bridge.stateAvailable
    readonly property string readiness: bridge.readiness
    readonly property string errorCode: bridge.errorCode
    readonly property string errorMessage: bridge.errorMessage
    readonly property var metrics: bridge.metrics
    readonly property date lastUpdate: bridge.lastUpdate
    readonly property double lastUpdateMs: bridge.lastUpdateMs

    readonly property bool secondaryStateAvailable: bridgeSecondary.stateAvailable
    readonly property string secondaryReadiness: bridgeSecondary.readiness

    // Reactive QML bindings backed by `bridge.metrics` notify signal
    readonly property bool hasCpuUsage: bridge.metrics !== null && bridge.hasMetric("cpu_usage_percent")
    readonly property var cpuUsageValue: bridge.metrics["cpu_usage_percent"]
    readonly property bool hasGpuUsage: bridge.metrics !== null && bridge.hasMetric("gpu_usage_percent")
    readonly property var gpuUsageValue: bridge.metrics["gpu_usage_percent"]
    readonly property bool hasPing: bridge.metrics !== null && bridge.hasMetric("network_ping_ms")
    readonly property bool hasFsUsagePercent: bridge.metrics !== null && bridge.hasMetric("storage_filesystem_usage_percent")
    readonly property var fsUsagePercentValue: bridge.metrics["storage_filesystem_usage_percent"]

    function queryHasMetric(key) {
        return bridge.hasMetric(key)
    }

    function queryMetricValue(key) {
        return bridge.metricValue(key)
    }

    property int stateChangeCount: 0
    property int metricsChangeCount: 0

    Connections {
        target: bridge
        function onStateChanged() { root.stateChangeCount += 1 }
        function onMetricsChanged() { root.metricsChangeCount += 1 }
    }
}
