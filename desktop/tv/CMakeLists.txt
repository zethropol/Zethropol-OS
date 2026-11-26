cmake_minimum_required(VERSION 3.20)
project(zethropol_tv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network OpenGL)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

include(GNUInstallDirs)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

# Core C++ library (independent of ZCI and D-Bus)
add_library(zethropol_tv_core STATIC
    src/StreamSecurity.h
    src/StreamSecurity.cpp
    src/StreamResolver.h
    src/StreamResolver.cpp
    src/MpvItem.h
    src/MpvItem.cpp
    src/ChannelManager.h
    src/ChannelManager.cpp
    src/EpgManager.h
    src/EpgManager.cpp
)
target_include_directories(zethropol_tv_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(zethropol_tv_core PRIVATE -Wall -Wextra)
target_link_libraries(zethropol_tv_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::OpenGL
    ZLIB::ZLIB
    PkgConfig::MPV
)

qt_add_executable(zethropol-tv
    src/main.cpp
)
target_compile_options(zethropol-tv PRIVATE -Wall -Wextra)
target_link_libraries(zethropol-tv PRIVATE
    zethropol_tv_core
)

qt_add_qml_module(zethropol-tv
    URI Zethropol.TV
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/VideoOsd.qml
        qml/ChannelSidebar.qml
        qml/EpgTimelineBar.qml
        qml/SourceImportDialog.qml
)

# Standard /usr installation targets (used by Arch PKGBUILD / isolated DESTDIR checks)
install(TARGETS zethropol-tv
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES data/org.zethropol.tv.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)
install(FILES data/zethropol-tv.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(tv_m3u_security_test
        tests/M3uSecurityTest.cpp
    )
    target_compile_options(tv_m3u_security_test PRIVATE -Wall -Wextra)
    target_link_libraries(tv_m3u_security_test PRIVATE
        zethropol_tv_core
        Qt6::Test
    )
    add_test(NAME tv_m3u_security_test COMMAND tv_m3u_security_test)

    qt_add_executable(tv_epg_window_test
        tests/EpgWindowTest.cpp
    )
    target_compile_options(tv_epg_window_test PRIVATE -Wall -Wextra)
    target_link_libraries(tv_epg_window_test PRIVATE
        zethropol_tv_core
        Qt6::Test
    )
    add_test(NAME tv_epg_window_test COMMAND tv_epg_window_test)

    qt_add_executable(tv_stream_resolver_test
        tests/StreamResolverTest.cpp
    )
    target_compile_options(tv_stream_resolver_test PRIVATE -Wall -Wextra)
    target_link_libraries(tv_stream_resolver_test PRIVATE
        zethropol_tv_core
        Qt6::Test
    )
    add_test(NAME tv_stream_resolver_test COMMAND tv_stream_resolver_test)
    set_tests_properties(tv_stream_resolver_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        TIMEOUT 60
    )

    qt_add_executable(tv_qml_smoke_test
        tests/TvQmlSmokeTest.cpp
    )
    target_compile_options(tv_qml_smoke_test PRIVATE -Wall -Wextra)
    target_link_libraries(tv_qml_smoke_test PRIVATE
        zethropol_tv_core
        Qt6::Test
    )
    qt_add_resources(tv_qml_smoke_test "tv_smoke_qml"
        PREFIX "/qt/qml/Zethropol/TV"
        FILES
            qml/Main.qml
            qml/VideoOsd.qml
            qml/ChannelSidebar.qml
            qml/EpgTimelineBar.qml
            qml/SourceImportDialog.qml
    )
    add_test(NAME tv_qml_smoke_test COMMAND tv_qml_smoke_test)
    set_tests_properties(tv_qml_smoke_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_QPA_PLATFORMTHEME="
        TIMEOUT 60
    )
endif()
