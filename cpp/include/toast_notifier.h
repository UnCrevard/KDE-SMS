#pragma once
#include "common.h"
#include <shellapi.h>
#include <vector>

namespace KDEConnect {

    constexpr UINT WM_TRAY_ICON = WM_USER + 101;
    constexpr UINT WM_SHOW_SMS_BUBBLE = WM_USER + 102;
    constexpr UINT WM_OPEN_SMS_THREAD = WM_USER + 103;

    class ToastNotifier {
    public:
        static ToastNotifier& Instance();

        void Initialize(HWND appHwnd = nullptr);
        void EnsureTrayIcon();
        void Shutdown();

        HICON GetSmsIcon() const { return m_smsIcon; }
        bool HasTrayWindow() const { return m_hwnd != nullptr && IsWindow(m_hwnd) != FALSE; }

        // Show a non-intrusive floating bubble notification (never enters Windows Notification Center)
        bool ShowSmsNotification(
            const std::wstring& senderName,
            const std::wstring& messageBody,
            int64_t threadId,
            const std::wstring& avatarPath = L""
        );

        // Must be called on the UI thread upon receiving WM_SHOW_SMS_BUBBLE
        void ProcessPendingBubbles();

    private:
        struct PendingBubble {
            std::wstring sender;
            std::wstring body;
            int64_t threadId = 0;
            std::wstring avatarPath;
        };

        ToastNotifier() = default;
        ~ToastNotifier() = default;

        mutable std::mutex m_mutex;
        HWND m_hwnd = nullptr;
        HICON m_smsIcon = nullptr;
        NOTIFYICONDATAW m_nid{};
        bool m_nidAdded = false;
        std::vector<PendingBubble> m_pendingBubbles;
    };

} // namespace KDEConnect
