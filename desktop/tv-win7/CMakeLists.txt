cmake_minimum_required(VERSION 3.20)
project(ZethropolTVWin7 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build host unit tests" ON)

add_library(zethropol_tv_win7_core STATIC
    src/StreamSecurity.cpp
    src/ChannelCatalog.cpp
    src/StreamResolver.cpp
)

target_include_directories(zethropol_tv_win7_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    target_compile_options(zethropol_tv_win7_core PRIVATE /W4)
else()
    target_compile_options(zethropol_tv_win7_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WIN32)
    set(OPENSSL_WIN32_DIR "" CACHE PATH "Path to cross-compiled 32-bit OpenSSL prefix")
    set(LIBMPV_WIN32_INCLUDE_DIR "" CACHE PATH "Path to libmpv include directory")

    if (NOT OPENSSL_WIN32_DIR)
        message(FATAL_ERROR "OPENSSL_WIN32_DIR must point to the 32-bit OpenSSL prefix")
    endif()
    if (NOT LIBMPV_WIN32_INCLUDE_DIR)
        message(FATAL_ERROR "LIBMPV_WIN32_INCLUDE_DIR must point to the libmpv include directory")
    endif()

    enable_language(RC)

    add_executable(ZethropolTV WIN32
        src/main.cpp
        src/MainWindow.cpp
        src/HttpsClient.cpp
        src/MpvController.cpp
        resources/zethropol-tv.rc
    )

    target_include_directories(ZethropolTV PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
        ${OPENSSL_WIN32_DIR}/include
        ${LIBMPV_WIN32_INCLUDE_DIR}
    )

    target_compile_definitions(ZethropolTV PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
    )

    target_compile_options(ZethropolTV PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-march=i686>
        $<$<COMPILE_LANGUAGE:CXX>:-mtune=generic>
    )

    target_link_libraries(ZethropolTV PRIVATE
        zethropol_tv_win7_core
        ${OPENSSL_WIN32_DIR}/lib/libssl.dll.a
        ${OPENSSL_WIN32_DIR}/lib/libcrypto.dll.a
        ws2_32
        crypt32
        comctl32
        gdi32
        user32
        shell32
        ole32
        winmm
        uxtheme
        msimg32
    )

    if (MINGW)
        target_link_options(ZethropolTV PRIVATE
            -municode
            -static
            -static-libgcc
            -static-libstdc++
            -Wl,--subsystem,windows:6.01
        )
    endif()
endif()

if (BUILD_TESTING AND NOT WIN32)
    enable_testing()
    add_executable(tv_win7_core_test
        tests/Win7CoreTest.cpp
    )
    target_link_libraries(tv_win7_core_test PRIVATE zethropol_tv_win7_core)
    target_compile_options(tv_win7_core_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME tv_win7_core_test COMMAND tv_win7_core_test)
endif()
