cmake_minimum_required(VERSION 3.20)
project(zethropol_monitoring_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Qml DBus)

qt_standard_project_setup(REQUIRES 6.5)

# Core bridge library (can be linked by tests or embedded without duplicating logic)
add_library(zethropol_monitoring_bridge_core STATIC
    src/MonitoringSharedClient.h
    src/MonitoringSharedClient.cpp
    src/MonitoringBridge.h
    src/MonitoringBridge.cpp
)
set_target_properties(zethropol_monitoring_bridge_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(zethropol_monitoring_bridge_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(zethropol_monitoring_bridge_core PUBLIC
    Qt6::Core
    Qt6::Qml
    Qt6::DBus
)

target_compile_options(zethropol_monitoring_bridge_core PRIVATE -Wall -Wextra)

# Standalone Qt6/QML module plugin (`import org.zethropol.monitoring 1.0`)
# Suitable for Plasma 6 plasmoids when installed under QT_INSTALL_QML (/usr/lib/qt6/qml).
# Setting PLUGIN_TARGET equal to the module target produces a single self-contained
# shared plugin library inside org/zethropol/monitoring/ with no extra /usr/lib .so.
qt_add_qml_module(zethropol_monitoring_qml
    URI org.zethropol.monitoring
    VERSION 1.0
    PLUGIN_TARGET zethropol_monitoring_qml
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/org/zethropol/monitoring"
    SOURCES
        src/MonitoringBridge.h
        src/MonitoringBridge.cpp
        src/MonitoringSharedClient.h
        src/MonitoringSharedClient.cpp
)
target_include_directories(zethropol_monitoring_qml PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(zethropol_monitoring_qml PRIVATE -Wall -Wextra)
target_link_libraries(zethropol_monitoring_qml PRIVATE
    Qt6::Core
    Qt6::Qml
    Qt6::DBus
)
set_target_properties(zethropol_monitoring_qml PROPERTIES
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Packaging install layout (production QML plugin files only)
set(ZETHROPOL_QML_INSTALL_DIR "lib/qt6/qml" CACHE PATH "Qt6 QML import installation directory")
install(TARGETS zethropol_monitoring_qml
    LIBRARY DESTINATION "${ZETHROPOL_QML_INSTALL_DIR}/org/zethropol/monitoring"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/org/zethropol/monitoring/qmldir"
    "${CMAKE_CURRENT_BINARY_DIR}/org/zethropol/monitoring/zethropol_monitoring_qml.qmltypes"
    DESTINATION "${ZETHROPOL_QML_INSTALL_DIR}/org/zethropol/monitoring"
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Quick Test)

    qt_add_executable(monitoring_bridge_test
        tests/MonitoringBridgeTest.cpp
    )
    target_link_libraries(monitoring_bridge_test PRIVATE
        zethropol_monitoring_bridge_core
        Qt6::Core
        Qt6::Qml
        Qt6::Quick
        Qt6::DBus
        Qt6::Test
    )
    target_compile_definitions(monitoring_bridge_test PRIVATE
        BRIDGE_QML_IMPORT_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        BRIDGE_TEST_QML_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml/BridgeConsumer.qml"
    )
    add_dependencies(monitoring_bridge_test zethropol_monitoring_qml)

    add_test(NAME monitoring_bridge_test
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge-private-bus.sh"
            $<TARGET_FILE:monitoring_bridge_test>
    )
    set_tests_properties(monitoring_bridge_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 90
    )
endif()
