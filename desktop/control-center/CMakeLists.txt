cmake_minimum_required(VERSION 3.16)
project(control_center LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick DBus)

set(ZETHROPOL_DBUS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_security_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_services_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_update_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_recovery_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_applications_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_power_client.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_monitoring_client.cpp
)

set(CONTROL_CENTER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HardwareServiceClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UpdateService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RecoveryService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SecurityService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ServiceService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ApplicationService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NotificationService.cpp
)

qt_add_executable(control_center
    src/main.cpp
    ${CONTROL_CENTER_SRCS}
    ${ZETHROPOL_DBUS_SRCS}
)

set_source_files_properties(qml/SystemState.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(control_center
    URI Zethropol.ControlCenter
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/SystemState.qml
)

target_link_libraries(control_center PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::DBus
)
