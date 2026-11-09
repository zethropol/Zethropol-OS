pragma Singleton
import QtQml

QtObject {
    function numeric(value, digits, suffix) {
        return typeof value === "number" && isFinite(value) ? value.toFixed(digits) + suffix : "Unavailable"
    }
    function known(value) {
        return value !== null && value !== undefined && String(value) !== "" ? String(value) : "Unavailable"
    }
    property var hardware: HardwareBridge.state

    property var system: hardware.system
    property var power: hardware.power
    property string powerAvailability: power ? (power.available ? "Available" : "Unavailable") : "Unknown"
    property string powerSource: power && power.battery_present ? "Battery" : (power && power.ac_online === true ? "AC Power" : "No battery detected")
    property string batteryLevel: power && power.battery_percent !== null && power.battery_percent !== undefined ? numeric(power.battery_percent, 1, "") + "%" : "N/A"
    property string batteryStatus: power && power.battery_status ? power.battery_status : "N/A"
    property string chargingStatus: power && power.charging !== null && power.charging !== undefined ? (power.charging ? "Charging" : "Not charging") : "N/A"
    property string powerUsage: power && power.power_w !== null && power.power_w !== undefined ? numeric(power.power_w, 2, "") + " W" : "N/A"
    property string energyNow: power && power.energy_now_wh !== null && power.energy_now_wh !== undefined ? numeric(power.energy_now_wh, 2, "") + " Wh" : "N/A"
    property string energyFull: power && power.energy_full_wh !== null && power.energy_full_wh !== undefined ? numeric(power.energy_full_wh, 2, "") + " Wh" : "N/A"
    property string powerProfile: power && power.profile ? power.profile : "Not available"
    property string powerGovernor: power && power.governor ? power.governor : "Not available"

    property string osName: system ? known(system.os_name) : "Unknown"
    property string osVersion: system && system.os_version !== "Unknown" ? known(system.os_version) : "Not defined"
    property string baseSystem: system ? known(system.base_system) : "Unknown"
    property string architecture: system ? known(system.architecture) : "Unknown"
    property string kernelVersion: system ? known(system.kernel_version) : "Unknown"
    property string desktopName: system ? known(system.desktop_name) : "Unknown"
    property string desktopVersion: system ? known(system.desktop_version) : "Unknown"
    property string sessionType: system ? known(system.session_type) : "Unknown"
    property string hostname: system ? known(system.hostname) : "Unknown"
    property string uptimeDetails: system && system.uptime_seconds !== null && system.uptime_seconds !== undefined ? Math.floor(system.uptime_seconds / 86400) + "d " + Math.floor((system.uptime_seconds % 86400) / 3600) + "h " + Math.floor((system.uptime_seconds % 3600) / 60) + "m" : "Unknown"

    property string overallHealth: hardware.health ? known(hardware.health.overall_status) : "Unknown"

    property string cpuStatus: hardware.health ? known(hardware.health.cpu_status) : "Unknown"
    property string cpuMessage: hardware.cpu ? known(hardware.cpu.model) : "System data unavailable"

    property string gpuStatus: hardware.health ? known(hardware.health.gpu_status) : "Unknown"
    property string gpuMessage: hardware.gpu ? known(hardware.gpu.capability_status) : "System data unavailable"
    property string gpuModel: hardware.gpu ? known(hardware.gpu.model) : "Unknown"
    property string gpuFamily: hardware.gpu ? known(hardware.gpu.family) : "Unknown"

    property string memoryStatus: hardware.health ? known(hardware.health.memory_status) : "Unknown"
    property string memoryMessage: hardware.memory ? "Used " + numeric(hardware.memory.used_gb, 1, "") + " / " + numeric(hardware.memory.total_gb, 1, "") + " GB" : "System data unavailable"

    property string storageStatus: hardware.health ? known(hardware.health.storage_status) : "Unknown"
    property string storageMessage: hardware.normalized_storage ? known(hardware.normalized_storage.model) : "System data unavailable"

    property string networkStatus: hardware.network && hardware.network.operstate ? known(hardware.network.operstate) : "Unknown"
    property string networkMessage: hardware.network ? "Ping " + numeric(hardware.network.ping_ms, 1, "") + " ms" : "System data unavailable"

    property string systemStatus: hardware.health ? known(hardware.health.overall_status) : "Unknown"
    property string systemMessage: hardware.health ? known(hardware.health.driver_status) : "System data unavailable"

    property string hardwareSummary: hardware.cpu && hardware.gpu ? hardware.cpu.model + "
" + hardware.gpu.model : "System data unavailable"
    property string hardwareMessage: hardware.normalized_storage ? known(hardware.normalized_storage.model) + " · " + numeric(hardware.normalized_storage.capacity_gb, 0, "") + " GB" : "System data unavailable"

    property string performanceStatus: hardware.cpu ? numeric(hardware.cpu.usage_percent, 1, "") + "% CPU" : "Unknown"
    property string performanceMessage: hardware.memory && hardware.cpu ? "RAM " + numeric(hardware.memory.total_gb > 0 && typeof hardware.memory.used_gb === "number" ? (hardware.memory.used_gb / hardware.memory.total_gb) * 100 : undefined, 1, "") + "% · " + numeric(hardware.cpu.frequency_ghz, 2, "") + " GHz" : "System data unavailable"

    property string diagnosticsHealthStatus: hardware.diagnostics ? known(hardware.diagnostics.health_status) : "Unknown"
    property string diagnosticsTemperature: hardware.diagnostics && hardware.diagnostics.temperature_c !== null && hardware.diagnostics.temperature_c !== undefined ? numeric(hardware.diagnostics.temperature_c, 1, "") + " °C" : "Unknown"
    property string diagnosticsPercentageUsed: hardware.diagnostics && hardware.diagnostics.percentage_used !== null && hardware.diagnostics.percentage_used !== undefined ? numeric(hardware.diagnostics.percentage_used, 1, "") + "%" : "Unknown"
    property string diagnosticsAvailableSpare: hardware.diagnostics && hardware.diagnostics.available_spare_percent !== null && hardware.diagnostics.available_spare_percent !== undefined ? numeric(hardware.diagnostics.available_spare_percent, 1, "") + "%" : "Unknown"
    property string diagnosticsMediaErrors: hardware.diagnostics && hardware.diagnostics.media_data_integrity_errors !== null && hardware.diagnostics.media_data_integrity_errors !== undefined ? String(hardware.diagnostics.media_data_integrity_errors) : "Unknown"
    property string diagnosticsUnsafeShutdowns: hardware.diagnostics && hardware.diagnostics.unsafe_shutdowns !== null && hardware.diagnostics.unsafe_shutdowns !== undefined ? String(hardware.diagnostics.unsafe_shutdowns) : "Unknown"
    property string diagnosticsErrorLogEntries: hardware.diagnostics && hardware.diagnostics.error_log_entries !== null && hardware.diagnostics.error_log_entries !== undefined ? String(hardware.diagnostics.error_log_entries) : "Unknown"
    property string diagnosticsUnsafeShutdownsStatus: hardware.diagnostics ? known(hardware.diagnostics.unsafe_shutdowns_status) : "Unknown"
    property string diagnosticsErrorLogStatus: hardware.diagnostics ? known(hardware.diagnostics.error_log_status) : "Unknown"
    property string diagnosticsSelfTestStatus: hardware.diagnostics ? known(hardware.diagnostics.self_test_status) : "Unknown"
    property string diagnosticsDetails: hardware.diagnostics ? known(hardware.diagnostics.health_status) : "System data unavailable"

    property string firmwareStatus: hardware.firmware ? known(hardware.firmware.status) : "Unknown"
    property string changesStatus: hardware.changes ? known(hardware.changes.status) : "Unknown"

    property string cpuDetails: hardware.cpu ? hardware.cpu.cores + " cores · " + hardware.cpu.threads + " threads · " + numeric(hardware.cpu.frequency_ghz, 2, "") + " GHz" : "System data unavailable"
    property string gpuDetails: hardware.gpu ? known(hardware.gpu.model) + " · " + hardware.gpu.capability_status : "System data unavailable"
    property string memoryDetails: hardware.memory ? numeric(hardware.memory.used_gb, 1, "") + " / " + numeric(hardware.memory.total_gb, 1, "") + " GB used" : "System data unavailable"
    property string storageDetails: hardware.normalized_storage ? known(hardware.normalized_storage.model) + " · " + numeric(hardware.normalized_storage.capacity_gb, 0, "") + " GB" : "System data unavailable"
    property string networkDetails: hardware.network ? "Ping " + numeric(hardware.network.ping_ms, 1, "") + " ms · ↓ " + numeric(hardware.network.download_mbps, 2, "") + " Mbps · ↑ " + numeric(hardware.network.upload_mbps, 2, "") + " Mbps" : "System data unavailable"
    property string firmwareDetails: hardware.firmware ? known(hardware.firmware.status) : "System data unavailable"
    property string capabilitiesDetails: hardware.capabilities ? "GPU acceleration: " + (typeof hardware.capabilities.gpu_acceleration === "boolean" ? (hardware.capabilities.gpu_acceleration ? "Yes" : "No") : "Unverified") + " · VRAM: " + numeric(hardware.capabilities.gpu_vram_gb, 1, "") + " GB · Thread capacity: " + hardware.capabilities.thread_capacity : "System data unavailable"
    property string changesDetails: hardware.changes ? known(hardware.changes.message) : "System data unavailable"
}
