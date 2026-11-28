cmake_minimum_required(VERSION 3.20)
project(zethropol_zci_chat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Qml Quick QuickControls2)

include(GNUInstallDirs)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

# Core C++ D-Bus bridge (zero Qt6::Network, zero QProcess)
add_library(zethropol_zci_chat_core STATIC
    src/ChatClient.h
    src/ChatClient.cpp
)
target_include_directories(zethropol_zci_chat_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(zethropol_zci_chat_core PRIVATE -Wall -Wextra)
target_link_libraries(zethropol_zci_chat_core PUBLIC
    Qt6::Core
    Qt6::DBus
)

qt_add_executable(zci-chat
    src/main.cpp
)
target_compile_options(zci-chat PRIVATE -Wall -Wextra)
target_link_libraries(zci-chat PRIVATE
    zethropol_zci_chat_core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

qt_add_qml_module(zci-chat
    URI Zethropol.ZciChat
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

qt_add_resources(zci-chat "zci_chat_icons"
    PREFIX "/icons"
    BASE "data"
    FILES
        data/zethropol-zci-chat.svg
)

install(TARGETS zci-chat
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES data/zci-chat.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES data/zethropol-zci-chat.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(zci_chat_qml_test
        tests/ZciChatQmlTest.cpp
    )
    target_compile_options(zci_chat_qml_test PRIVATE -Wall -Wextra)
    target_compile_definitions(zci_chat_qml_test PRIVATE
        ZCI_CHAT_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
        ZCI_CHAT_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
        ZCI_CHAT_DESKTOP_FILE="${CMAKE_CURRENT_SOURCE_DIR}/data/zci-chat.desktop"
    )
    target_link_libraries(zci_chat_qml_test PRIVATE
        zethropol_zci_chat_core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    qt_add_resources(zci_chat_qml_test "zci_chat_test_qml"
        PREFIX "/qt/qml/Zethropol/ZciChat"
        FILES
            qml/Main.qml
    )
    add_test(NAME zci_chat_qml_test COMMAND zci_chat_qml_test)
    set_tests_properties(zci_chat_qml_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_QPA_PLATFORMTHEME="
        TIMEOUT 60
    )
endif()
