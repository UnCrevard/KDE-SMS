cmake_minimum_required(VERSION 3.20)
project(KDEConnectSMSWin32 VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce Unicode build across all targets
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
    # Enable High DPI and modern Windows features
    add_link_options("/MANIFEST:EMBED" "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")
endif()

include_directories(include)

# Source files for the core library
set(CORE_SOURCES
    src/theme_manager.cpp
    src/kdeconnect_protocol.cpp
    src/toast_notifier.cpp
    src/daemon.cpp
)

# UI specific sources
set(UI_SOURCES
    src/direct2d_renderer.cpp
    src/main_ui.cpp
    resources/app.rc
)

# Daemon executable (background headless service / tray worker)
add_executable(kdeconnect_daemon
    src/main_daemon.cpp
    ${CORE_SOURCES}
    resources/app.rc
)

target_link_libraries(kdeconnect_daemon PRIVATE
    ws2_32
    crypt32
    secur32
    shlwapi
    windowsapp
)

# UI executable (Direct2D / DirectWrite native Win32 window)
add_executable(kdeconnect_sms_ui WIN32
    ${UI_SOURCES}
    ${CORE_SOURCES}
)

target_link_libraries(kdeconnect_sms_ui PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    comctl32
    ws2_32
    crypt32
    secur32
    shlwapi
    windowsapp
    ole32
    shell32
    comdlg32
    advapi32
)

set_target_properties(kdeconnect_sms_ui PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "KDEConnectSMS"
)

set_target_properties(kdeconnect_daemon PROPERTIES
    OUTPUT_NAME "KDEConnectDaemon"
)
