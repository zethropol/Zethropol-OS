cmake_minimum_required(VERSION 3.24)
project(zethropol-dbus-prototype LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core DBus)

qt_standard_project_setup()

qt_add_executable(zethropol-dbus-service
    service.cpp
)

target_link_libraries(zethropol-dbus-service PRIVATE
    Qt6::Core
    Qt6::DBus
)

qt_add_executable(zethropol-dbus-client
    client.cpp
)

target_link_libraries(zethropol-dbus-client PRIVATE
    Qt6::Core
    Qt6::DBus
)
