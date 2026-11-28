cmake_minimum_required(VERSION 3.22)
project(zethropol_zci_agent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Local AF_UNIX D-Bus IPC only; no network stack and no subprocess shell execution.
find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

add_library(zethropol_zci_agent_core STATIC
    src/AgentClient.h
    src/AgentClient.cpp
)
target_include_directories(zethropol_zci_agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(zethropol_zci_agent_core PUBLIC
    Qt6::Core
    Qt6::DBus
)
target_compile_options(zethropol_zci_agent_core PRIVATE -Wall -Wextra -Wpedantic)

qt_add_executable(zci-agent
    src/main.cpp
)
target_link_libraries(zci-agent PRIVATE
    zethropol_zci_agent_core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)
target_compile_options(zci-agent PRIVATE -Wall -Wextra -Wpedantic)

qt_add_qml_module(zci-agent
    URI Zethropol.ZciAgent
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

qt_add_resources(zci-agent "zci_agent_icons"
    PREFIX "/zci-agent"
    FILES
        data/zethropol-zci-agent.svg
)

include(GNUInstallDirs)
install(TARGETS zci-agent RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/zci-agent.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/zethropol-zci-agent.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

option(BUILD_TESTING "Build ZCI Agent QTest suite" ON)
if(BUILD_TESTING)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(zci_agent_qml_test
        tests/ZciAgentQmlTest.cpp
    )
    qt_add_resources(zci_agent_qml_test "zci_agent_test_qml"
        PREFIX "/test-qml"
        FILES
            qml/Main.qml
    )
    target_compile_definitions(zci_agent_qml_test PRIVATE
        ZCI_AGENT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(zci_agent_qml_test PRIVATE
        zethropol_zci_agent_core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_options(zci_agent_qml_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME zci_agent_qml_test COMMAND zci_agent_qml_test)
    set_tests_properties(zci_agent_qml_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_QPA_PLATFORMTHEME="
    )
endif()
