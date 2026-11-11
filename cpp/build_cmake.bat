@echo off
setlocal

echo ==============================================================
echo   Building KDE Connect SMS Win32 using CMake
echo ==============================================================

if not exist "build" mkdir build
cd build

cmake -G "Visual Studio 17 2022" -A x64 ..
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed!
    exit /b 1
)

cmake --build . --config Release --parallel
if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    exit /b 1
)

echo.
echo [SUCCESS] Binaries built in build\Release\
cd ..
