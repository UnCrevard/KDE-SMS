#pragma once
#include "common.h"
#include <shellapi.h>

namespace KDEConnect {

    constexpr UINT WM_TRAY_ICON = WM_USER + 101;

    class ToastNotifier {
    public:
        static ToastNotifier& Instance();

        void Initialize(HWND appHwnd = nullptr);
        void EnsureTrayIcon();
        void Shutdown();

        HICON GetSmsIcon() const { return m_smsIcon; }

        // Show a rich Windows Notification for incoming SMS
        bool ShowSmsNotification(
            const std::wstring& senderName,
            const std::wstring& messageBody,
            int64_t threadId,
            const std::wstring& avatarPath = L""
        );

    private:
        ToastNotifier() = default;
        ~ToastNotifier() = default;

        bool ShowWinRTToast(const std::wstring& sender, const std::wstring& body, int64_t threadId);
        bool ShowFallbackTrayBalloon(const std::wstring& sender, const std::wstring& body);

        HWND m_hwnd = nullptr;
        HICON m_smsIcon = nullptr;
        NOTIFYICONDATAW m_nid{};
        bool m_nidAdded = false;
        bool m_winRtAvailable = false;
    };

} // namespace KDEConnect
