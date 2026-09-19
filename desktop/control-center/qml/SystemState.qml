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
}
