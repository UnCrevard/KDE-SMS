# KDE Connect SMS for Windows (Native C++ Win32)

A pure native Win32 Unicode C++20 implementation of the KDE Connect SMS synchronization subsystem, built for maximum performance, minimal memory footprint (<25 MB), and zero web overhead (no WebView2, no Electron).

## Features

- **Direct2D & DirectWrite Hardware Acceleration**: Smooth 60fps scrolling, anti-aliased vector bubbles, and full multi-color emoji support via `D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT` and `Segoe UI Emoji`.
- **Windows Imaging Component (WIC)**: Native decoding and rendering of incoming MMS picture attachments (JPEG, PNG, GIF, WebP).
- **Background Daemon & Real-Time Sync**: Asynchronous directory monitoring via `ReadDirectoryChangesW` watching `%APPDATA%\KDEConnectSMS\messages\`.
- **KDE Connect Network Protocol**:
  - UDP 1716 discovery broadcasting and receiving (`kdeconnect.identity`).
  - TCP 1716 JSON packet framing.
  - Implements `kdeconnect.telephony`, `kdeconnect.sms.messages`, `kdeconnect.sms.request`, and `kdeconnect.battery`.
- **Native Windows Toast Notifications**: Windows 10/11 WinRT Toast Notifications (`Windows.UI.Notifications.ToastNotificationManager`) with Quick Reply action input and desktop shell balloon fallback.
- **Adaptive Dark / Light Theme**:
  - Automatically queries `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`.
  - Listens to `WM_SETTINGCHANGE` for real-time OS theme switching.
  - Injects `DWMWA_USE_IMMERSIVE_DARK_MODE` (attribute 20/19) into DWM for native title bar theme matching.
  - Dynamic Direct2D brush recreation without flickering.

## Project Structure

```
cpp/
├── CMakeLists.txt              # Standard CMake build script
├── build_msvc.bat              # 1-click MSVC command prompt compilation
├── build_cmake.bat             # 1-click CMake + Visual Studio 2022 build
├── README.md                   # Technical documentation
├── resources/
│   ├── app.manifest            # Per-Monitor V2 DPI & Common-Controls v6
│   └── app.rc                  # Win32 resource file with version information
├── include/
│   ├── common.h                # Global Unicode definitions, data models
│   ├── theme_manager.h         # Dark/Light DWM & color palette management
│   ├── direct2d_renderer.h     # Direct2D, DirectWrite color emoji & WIC
│   ├── kdeconnect_protocol.h   # KDE Connect UDP & TCP socket protocol
│   ├── daemon.h                # ReadDirectoryChangesW watcher & IPC server
│   └── toast_notifier.h        # Windows Toast & Shell Notifications
└── src/
    ├── theme_manager.cpp       # Theme registry query & DWM attribute
    ├── direct2d_renderer.cpp   # Direct2D rendering implementation
    ├── kdeconnect_protocol.cpp # KDE packet framing & serialization
    ├── daemon.cpp              # Directory watcher & message storage
    ├── toast_notifier.cpp      # WinRT toast & notify icon implementation
    ├── main_ui.cpp             # Win32 GUI client entry point & WndProc
    └── main_daemon.cpp         # Standalone headless daemon service
```

## Compilation Instructions

### Prerequisites
- Windows 10 (1809+) or Windows 11
- Visual Studio 2022 (Community, Professional, or Enterprise) with:
  - "Desktop development with C++"
  - "Windows 10/11 SDK (10.0.19041.0 or newer)"

### Option 1: Visual Studio Developer Command Prompt (Fastest)
1. Open **x64 Native Tools Command Prompt for VS 2022**.
2. Navigate to the `cpp` directory:
   ```cmd
   cd cpp
   build_msvc.bat
   ```
3. The binaries will be created in `bin\`:
   - `bin\KDEConnectSMS.exe` (Main Win32 GUI Client)
   - `bin\KDEConnectDaemon.exe` (Background Sync Daemon)

### Option 2: CMake
1. Open a terminal in `cpp/`:
   ```cmd
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
2. Binaries will be generated in `build\Release\`.

### Option 3: Clang-cl / LLVM
```cmd
clang-cl /std:c++20 /O2 /W4 /utf-8 /DUNICODE /D_UNICODE /Iinclude ^
    src\*.cpp resources\app.rc ^
    /link /SUBSYSTEM:WINDOWS /OUT:KDEConnectSMS.exe ^
    d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib comctl32.lib ws2_32.lib crypt32.lib secur32.lib shlwapi.lib ole32.lib shell32.lib runtimeobject.lib
```
