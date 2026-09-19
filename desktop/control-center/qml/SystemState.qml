pragma Singleton
import QtQml

QtObject {
    property var hardware: HardwareBridge.state

    property string overallHealth: hardware.health ? hardware.health.overall_status : "Unknown"

    property string cpuStatus: hardware.health ? hardware.health.cpu_status : "Unknown"
    property string cpuMessage: hardware.cpu ? hardware.cpu.model : "System data unavailable"

    property string gpuStatus: hardware.health ? hardware.health.gpu_status : "Unknown"
    property string gpuMessage: hardware.gpu ? hardware.gpu.capability_status : "System data unavailable"

    property string memoryStatus: hardware.health ? hardware.health.memory_status : "Unknown"
    property string memoryMessage: hardware.memory ? "Used " + hardware.memory.used_gb.toFixed(1) + " / " + hardware.memory.total_gb.toFixed(1) + " GB" : "System data unavailable"

    property string storageStatus: hardware.health ? hardware.health.storage_status : "Unknown"
    property string storageMessage: hardware.normalized_storage ? hardware.normalized_storage.model : "System data unavailable"

    property string networkStatus: hardware.network ? "Connected" : "Unknown"
    property string networkMessage: hardware.network ? "Ping " + hardware.network.ping_ms.toFixed(1) + " ms" : "System data unavailable"

    property string systemStatus: hardware.health ? hardware.health.overall_status : "Unknown"
    property string systemMessage: hardware.health ? hardware.health.driver_status : "System data unavailable"

    property string hardwareSummary: hardware.cpu && hardware.gpu ? hardware.cpu.model + " · " + hardware.gpu.driver : "System data unavailable"
    property string hardwareMessage: hardware.normalized_storage ? hardware.normalized_storage.model + " · " + hardware.normalized_storage.capacity_gb.toFixed(0) + " GB" : "System data unavailable"

    property string performanceStatus: hardware.cpu ? hardware.cpu.usage_percent.toFixed(1) + "% CPU" : "Unknown"
    property string performanceMessage: hardware.memory ? "RAM " + ((hardware.memory.used_gb / hardware.memory.total_gb) * 100).toFixed(1) + "% · " + hardware.cpu.frequency_ghz.toFixed(2) + " GHz" : "System data unavailable"

    property string firmwareStatus: hardware.firmware ? hardware.firmware.status : "Unknown"
    property string changesStatus: hardware.changes ? hardware.changes.status : "Unknown"
}
