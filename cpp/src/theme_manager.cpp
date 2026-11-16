#include "theme_manager.h"
#include <cstring>
#include <iomanip>
#include <sstream>

namespace KDEConnect {

    namespace {

        std::wstring GetExecutableDirectory() {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                DWORD copied = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
                if (copied == 0) return L"";
                if (copied < buf.size() - 1) {
                    buf.resize(copied);
                    break;
                }
                if (buf.size() >= 32768) {
                    buf.resize(copied);
                    break;
                }
                buf.resize(buf.size() * 2, L'\0');
            }
            size_t slash = buf.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return L"";
            if (slash == 2 && buf[1] == L':') {
                return buf.substr(0, 3);
            }
            return buf.substr(0, slash);
        }

        bool FileExistsWin32(const std::wstring& path) {
            if (path.empty()) return false;
            DWORD attrs = GetFileAttributesW(path.c_str());
            return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
        }

        std::wstring ResolveInitialCurrentDirectory() {
            static std::wstring cachedDir;
            static std::once_flag once;
            std::call_once(once, []() {
                std::wstring cwd;
                DWORD len = GetCurrentDirectoryW(0, nullptr);
                if (len > 0) {
                    std::wstring buf(len, L'\0');
                    DWORD copied = GetCurrentDirectoryW(len, &buf[0]);
                    if (copied > 0) {
                        buf.resize(copied);
                        while (buf.size() > 3 && (buf.back() == L'\\' || buf.back() == L'/')) {
                            buf.pop_back();
                        }
                        cwd = buf;
                    }
                }

                std::wstring exeDir = GetExecutableDirectory();
                std::wstring cwdIni = cwd.empty() ? L"" : (cwd + L"\\" + SETTINGS_INI_FILENAME);
                std::wstring exeIni = exeDir.empty() ? L"" : (exeDir + L"\\" + SETTINGS_INI_FILENAME);

                // Prefer an already-existing INI (exeDir first if both exist, or cwd if only cwd exists),
                // and default new installs to exeDir so KDEConnectSMS.exe and KDEConnectDaemon.exe always share one INI.
                if (FileExistsWin32(exeIni)) {
                    cachedDir = exeDir;
                } else if (FileExistsWin32(cwdIni)) {
                    cachedDir = cwd;
                } else if (!exeDir.empty()) {
                    cachedDir = exeDir;
                } else if (!cwd.empty()) {
                    cachedDir = cwd;
                } else {
                    cachedDir = L".";
                }
            });
            return cachedDir;
        }

        void ClearReadOnlyAttribute(const std::wstring& path) {
            DWORD attrs = GetFileAttributesW(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
                SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            }
        }

        void EnsureIniFileReadyForWin32(const std::wstring& iniPath) {
            DWORD attrs = GetFileAttributesW(iniPath.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                HANDLE hFile = CreateFileW(
                    iniPath.c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ,
                    nullptr,
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr
                );
                if (hFile != INVALID_HANDLE_VALUE) {
                    const unsigned char bom[2] = { 0xFF, 0xFE };
                    DWORD written = 0;
                    WriteFile(hFile, bom, sizeof(bom), &written, nullptr);
                    CloseHandle(hFile);
                }
                return;
            }

            // Convert UTF-8 BOM (EF BB BF) or bare UTF-8 INI files to UTF-16LE so GetPrivateProfileStringW never fails
            HANDLE hRead = CreateFileW(
                iniPath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (hRead == INVALID_HANDLE_VALUE) return;

            LARGE_INTEGER fileSize{};
            if (!GetFileSizeEx(hRead, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 1024 * 1024) {
                CloseHandle(hRead);
                return;
            }

            std::string raw(static_cast<size_t>(fileSize.QuadPart), '\0');
            DWORD bytesRead = 0;
            BOOL readOk = ReadFile(hRead, &raw[0], static_cast<DWORD>(raw.size()), &bytesRead, nullptr);
            CloseHandle(hRead);
            if (!readOk || bytesRead < 3) return;
            raw.resize(bytesRead);

            const unsigned char* u = reinterpret_cast<const unsigned char*>(raw.data());
            if (u[0] == 0xFF && u[1] == 0xFE) {
                return; // Already UTF-16LE BOM
            }
            bool hasUtf8Bom = (bytesRead >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF);
            if (!hasUtf8Bom) {
                return; // Plain ANSI/ASCII INI is already handled natively by Win32 INI APIs
            }

            std::string utf8Content = raw.substr(3);
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Content.data(), static_cast<int>(utf8Content.size()), nullptr, 0);
            if (wideLen < 0) return;
            std::wstring wideContent(static_cast<size_t>(wideLen), L'\0');
            if (wideLen > 0) {
                MultiByteToWideChar(CP_UTF8, 0, utf8Content.data(), static_cast<int>(utf8Content.size()), &wideContent[0], wideLen);
            }

            ClearReadOnlyAttribute(iniPath);
            HANDLE hWrite = CreateFileW(
                iniPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (hWrite != INVALID_HANDLE_VALUE) {
                const unsigned char bom[2] = { 0xFF, 0xFE };
                DWORD written = 0;
                WriteFile(hWrite, bom, sizeof(bom), &written, nullptr);
                if (!wideContent.empty()) {
                    WriteFile(hWrite, wideContent.data(), static_cast<DWORD>(wideContent.size() * sizeof(wchar_t)), &written, nullptr);
                }
                CloseHandle(hWrite);
                WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
            }
        }

        void EnsureDirectoryExistsRecursive(const std::wstring& dirPath) {
            if (dirPath.empty()) return;
            SHCreateDirectoryExW(nullptr, dirPath.c_str(), nullptr);
        }

    } // namespace

    std::wstring GetAppDataDirectory() {
        return ResolveInitialCurrentDirectory();
    }

    std::wstring GetSettingsIniPath() {
        return GetAppDataDirectory() + L"\\" + std::wstring(SETTINGS_INI_FILENAME);
    }

    std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue) {
        std::wstring iniPath = GetSettingsIniPath();
        if (FileExistsWin32(iniPath)) {
            EnsureIniFileReadyForWin32(iniPath);
        }
        wchar_t buffer[2048]{};
        DWORD count = GetPrivateProfileStringW(
            section,
            key,
            defaultValue ? defaultValue : L"",
            buffer,
            static_cast<DWORD>(sizeof(buffer) / sizeof(wchar_t)),
            iniPath.c_str()
        );
        return std::wstring(buffer, count);
    }

    bool WriteIniString(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
        std::wstring iniPath = GetSettingsIniPath();
        EnsureDirectoryExistsRecursive(GetAppDataDirectory());
        EnsureIniFileReadyForWin32(iniPath);
        ClearReadOnlyAttribute(iniPath);
        BOOL ok = WritePrivateProfileStringW(section, key, value ? value : L"", iniPath.c_str());
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
        return ok != FALSE;
    }

    void DeleteIniKey(const wchar_t* section, const wchar_t* key) {
        if (!section || !key) return;
        std::wstring iniPath = GetSettingsIniPath();
        if (!FileExistsWin32(iniPath)) return;
        EnsureIniFileReadyForWin32(iniPath);
        ClearReadOnlyAttribute(iniPath);
        WritePrivateProfileStringW(section, key, nullptr, iniPath.c_str());
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
    }

    std::vector<std::pair<std::wstring, std::wstring>> ReadIniSectionPairs(const wchar_t* section) {
        std::vector<std::pair<std::wstring, std::wstring>> pairs;
        if (!section) return pairs;
        std::wstring iniPath = GetSettingsIniPath();
        if (FileExistsWin32(iniPath)) {
            EnsureIniFileReadyForWin32(iniPath);
        }
        std::vector<wchar_t> buffer(16384, L'\0');
        DWORD copied = GetPrivateProfileSectionW(
            section,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            iniPath.c_str()
        );
        if (copied == 0) return pairs;

        const wchar_t* ptr = buffer.data();
        while (*ptr != L'\0') {
            std::wstring entry(ptr);
            size_t eq = entry.find(L'=');
            if (eq != std::wstring::npos && eq > 0) {
                pairs.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
            }
            ptr += entry.size() + 1;
        }
        return pairs;
    }

    int ReadIniInt(const wchar_t* section, const wchar_t* key, int defaultValue) {
        std::wstring iniPath = GetSettingsIniPath();
        if (FileExistsWin32(iniPath)) {
            EnsureIniFileReadyForWin32(iniPath);
        }
        return static_cast<int>(GetPrivateProfileIntW(section, key, defaultValue, iniPath.c_str()));
    }

    void WriteIniInt(const wchar_t* section, const wchar_t* key, int value) {
        std::wstring valStr = std::to_wstring(value);
        WriteIniString(section, key, valStr.c_str());
    }

    bool HasConfiguredConversationsDirectory() {
        std::wstring configured = ReadIniString(L"Storage", L"ConversationsDir", L"");
        return !configured.empty();
    }

    std::wstring GetDefaultConversationsDirectory() {
        return GetAppDataDirectory() + L"\\Conversations";
    }

    void SetConversationsDirectory(const std::wstring& dirPath) {
        std::wstring clean = dirPath;
        while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
            clean.pop_back();
        }
        if (clean.empty()) {
            clean = GetDefaultConversationsDirectory();
        }
        EnsureDirectoryExistsRecursive(clean);
        WriteIniString(L"Storage", L"ConversationsDir", clean.c_str());
        WriteIniInt(L"Storage", L"Initialized", 1);
    }

    std::wstring GetMessagesDirectory() {
        std::wstring path = ReadIniString(L"Storage", L"ConversationsDir", L"");
        if (path.empty()) {
            path = GetDefaultConversationsDirectory();
        }
        EnsureDirectoryExistsRecursive(path);
        return path;
    }

    std::wstring GetDefaultReceivedFilesDirectory() {
        return GetAppDataDirectory() + L"\\FichiersRecus";
    }

    std::wstring GetReceivedFilesDirectory() {
        std::wstring path = ReadIniString(L"Storage", L"ReceivedFilesDir", L"");
        if (path.empty()) {
            path = GetDefaultReceivedFilesDirectory();
        }
        EnsureDirectoryExistsRecursive(path);
        return path;
    }

    void SetReceivedFilesDirectory(const std::wstring& dirPath) {
        std::wstring clean = dirPath;
        while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
            clean.pop_back();
        }
        if (clean.empty()) {
            clean = GetDefaultReceivedFilesDirectory();
        }
        EnsureDirectoryExistsRecursive(clean);
        WriteIniString(L"Storage", L"ReceivedFilesDir", clean.c_str());
    }

    std::wstring GetDefaultDeviceName() {
        wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        if (!GetComputerNameW(computerName, &size) || wcslen(computerName) == 0) {
            return L"WindowsPC";
        }
        return std::wstring(computerName);
    }

    std::wstring GetConfiguredDeviceName() {
        std::wstring configured = ReadIniString(L"Network", L"DeviceName", L"");
        while (!configured.empty() && (configured.front() == L' ' || configured.front() == L'\t')) {
            configured.erase(configured.begin());
        }
        while (!configured.empty() && (configured.back() == L' ' || configured.back() == L'\t')) {
            configured.pop_back();
        }
        return configured.empty() ? GetDefaultDeviceName() : configured;
    }

    void SetConfiguredDeviceName(const std::wstring& deviceName) {
        std::wstring clean = deviceName;
        while (!clean.empty() && (clean.front() == L' ' || clean.front() == L'\t')) {
            clean.erase(clean.begin());
        }
        while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'\t')) {
            clean.pop_back();
        }
        WriteIniString(L"Network", L"DeviceName", clean.c_str());
    }

    std::wstring Utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) return L"";
        int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(sizeNeeded, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], sizeNeeded);
        return wide;
    }

    std::string WideToUtf8(const std::wstring& wide) {
        if (wide.empty()) return "";
        int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(sizeNeeded, 0);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &utf8[0], sizeNeeded, nullptr, nullptr);
        return utf8;
    }

    std::wstring Message::GetFormattedTime() const {
        if (timestamp <= 0) return L"";
        time_t rawTime = static_cast<time_t>(timestamp / 1000);
        struct tm* tmInfo = std::localtime(&rawTime);
        if (!tmInfo) return L"";
        wchar_t buffer[64];
        wcsftime(buffer, sizeof(buffer)/sizeof(wchar_t), L"%H:%M", tmInfo);
        return buffer;
    }

    ThemeManager& ThemeManager::Instance() {
        static ThemeManager instance;
        return instance;
    }

    ThemeManager::ThemeManager() {
        UpdateColors();
    }

    void ThemeManager::Initialize(HWND mainHwnd) {
        m_mainHwnd = mainHwnd;
        int savedMode = ReadIniInt(L"Appearance", L"ThemeMode", static_cast<int>(ThemeMode::System));
        if (savedMode >= static_cast<int>(ThemeMode::System) && savedMode <= static_cast<int>(ThemeMode::Dark)) {
            m_currentMode = static_cast<ThemeMode>(savedMode);
        }
        UpdateColors();
        ApplyToWindow(m_mainHwnd);
    }

    void ThemeManager::SetMode(ThemeMode mode) {
        m_currentMode = mode;
        WriteIniInt(L"Appearance", L"ThemeMode", static_cast<int>(mode));
        UpdateColors();
        if (m_mainHwnd) {
            ApplyToWindow(m_mainHwnd);
        }
        NotifyCallbacks();
    }

    bool ThemeManager::IsDarkActive() const {
        return m_isDarkActive;
    }

    const ThemeColors& ThemeManager::GetColors() const {
        return m_colors;
    }

    bool ThemeManager::QuerySystemUsesLightTheme() const {
        DWORD value = 1; // Default to light if query fails
        DWORD size = sizeof(value);
        LSTATUS status = RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size
        );
        return (status == ERROR_SUCCESS) && (value != 0);
    }

    void ThemeManager::UpdateColors() {
        if (m_currentMode == ThemeMode::System) {
            m_isDarkActive = !QuerySystemUsesLightTheme();
        } else {
            m_isDarkActive = (m_currentMode == ThemeMode::Dark);
        }

        if (m_isDarkActive) {
            // Dark Theme Palette
            m_colors.windowBackground  = D2D1::ColorF(0.06f, 0.09f, 0.16f, 1.0f); // #0F172A
            m_colors.sidebarBackground = D2D1::ColorF(0.12f, 0.16f, 0.23f, 1.0f); // #1E293B
            m_colors.sidebarHover      = D2D1::ColorF(0.20f, 0.25f, 0.35f, 1.0f); // #334155
            m_colors.sidebarSelected   = D2D1::ColorF(0.15f, 0.23f, 0.40f, 1.0f); // #1E3A8A / Dark Blue Accent
            m_colors.dividerColor      = D2D1::ColorF(0.20f, 0.25f, 0.35f, 1.0f); // #334155
            m_colors.textPrimary       = D2D1::ColorF(0.97f, 0.98f, 0.99f, 1.0f); // #F8FAFC
            m_colors.textSecondary     = D2D1::ColorF(0.58f, 0.64f, 0.72f, 1.0f); // #94A3B8
            m_colors.textMuted         = D2D1::ColorF(0.39f, 0.45f, 0.55f, 1.0f); // #64748B
            m_colors.bubbleInbound     = D2D1::ColorF(0.20f, 0.25f, 0.33f, 1.0f); // #334155
            m_colors.bubbleInboundText = D2D1::ColorF(0.97f, 0.98f, 0.99f, 1.0f); // #F8FAFC
            m_colors.bubbleInboundBorder = D2D1::ColorF(0.28f, 0.33f, 0.43f, 1.0f);
            m_colors.bubbleOutbound    = D2D1::ColorF(0.15f, 0.39f, 0.92f, 1.0f); // #2563EB Royal Blue
            m_colors.bubbleOutboundText = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            m_colors.accent            = D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f); // #3B82F6
            m_colors.inputBackground   = D2D1::ColorF(0.12f, 0.16f, 0.23f, 1.0f);
            m_colors.inputBorder       = D2D1::ColorF(0.28f, 0.33f, 0.43f, 1.0f);
            m_colors.win32BgColor      = RGB(15, 23, 42);
            m_colors.win32TextColor    = RGB(248, 250, 252);
        } else {
            // Light Theme Palette
            m_colors.windowBackground  = D2D1::ColorF(0.97f, 0.98f, 0.99f, 1.0f); // #F8FAFC
            m_colors.sidebarBackground = D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.0f); // #F1F5F9
            m_colors.sidebarHover      = D2D1::ColorF(0.89f, 0.91f, 0.94f, 1.0f); // #E2E8F0
            m_colors.sidebarSelected   = D2D1::ColorF(0.85f, 0.91f, 0.99f, 1.0f); // Soft blue selection
            m_colors.dividerColor      = D2D1::ColorF(0.89f, 0.91f, 0.94f, 1.0f); // #E2E8F0
            m_colors.textPrimary       = D2D1::ColorF(0.06f, 0.09f, 0.16f, 1.0f); // #0F172A
            m_colors.textSecondary     = D2D1::ColorF(0.39f, 0.45f, 0.55f, 1.0f); // #64748B
            m_colors.textMuted         = D2D1::ColorF(0.58f, 0.64f, 0.72f, 1.0f); // #94A3B8
            m_colors.bubbleInbound     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);     // White card
            m_colors.bubbleInboundText = D2D1::ColorF(0.06f, 0.09f, 0.16f, 1.0f); // Dark text
            m_colors.bubbleInboundBorder = D2D1::ColorF(0.89f, 0.91f, 0.94f, 1.0f); // Subtle border
            m_colors.bubbleOutbound    = D2D1::ColorF(0.15f, 0.39f, 0.92f, 1.0f); // #2563EB
            m_colors.bubbleOutboundText = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            m_colors.accent            = D2D1::ColorF(0.15f, 0.39f, 0.92f, 1.0f);
            m_colors.inputBackground   = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            m_colors.inputBorder       = D2D1::ColorF(0.80f, 0.83f, 0.88f, 1.0f);
            m_colors.win32BgColor      = RGB(248, 250, 252);
            m_colors.win32TextColor    = RGB(15, 23, 42);
        }
    }

    namespace {

        enum class PreferredAppModeWin {
            Default = 0,
            AllowDark = 1,
            ForceDark = 2,
            ForceLight = 3,
            Max = 4
        };

        using FnSetWindowTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        using FnAllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
        using FnSetPreferredAppMode = PreferredAppModeWin(WINAPI*)(PreferredAppModeWin);
        using FnFlushMenuThemes = void(WINAPI*)();
        using FnRefreshImmersiveColorPolicyState = void(WINAPI*)();

        struct UxThemeApi {
            HMODULE hMod = nullptr;
            FnSetWindowTheme setWindowTheme = nullptr;
            FnAllowDarkModeForWindow allowDarkModeForWindow = nullptr;
            FnSetPreferredAppMode setPreferredAppMode = nullptr;
            FnFlushMenuThemes flushMenuThemes = nullptr;
            FnRefreshImmersiveColorPolicyState refreshImmersiveColorPolicyState = nullptr;
        };

        template<typename Fn>
        Fn LoadProcSafe(HMODULE mod, const char* nameOrOrd) {
            if (!mod) return nullptr;
            FARPROC p = GetProcAddress(mod, nameOrOrd);
            if (!p) return nullptr;
            Fn fn = nullptr;
            static_assert(sizeof(fn) == sizeof(p), "Function pointer size mismatch");
            memcpy(&fn, &p, sizeof(fn));
            return fn;
        }

        const UxThemeApi& GetUxThemeApi() {
            static UxThemeApi api;
            static std::once_flag once;
            std::call_once(once, []() {
                api.hMod = LoadLibraryW(L"uxtheme.dll");
                if (!api.hMod) return;
                api.setWindowTheme = LoadProcSafe<FnSetWindowTheme>(api.hMod, "SetWindowTheme");
                api.refreshImmersiveColorPolicyState = LoadProcSafe<FnRefreshImmersiveColorPolicyState>(api.hMod, MAKEINTRESOURCEA(104));
                api.allowDarkModeForWindow = LoadProcSafe<FnAllowDarkModeForWindow>(api.hMod, MAKEINTRESOURCEA(133));
                api.setPreferredAppMode = LoadProcSafe<FnSetPreferredAppMode>(api.hMod, MAKEINTRESOURCEA(135));
                api.flushMenuThemes = LoadProcSafe<FnFlushMenuThemes>(api.hMod, MAKEINTRESOURCEA(136));
                if (api.setPreferredAppMode) {
                    api.setPreferredAppMode(PreferredAppModeWin::AllowDark);
                }
                if (api.refreshImmersiveColorPolicyState) {
                    api.refreshImmersiveColorPolicyState();
                }
            });
            return api;
        }

    } // namespace

    void ThemeManager::ApplyToControl(HWND hCtrl) {
        if (!hCtrl || !IsWindow(hCtrl)) return;

        const UxThemeApi& api = GetUxThemeApi();
        if (api.allowDarkModeForWindow) {
            api.allowDarkModeForWindow(hCtrl, m_isDarkActive);
        }

        wchar_t cls[64]{};
        GetClassNameW(hCtrl, cls, 64);

        if (api.setWindowTheme) {
            if (_wcsicmp(cls, L"ComboBox") == 0) {
                api.setWindowTheme(hCtrl, m_isDarkActive ? L"DarkMode_CFD" : L"Explorer", nullptr);
                COMBOBOXINFO cbi{};
                cbi.cbSize = sizeof(cbi);
                if (GetComboBoxInfo(hCtrl, &cbi)) {
                    if (cbi.hwndList) {
                        if (api.allowDarkModeForWindow) {
                            api.allowDarkModeForWindow(cbi.hwndList, m_isDarkActive);
                        }
                        api.setWindowTheme(cbi.hwndList, m_isDarkActive ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                    }
                    if (cbi.hwndItem) {
                        if (api.allowDarkModeForWindow) {
                            api.allowDarkModeForWindow(cbi.hwndItem, m_isDarkActive);
                        }
                        api.setWindowTheme(cbi.hwndItem, m_isDarkActive ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                    }
                }
            } else if (_wcsicmp(cls, L"Edit") == 0) {
                api.setWindowTheme(hCtrl, m_isDarkActive ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            } else if (_wcsicmp(cls, L"Button") == 0) {
                LONG_PTR style = GetWindowLongPtrW(hCtrl, GWL_STYLE);
                DWORD btnType = static_cast<DWORD>(style & BS_TYPEMASK);
                if (btnType == BS_GROUPBOX) {
                    api.setWindowTheme(hCtrl, m_isDarkActive ? L"" : L"Explorer", m_isDarkActive ? L"" : nullptr);
                } else {
                    api.setWindowTheme(hCtrl, m_isDarkActive ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                }
            } else {
                api.setWindowTheme(hCtrl, m_isDarkActive ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            }
        }

        SendMessageW(hCtrl, WM_THEMECHANGED, 0, 0);
        InvalidateRect(hCtrl, nullptr, TRUE);
    }

    void ThemeManager::ApplyThemeToChildren(HWND parentHwnd) {
        if (!parentHwnd || !IsWindow(parentHwnd)) return;
        EnumChildWindows(parentHwnd, [](HWND hChild, LPARAM lParam) -> BOOL {
            auto* mgr = reinterpret_cast<ThemeManager*>(lParam);
            if (mgr) mgr->ApplyToControl(hChild);
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
        RedrawWindow(parentHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    void ThemeManager::ApplyToWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;

        const UxThemeApi& api = GetUxThemeApi();
        if (api.setPreferredAppMode) {
            api.setPreferredAppMode(m_isDarkActive ? PreferredAppModeWin::ForceDark : PreferredAppModeWin::ForceLight);
        }
        if (api.allowDarkModeForWindow) {
            api.allowDarkModeForWindow(hwnd, m_isDarkActive);
        }
        if (api.flushMenuThemes) {
            api.flushMenuThemes();
        }

        // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 (Windows 10 20H1+ and Windows 11), 19 on older Win10
        BOOL useDarkMode = m_isDarkActive ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &useDarkMode, sizeof(useDarkMode));
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));

        // DWMWA_WINDOW_CORNER_PREFERENCE (33): DWMWCP_ROUND (2) for modern Windows 11 rounded corners
        DWORD cornerPref = 2;
        DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));

        ApplyThemeToChildren(hwnd);

        // Redraw frame to force DWM titlebar repaint
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void ThemeManager::HandleSettingChange(HWND hwnd, LPARAM lParam) {
        if (m_currentMode != ThemeMode::System) return;

        if (lParam != 0) {
            const wchar_t* paramStr = reinterpret_cast<const wchar_t*>(lParam);
            if (wcscmp(paramStr, L"ImmersiveColorSet") == 0 || wcscmp(paramStr, L"WindowsThemeElement") == 0) {
                UpdateColors();
                ApplyToWindow(hwnd);
                NotifyCallbacks();
            }
        }
    }

    void ThemeManager::RegisterCallback(const std::string& key, ThemeChangedCallback callback) {
        m_callbacks.emplace_back(key, callback);
    }

    void ThemeManager::UnregisterCallback(const std::string& key) {
        m_callbacks.erase(
            std::remove_if(m_callbacks.begin(), m_callbacks.end(),
                [&](const auto& pair) { return pair.first == key; }),
            m_callbacks.end()
        );
    }

    void ThemeManager::NotifyCallbacks() {
        for (const auto& [key, cb] : m_callbacks) {
            if (cb) cb(m_isDarkActive, m_colors);
        }
    }

} // namespace KDEConnect
