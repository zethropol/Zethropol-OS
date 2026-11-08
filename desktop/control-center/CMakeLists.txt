cmake_minimum_required(VERSION 3.16)
project(control_center LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick DBus)

set(ZETHROPOL_DBUS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_security_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_services_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_recovery_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_applications_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_power_client.cpp
   ${CMAKE_CURRENT_SOURCE_DIR}/src/core/zethropol_monitoring_client.cpp
)

set(CONTROL_CENTER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UpdateScanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/UpdateInstaller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/HardwareServiceClient.cpp
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
        qml/UpdateReport.qml
        qml/SystemState.qml
)

target_link_libraries(control_center PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::DBus
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(control_center_update_scanner_test
        tests/UpdateScannerTest.cpp
        src/UpdateScanner.cpp
        src/UpdateScanner.h
    )
    target_include_directories(control_center_update_scanner_test PRIVATE src)
    target_link_libraries(control_center_update_scanner_test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME control_center_update_scanner_test
             COMMAND control_center_update_scanner_test)

    qt_add_executable(control_center_update_installer_test
        tests/UpdateInstallerTest.cpp
        src/UpdateInstaller.cpp src/UpdateInstaller.h
        src/UpdateScanner.cpp src/UpdateScanner.h)
    target_include_directories(control_center_update_installer_test PRIVATE src)
    target_link_libraries(control_center_update_installer_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
    target_compile_definitions(control_center_update_installer_test PRIVATE
        UPDATE_REPORT_QML="${CMAKE_CURRENT_SOURCE_DIR}/qml/UpdateReport.qml")
    add_test(NAME control_center_update_installer_test
             COMMAND control_center_update_installer_test)
    set_tests_properties(control_center_update_installer_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
