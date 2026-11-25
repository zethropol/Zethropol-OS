cmake_minimum_required(VERSION 3.20)
project(zethropol_system_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)

# Build the standalone Qt6/QML monitoring bridge (`org.zethropol.monitoring 1.0`)
# and its own unit/integration test suite.
add_subdirectory(bridge)

# Standard Plasma 6 KPackage installation layout (production files only;
# test binaries and tests/qml-stubs are intentionally excluded).
set(ZETHROPOL_PLASMOID_INSTALL_DIR "share/plasma/plasmoids/org.zethropol.systemmonitor"
    CACHE PATH "Plasma 6 plasmoid KPackage installation directory")
install(FILES metadata.json
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}"
)
install(FILES contents/ui/main.qml
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}/contents/ui"
)
install(FILES contents/ui/config/Appearance.qml
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}/contents/ui/config"
)
install(FILES
    contents/config/main.xml
    contents/config/config.qml
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}/contents/config"
)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 DBus Test)

    add_executable(system_monitor_qml_test
        tests/SystemMonitorQmlTest.cpp
    )
    target_compile_options(system_monitor_qml_test PRIVATE -Wall -Wextra)
    # Intentionally does NOT link zethropol_monitoring_bridge_core so the test
    # proves `contents/ui/main.qml` dynamically loads the built
    # `org.zethropol.monitoring 1.0` QML plugin from the configured import path.
    target_link_libraries(system_monitor_qml_test PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::DBus
        Qt6::Test
    )
    target_compile_definitions(system_monitor_qml_test PRIVATE
        SYSTEM_MONITOR_QML_IMPORT_DIR="${CMAKE_CURRENT_BINARY_DIR}/bridge"
        SYSTEM_MONITOR_TEST_STUB_IMPORT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml-stubs"
        SYSTEM_MONITOR_MAIN_QML="${CMAKE_CURRENT_SOURCE_DIR}/contents/ui/main.qml"
        SYSTEM_MONITOR_APPEARANCE_QML="${CMAKE_CURRENT_SOURCE_DIR}/contents/ui/config/Appearance.qml"
        SYSTEM_MONITOR_MAIN_XML="${CMAKE_CURRENT_SOURCE_DIR}/contents/config/main.xml"
        SYSTEM_MONITOR_METADATA_JSON="${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
    )
    add_dependencies(system_monitor_qml_test zethropol_monitoring_qml)

    add_test(NAME system_monitor_qml_test
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/bridge/tests/bridge-private-bus.sh"
            $<TARGET_FILE:system_monitor_qml_test>
    )
    set_tests_properties(system_monitor_qml_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_QPA_PLATFORMTHEME="
        TIMEOUT 90
    )
endif()
