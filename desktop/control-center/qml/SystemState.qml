pragma Singleton
import QtQml

QtObject {
    property string overallHealth: "Unknown"

    property string cpuStatus: "Unknown"
    property string cpuMessage: "System data unavailable"

    property string gpuStatus: "Unknown"
    property string gpuMessage: "System data unavailable"

    property string memoryStatus: "Unknown"
    property string memoryMessage: "System data unavailable"

    property string storageStatus: "Unknown"
    property string storageMessage: "System data unavailable"

    property string networkStatus: "Unknown"
    property string networkMessage: "System data unavailable"
} 
