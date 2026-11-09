import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: page
    objectName: "hardwarePage"
    clip: true
    contentWidth: availableWidth
    property var state: HardwareBridge.state
    function number(value, digits, suffix) {
        return typeof value === "number" && isFinite(value) ? value.toFixed(digits) + suffix : "Unavailable"
    }
    function text(value) {
        return value !== undefined && value !== null && String(value).trim() !== "" ? String(value) : "Unavailable"
    }
    function cards() {
        const s = state || {}
        const health = s.health || {}
        const cpu = s.cpu || {}
        const ram = s.memory || {}
        const disk = s.normalized_storage || {}
        const board = s.motherboard || {}
        const net = s.network || {}
        const caps = s.capabilities || {}
        let result = [{title: "CPU", details: text(cpu.model)
            + "\nCores: " + (cpu.cores > 0 ? cpu.cores : "Unavailable") + " · Threads: " + (cpu.threads > 0 ? cpu.threads : "Unavailable")
            + "\nFrequency: " + number(cpu.frequency_ghz, 2, " GHz")
            + "\nTemperature: " + number(cpu.temperature_c, 1, " °C")
            + " · Usage: " + number(cpu.usage_percent, 1, "%")
            + "\nGovernor: " + text(cpu.governor) + "\nHealth: " + text(health.cpu_status)}]
        const gpus = s.gpus !== undefined ? s.gpus : (s.gpu && s.gpu.detected ? [s.gpu] : [])
        for (let i = 0; i < gpus.length; ++i) {
            const gpu = gpus[i]
            result.push({title: "GPU " + (i + 1) + " · " + text(gpu.card), details: text(gpu.model)
                + "\nPCI: " + text(gpu.vendor) + ":" + text(gpu.device)
                + " · Driver: " + text(gpu.driver)
                + "\nUsage: " + number(gpu.usage_percent, 1, "%")
                + " · Temperature: " + number(gpu.temperature_c, 1, " °C")
                + "\nVRAM: " + number(gpu.vram_used_gb, 2, " GiB") + " / " + number(gpu.vram_total_gb, 2, " GiB")
                + "\nCore: " + number(gpu.core_clock_mhz, 0, " MHz") + " · Memory: " + number(gpu.memory_clock_mhz, 0, " MHz")
                + "\n" + text(gpu.capability_status)})
        }
        if (gpus.length === 0)
            result.push({title: "GPU", details: "No accessible graphics adapter detected"})
        result.push({title: "Memory", details: "Total: " + number(ram.total_gb, 2, " GiB")
            + "\nUsed: " + number(ram.used_gb, 2, " GiB") + " · Available: " + number(ram.available_gb, 2, " GiB")
            + "\nHealth: " + text(health.memory_status)})
        result.push({title: "Storage · root device", details: text(disk.model) + "\nDevice: " + text(disk.device_path)
            + "\nPhysical capacity: " + number(disk.capacity_gb, 1, " GB")
            + "\nRoot filesystem used: " + number(disk.used_gb, 1, " GB") + " · Available: " + number(disk.available_gb, 1, " GB")
            + "\nFirmware: " + text(disk.firmware) + " · Temperature: " + number(disk.temperature_c, 1, " °C")
            + "\nHealth: " + text(health.storage_status)})
        result.push({title: "Motherboard / BIOS", details: text(board.vendor) + " · " + text(board.model)
            + "\nBoard revision: " + text(board.version)
            + "\nBIOS: " + text(board.bios_vendor) + " · " + text(board.bios_version)
            + "\nBIOS date: " + text(board.bios_date)})
        result.push({title: "Network", details: "Interface: " + text(net.interface)
            + "\nLink: " + text(net.operstate)
            + "\nDownload: " + number(net.download_mbps, 2, " Mbps") + " · Upload: " + number(net.upload_mbps, 2, " Mbps")
            + "\nPing: " + number(net.ping_ms, 1, " ms")})
        result.push({title: "Firmware", details: text(s.firmware ? s.firmware.status : undefined)})
        result.push({title: "Capabilities", details: "GPU acceleration: " + (typeof caps.gpu_acceleration === "boolean" ? (caps.gpu_acceleration ? "Available" : "Unavailable") : "Unverified")
            + "\nVRAM: " + number(caps.gpu_vram_gb, 2, " GiB")
            + "\nThread capacity: " + text(caps.thread_capacity)
            + "\nStorage power management: " + (typeof caps.storage_power_management === "boolean" ? (caps.storage_power_management ? "Available" : "Unavailable") : "Unverified")})
        result.push({title: "Hardware Changes", details: text(s.changes ? s.changes.message : undefined)})
        return result
    }
    ColumnLayout {
        width: page.availableWidth
        spacing: 14
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Hardware"; font.pixelSize: 32; Layout.fillWidth: true }
            Button {
                objectName: "hardwareRefresh"
                text: HardwareBridge.loading ? "Refreshing…" : "Refresh"
                enabled: !HardwareBridge.loading
                onClicked: HardwareBridge.refresh()
            }
        }
        Label {
            objectName: "hardwareStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: HardwareBridge.available ? "Detected hardware · Last refresh: " + HardwareBridge.lastUpdated
                : HardwareBridge.loading ? "Reading hardware information…" : "Hardware information unavailable: " + HardwareBridge.error
        }
        GridLayout {
            Layout.fillWidth: true
            columns: page.availableWidth >= 760 ? 2 : 1
            columnSpacing: 12
            rowSpacing: 12
            Repeater {
                model: page.cards()
                delegate: Rectangle {
                    required property var modelData
                    objectName: "hardwareCard"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: cardContents.implicitHeight + 28
                    color: page.palette.alternateBase
                    radius: 8
                    border.width: 1
                    border.color: page.palette.mid
                    ColumnLayout {
                        id: cardContents
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 14
                        spacing: 6
                        Label { text: modelData.title; font.pixelSize: 18; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                        Label { text: modelData.details; Layout.fillWidth: true; wrapMode: Text.WordWrap; opacity: 0.8 }
                    }
                }
            }
        }
    }
}
