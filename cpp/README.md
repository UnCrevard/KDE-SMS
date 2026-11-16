# KDE Connect SMS for Windows (Native Win32 C++)

A pure native Win32 Unicode C++20 implementation of KDE Connect SMS for Windows 10 and Windows 11, built for maximum responsiveness, minimal disk and memory footprint (~1–2 MB binary, < 25 MB RAM), and seamless Windows integration without Qt, QML, Electron, or WebView2 bloat.

## Highlights

- **Ultra-Lightweight Native Win32**: Single standalone executable (`KDEConnectSMS.exe`) linked directly against Windows system APIs — roughly 100× smaller than Qt-based KDE Connect builds.
- **Direct2D & DirectWrite Hardware Acceleration**: Smooth scrolling, anti-aliased vector bubbles, circular contact avatars, and full multi-color emoji support (`D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT` and `Segoe UI Emoji`).
- **Emoji Menu & Windows `Win + .` Input**: Built-in `RichEdit50W` composer supporting Windows' native emoji panel (`Win + .`) plus a built-in Direct2D emoji picker button.
- **Windows Imaging Component (WIC)**: Native decoding and rendering of MMS image attachments (`PNG`, `JPG`, `GIF`, `WebP`, `BMP`) and magic-byte extension detection for received media/videos.
- **Live Phone Connection Badge**: Green/red smartphone indicator icon in the header with hover device tooltip and instant TLS connection state updates.
- **KDE Connect Network Protocol (v8 TLS)**:
  - UDP `1716` discovery broadcasting & listening (`kdeconnect.identity`).
  - TCP `1716` + Windows Schannel TLS encrypted packet framing.
  - Configurable IPv4 network interface binding (`BindIp`) right from Settings.
- **Seamless Windows Theme & Shell Integration**:
  - Automatic Dark / Light mode via `AppsUseLightTheme` and live `WM_SETTINGCHANGE`.
  - Immersive dark title bar (`DWMWA_USE_IMMERSIVE_DARK_MODE`), Windows 11 visual styles, and custom dark-mode Settings dropdowns.
  - Native Windows Toast notifications (with Quick Reply) and system tray support.
- **Multi-Language Support**: Automatic OS language detection with Settings selector for English, French, Spanish, German, Italian, and Portuguese.

## Project Structure

```text
cpp/
├── CMakeLists.txt              # CMake build configuration
├── build_msvc.bat              # MSVC x64 command-line build script
├── build_cmake.bat             # CMake + Visual Studio 2022 build helper
├── README.md                   # Technical documentation
├── resources/
│   ├── app.manifest            # Per-Monitor V2 DPI & Common-Controls v6
│   └── app.rc                  # Win32 icon & version resource file
├── include/
│   ├── common.h                # Unicode definitions, models & INI config helpers
│   ├── i18n.h                  # Multi-language localization interface
│   ├── theme_manager.h         # Dark/Light DWM & control theming
│   ├── direct2d_renderer.h     # Direct2D, DirectWrite & WIC renderer
│   ├── kdeconnect_protocol.h   # KDE Connect UDP/TCP & Schannel TLS protocol
│   ├── daemon.h                # Local SMS/MMS & contacts engine
│   └── toast_notifier.h        # Windows Toast & system tray integration
└── src/
    ├── i18n.cpp                # Translation table & system language detection
    ├── theme_manager.cpp       # Theme registry query & DWM dark mode
    ├── direct2d_renderer.cpp   # Direct2D rendering implementation
    ├── kdeconnect_protocol.cpp # KDE packet framing, discovery & TLS
    ├── daemon.cpp              # JSON SMS persistence, MMS attachments & vCard cache
    ├── toast_notifier.cpp      # WinRT toast & tray icon implementation
    └── main_ui.cpp             # Win32 GUI entry point, composer & Settings dialog
```

## Build Instructions

### MinGW-w64 (`mingw32-make`)
Run from the repository root:
```cmd
mingw32-make KDEConnectSMS.exe
```

### Visual Studio 2022 (`build_msvc.bat`)
Open **x64 Native Tools Command Prompt for VS 2022**:
```cmd
cd cpp
build_msvc.bat
```

### CMake
```cmd
cd cpp
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
