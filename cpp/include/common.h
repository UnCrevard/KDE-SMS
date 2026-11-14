#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wincodec.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <chrono>
#include <functional>
#include <algorithm>
#include <mutex>
#include <sstream>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

namespace KDEConnect {

    // Theme configuration
    enum class ThemeMode {
        System = 0,
        Light = 1,
        Dark = 2
    };

    struct ThemeColors {
        D2D1_COLOR_F windowBackground;
        D2D1_COLOR_F sidebarBackground;
        D2D1_COLOR_F sidebarHover;
        D2D1_COLOR_F sidebarSelected;
        D2D1_COLOR_F dividerColor;
        D2D1_COLOR_F textPrimary;
        D2D1_COLOR_F textSecondary;
        D2D1_COLOR_F textMuted;
        D2D1_COLOR_F bubbleInbound;
        D2D1_COLOR_F bubbleInboundText;
        D2D1_COLOR_F bubbleInboundBorder;
        D2D1_COLOR_F bubbleOutbound;
        D2D1_COLOR_F bubbleOutboundText;
        D2D1_COLOR_F accent;
        D2D1_COLOR_F inputBackground;
        D2D1_COLOR_F inputBorder;
        COLORREF win32BgColor;
        COLORREF win32TextColor;
    };

    // SMS/MMS Message representation
    struct Attachment {
        std::wstring fileName;
        std::wstring mimeType;
        std::wstring localFilePath;
        uint64_t fileSize = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Message {
        int64_t id = 0;
        int64_t threadId = 0;
        std::wstring address;      // Phone number or contact identifier
        std::wstring body;         // SMS text content (supports UTF-16 Unicode & Emojis)
        int64_t timestamp = 0;     // Epoch milliseconds
        bool isOutgoing = false;   // True if sent from PC or user's phone, false if received
        bool isRead = true;
        std::vector<Attachment> attachments;

        std::wstring GetFormattedTime() const;
    };

    // Thread/Conversation summary
    struct Conversation {
        int64_t threadId = 0;
        std::wstring address;
        std::wstring contactName;
        std::wstring snippet;
        int64_t lastTimestamp = 0;
        int32_t unreadCount = 0;
        std::vector<Message> messages;
    };

    // Device status for KDE Connect link
    struct DeviceInfo {
        std::wstring deviceId;
        std::wstring deviceName;
        std::wstring deviceType;
        int batteryLevel = -1;
        bool isCharging = false;
        bool isPaired = false;
        bool isReachable = false;
        std::wstring ipAddress;
        uint16_t port = 1716;
    };

    // Named Pipe IPC definitions
    constexpr const wchar_t* IPC_PIPE_NAME = L"\\\\.\\pipe\\kdeconnect_sms_ipc";
    constexpr const wchar_t* APP_SUBKEY = L"Software\\KDEConnectWin32";
    constexpr const wchar_t* APPDATA_FOLDER = L"KDEConnectSMS";
    constexpr const wchar_t* SETTINGS_INI_FILENAME = L"KDEConnectSMS.ini";

    // Portable settings (.ini in current directory) & storage path helpers
    std::wstring GetSettingsIniPath();
    std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue = L"");
    void WriteIniString(const wchar_t* section, const wchar_t* key, const wchar_t* value);
    void DeleteIniKey(const wchar_t* section, const wchar_t* key);
    std::vector<std::pair<std::wstring, std::wstring>> ReadIniSectionPairs(const wchar_t* section);
    int ReadIniInt(const wchar_t* section, const wchar_t* key, int defaultValue);
    void WriteIniInt(const wchar_t* section, const wchar_t* key, int value);

    bool HasConfiguredConversationsDirectory();
    std::wstring GetDefaultConversationsDirectory();
    void SetConversationsDirectory(const std::wstring& dirPath);

    std::wstring GetDefaultReceivedFilesDirectory();
    std::wstring GetReceivedFilesDirectory();
    void SetReceivedFilesDirectory(const std::wstring& dirPath);

    std::wstring GetDefaultDeviceName();
    std::wstring GetConfiguredDeviceName();
    void SetConfiguredDeviceName(const std::wstring& deviceName);

    std::wstring GetAppDataDirectory();
    std::wstring GetMessagesDirectory();
    std::wstring Utf8ToWide(const std::string& utf8);
    std::string WideToUtf8(const std::wstring& wide);

} // namespace KDEConnect
