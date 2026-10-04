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
)

qt_add_executable(control_center
    src/main.cpp
    ${ZETHROPOL_DBUS_SRCS}
)

set_source_files_properties(src/main.qml PROPERTIES QT_RESOURCE_ALIAS "main.qml")

qt_add_resources(control_center "control_center_qml"
    PREFIX "/"
    FILES
        src/main.qml
)

target_link_libraries(control_center PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::DBus
)
