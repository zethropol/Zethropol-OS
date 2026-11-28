cmake_minimum_required(VERSION 3.20)
project(zethropol_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Qml)

qt_standard_project_setup(REQUIRES 6.5)

include(CTest)

# Static core library shared by the QML plugin and unit tests
add_library(zethropol_weather_core STATIC
    plugin/src/WeatherModels.h
    plugin/src/WeatherModels.cpp
    plugin/src/WeatherClient.h
    plugin/src/WeatherClient.cpp
)
set_target_properties(zethropol_weather_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(zethropol_weather_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src
)
target_compile_options(zethropol_weather_core PRIVATE -Wall -Wextra)
target_link_libraries(zethropol_weather_core PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Qml
)

# Standalone Qt6/QML module plugin (`import org.zethropol.weather 1.0`)
# Installed under /usr/lib/qt6/qml/org/zethropol/weather/ for KDE Plasma 6.
qt_add_qml_module(zethropol_weather_qml
    URI org.zethropol.weather
    VERSION 1.0
    PLUGIN_TARGET zethropol_weather_qml
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/org/zethropol/weather"
    SOURCES
        plugin/src/WeatherModels.h
        plugin/src/WeatherModels.cpp
        plugin/src/WeatherClient.h
        plugin/src/WeatherClient.cpp
)
target_include_directories(zethropol_weather_qml PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src
)
target_compile_options(zethropol_weather_qml PRIVATE -Wall -Wextra)
target_link_libraries(zethropol_weather_qml PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Qml
)
set_target_properties(zethropol_weather_qml PROPERTIES
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Production install layout: Plasma 6 KPackage + Qt6 QML plugin
set(ZETHROPOL_PLASMOID_INSTALL_DIR "share/plasma/plasmoids/org.zethropol.plasma.weather"
    CACHE PATH "Plasma 6 plasmoid KPackage installation directory")
set(ZETHROPOL_QML_INSTALL_DIR "lib/qt6/qml"
    CACHE PATH "Qt6 QML module installation directory")

install(FILES metadata.json
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}"
)
install(FILES
    contents/config/main.xml
    contents/config/config.qml
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}/contents/config"
)
install(FILES
    contents/ui/main.qml
    contents/ui/CompactRepresentation.qml
    contents/ui/FullRepresentation.qml
    contents/ui/configGeneral.qml
    DESTINATION "${ZETHROPOL_PLASMOID_INSTALL_DIR}/contents/ui"
)
install(TARGETS zethropol_weather_qml
    LIBRARY DESTINATION "${ZETHROPOL_QML_INSTALL_DIR}/org/zethropol/weather"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qml/org/zethropol/weather/qmldir"
    "${CMAKE_CURRENT_BINARY_DIR}/qml/org/zethropol/weather/zethropol_weather_qml.qmltypes"
    DESTINATION "${ZETHROPOL_QML_INSTALL_DIR}/org/zethropol/weather"
)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Gui Quick QuickControls2 Test)

    add_executable(weather_core_test
        tests/WeatherCoreTest.cpp
    )
    target_compile_options(weather_core_test PRIVATE -Wall -Wextra)
    target_link_libraries(weather_core_test PRIVATE
        zethropol_weather_core
        Qt6::Core
        Qt6::Network
        Qt6::Test
    )
    add_test(NAME weather_core_test COMMAND weather_core_test)
    set_tests_properties(weather_core_test PROPERTIES TIMEOUT 60)

    add_executable(weather_qml_test
        tests/WeatherQmlTest.cpp
    )
    target_compile_options(weather_qml_test PRIVATE -Wall -Wextra)
    target_link_libraries(weather_qml_test PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    target_compile_definitions(weather_qml_test PRIVATE
        WEATHER_QML_IMPORT_DIR="${CMAKE_CURRENT_BINARY_DIR}/qml"
        WEATHER_TEST_STUB_IMPORT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml-stubs"
        WEATHER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contents/ui"
        WEATHER_MAIN_XML="${CMAKE_CURRENT_SOURCE_DIR}/contents/config/main.xml"
        WEATHER_CONFIG_QML="${CMAKE_CURRENT_SOURCE_DIR}/contents/config/config.qml"
        WEATHER_METADATA_JSON="${CMAKE_CURRENT_SOURCE_DIR}/metadata.json"
    )
    add_dependencies(weather_qml_test zethropol_weather_qml)
    add_test(NAME weather_qml_test COMMAND weather_qml_test)
    set_tests_properties(weather_qml_test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=Basic;QT_QPA_PLATFORMTHEME="
        TIMEOUT 60
    )
endif()
