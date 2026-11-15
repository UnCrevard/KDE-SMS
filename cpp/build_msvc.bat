@echo off
setlocal enabledelayedexpansion

echo ==============================================================
echo   Building KDE Connect SMS Win32 with MSVC Developer Command Prompt
echo ==============================================================

:: Check for cl.exe in PATH
where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] cl.exe not found in PATH!
    echo Please run this script from the "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
)

if not exist "bin" mkdir bin
if not exist "bin\obj" mkdir bin\obj

echo [*] Compiling resource script...
rc.exe /fo bin\obj\app.res resources\app.rc
if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile resources!
    exit /b 1
)

echo [*] Compiling Core & Daemon sources...
cl.exe /nologo /O2 /W4 /utf-8 /EHsc /std:c++20 /DUNICODE /D_UNICODE /Iinclude ^
    src\theme_manager.cpp src\i18n.cpp src\kdeconnect_protocol.cpp src\toast_notifier.cpp src\daemon.cpp src\main_daemon.cpp ^
    bin\obj\app.res ^
    /link /OUT:bin\KDEConnectDaemon.exe ^
    ws2_32.lib crypt32.lib secur32.lib shlwapi.lib ole32.lib shell32.lib advapi32.lib dwmapi.lib runtimeobject.lib

if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile Daemon!
    exit /b 1
)

echo [*] Compiling Win32 Direct2D SMS Client GUI...
cl.exe /nologo /O2 /W4 /utf-8 /EHsc /std:c++20 /DUNICODE /D_UNICODE /Iinclude ^
    src\theme_manager.cpp src\i18n.cpp src\kdeconnect_protocol.cpp src\toast_notifier.cpp src\daemon.cpp src\direct2d_renderer.cpp src\main_ui.cpp ^
    bin\obj\app.res ^
    /link /SUBSYSTEM:WINDOWS /OUT:bin\KDEConnectSMS.exe ^
    d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib comctl32.lib ws2_32.lib crypt32.lib secur32.lib shlwapi.lib ole32.lib shell32.lib comdlg32.lib advapi32.lib runtimeobject.lib

if %errorlevel% neq 0 (
    echo [ERROR] Failed to compile UI!
    exit /b 1
)

echo.
echo ==============================================================
echo [SUCCESS] Build completed successfully!
echo Executables generated:
echo   - bin\KDEConnectSMS.exe   (Native Direct2D GUI Client)
echo   - bin\KDEConnectDaemon.exe (Background Sync Service)
echo ==============================================================
