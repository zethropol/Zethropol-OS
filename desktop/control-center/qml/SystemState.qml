pragma Singleton
import QtQml

QtObject {
    property var hardware: HardwareBridge.state

    property string overallHealth: hardware.health ? hardware.health.overall_status : "Unknown"

    property string cpuStatus: hardware.health ? hardware.health.cpu_status : "Unknown"
    property string cpuMessage: hardware.cpu ? hardware.cpu.model : "System data unavailable"

    property string gpuStatus: hardware.health ? hardware.health.gpu_status : "Unknown"
    property string gpuMessage: hardware.gpu ? hardware.gpu.capability_status : "System data unavailable"
    property string gpuModel: hardware.gpu ? hardware.gpu.model : "Unknown"
    property string gpuFamily: hardware.gpu ? hardware.gpu.family : "Unknown"

    property string memoryStatus: hardware.health ? hardware.health.memory_status : "Unknown"
    property string memoryMessage: hardware.memory ? "Used " + hardware.memory.used_gb.toFixed(1) + " / " + hardware.memory.total_gb.toFixed(1) + " GB" : "System data unavailable"

    property string storageStatus: hardware.health ? hardware.health.storage_status : "Unknown"
    property string storageMessage: hardware.normalized_storage ? hardware.normalized_storage.model : "System data unavailable"

    property string networkStatus: hardware.network ? "Connected" : "Unknown"
    property string networkMessage: hardware.network ? "Ping " + hardware.network.ping_ms.toFixed(1) + " ms" : "System data unavailable"

    property string systemStatus: hardware.health ? hardware.health.overall_status : "Unknown"
    property string systemMessage: hardware.health ? hardware.health.driver_status : "System data unavailable"

    property string hardwareSummary: hardware.cpu && hardware.gpu ? hardware.cpu.model + "
" + hardware.gpu.model : "System data unavailable"
    property string hardwareMessage: hardware.normalized_storage ? hardware.normalized_storage.model + " · " + hardware.normalized_storage.capacity_gb.toFixed(0) + " GB" : "System data unavailable"

    property string performanceStatus: hardware.cpu ? hardware.cpu.usage_percent.toFixed(1) + "% CPU" : "Unknown"
    property string performanceMessage: hardware.memory ? "RAM " + ((hardware.memory.used_gb / hardware.memory.total_gb) * 100).toFixed(1) + "% · " + hardware.cpu.frequency_ghz.toFixed(2) + " GHz" : "System data unavailable"

    property string firmwareStatus: hardware.firmware ? hardware.firmware.status : "Unknown"
    property string changesStatus: hardware.changes ? hardware.changes.status : "Unknown"

    property string cpuDetails: hardware.cpu ? hardware.cpu.cores + " cores · " + hardware.cpu.threads + " threads · " + hardware.cpu.frequency_ghz.toFixed(2) + " GHz" : "System data unavailable"
    property string gpuDetails: hardware.gpu ? hardware.gpu.model + " · " + hardware.gpu.capability_status : "System data unavailable"
    property string memoryDetails: hardware.memory ? hardware.memory.used_gb.toFixed(1) + " / " + hardware.memory.total_gb.toFixed(1) + " GB used" : "System data unavailable"
    property string storageDetails: hardware.normalized_storage ? hardware.normalized_storage.model + " · " + hardware.normalized_storage.capacity_gb.toFixed(0) + " GB" : "System data unavailable"
    property string networkDetails: hardware.network ? "Ping " + hardware.network.ping_ms.toFixed(1) + " ms · ↓ " + hardware.network.download_mbps.toFixed(2) + " Mbps · ↑ " + hardware.network.upload_mbps.toFixed(2) + " Mbps" : "System data unavailable"
    property string firmwareDetails: hardware.firmware ? hardware.firmware.status : "System data unavailable"
    property string capabilitiesDetails: hardware.capabilities ? "GPU acceleration: " + (hardware.capabilities.gpu_acceleration ? "Yes" : "No") + " · VRAM: " + hardware.capabilities.gpu_vram_gb.toFixed(1) + " GB · Thread capacity: " + hardware.capabilities.thread_capacity : "System data unavailable"
    property string changesDetails: hardware.changes ? hardware.changes.message : "System data unavailable"
}
