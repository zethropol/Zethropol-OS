cmake_minimum_required(VERSION 3.24)
project(zethropol-control-center VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_standard_project_setup()

qt_add_executable(zethropol-control-center
    src/main.cpp
    src/HardwareServiceClient.cpp
)

set_source_files_properties(qml/SystemState.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(zethropol-control-center
    URI Zethropol.ControlCenter
    VERSION 1.0
    QML_FILES
        qml/SystemState.qml
        qml/Main.qml
)

target_link_libraries(zethropol-control-center PRIVATE Qt6::Quick)

include(GNUInstallDirs)
install(TARGETS zethropol-control-center
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
