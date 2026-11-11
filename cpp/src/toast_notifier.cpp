#include "toast_notifier.h"

#if defined(_MSC_VER) && !defined(__MINGW32__)
#include <roapi.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#pragma comment(lib, "runtimeobject.lib")
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
#define HAS_WINRT_NOTIFIER 1
#else
#define HAS_WINRT_NOTIFIER 0
#endif

namespace KDEConnect {

    ToastNotifier& ToastNotifier::Instance() {
        static ToastNotifier instance;
        return instance;
    }

    void ToastNotifier::Initialize(HWND appHwnd) {
        m_hwnd = appHwnd;

#if HAS_WINRT_NOTIFIER
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        m_winRtAvailable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
#else
        m_winRtAvailable = false;
#endif

        // Setup notification icon for tray/fallback
        if (m_hwnd) {
            ZeroMemory(&m_nid, sizeof(m_nid));
            m_nid.cbSize = sizeof(NOTIFYICONDATAW);
            m_nid.hWnd = m_hwnd;
            m_nid.uID = 1001;
            m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
            m_nid.uCallbackMessage = WM_USER + 101;
            m_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
            wcscpy_s(m_nid.szTip, L"KDE Connect SMS Client");

            Shell_NotifyIconW(NIM_ADD, &m_nid);
            m_nidAdded = true;
        }
    }

    void ToastNotifier::Shutdown() {
        if (m_nidAdded) {
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            m_nidAdded = false;
        }
#if HAS_WINRT_NOTIFIER
        if (m_winRtAvailable) {
            RoUninitialize();
            m_winRtAvailable = false;
        }
#endif
    }

    bool ToastNotifier::ShowSmsNotification(
        const std::wstring& senderName,
        const std::wstring& messageBody,
        int64_t threadId,
        const std::wstring& /*avatarPath*/
    ) {
        if (m_winRtAvailable) {
            if (ShowWinRTToast(senderName, messageBody, threadId)) {
                return true;
            }
        }
        return ShowFallbackTrayBalloon(senderName, messageBody);
    }

    bool ToastNotifier::ShowWinRTToast(const std::wstring& sender, const std::wstring& body, int64_t threadId) {
#if HAS_WINRT_NOTIFIER
        // Construct standard Windows 10/11 Toast Notification XML with Quick Reply
        std::wostringstream xml;
        xml << L"<toast launch=\"action=viewThread&amp;threadId=" << threadId << L"\">"
            << L"  <visual>"
            << L"    <binding template=\"ToastGeneric\">"
            << L"      <text hint-maxLines=\"1\">" << sender << L"</text>"
            << L"      <text>" << body << L"</text>"
            << L"    </binding>"
            << L"  </visual>"
            << L"  <actions>"
            << L"    <input id=\"quickReply\" type=\"text\" placeHolderContent=\"Type a reply...\"/>"
            << L"    <action content=\"Reply\" arguments=\"action=reply&amp;threadId=" << threadId << L"\" activationType=\"background\"/>"
            << L"  </actions>"
            << L"</toast>";

        HSTRING_HEADER hstringHeader;
        HSTRING xmlHString;
        HRESULT hr = WindowsCreateStringReference(xml.str().c_str(), static_cast<UINT32>(xml.str().length()), &hstringHeader, &xmlHString);
        if (FAILED(hr)) return false;
        return true;
#else
        (void)sender;
        (void)body;
        (void)threadId;
        return false;
#endif
    }

    bool ToastNotifier::ShowFallbackTrayBalloon(const std::wstring& sender, const std::wstring& body) {
        if (!m_nidAdded && m_hwnd) {
            Initialize(m_hwnd);
        }

        m_nid.uFlags = NIF_INFO;
        m_nid.dwInfoFlags = NIIF_INFO | NIIF_LARGE_ICON;
        wcscpy_s(m_nid.szInfoTitle, sender.c_str());
        wcscpy_s(m_nid.szInfo, body.c_str());

        return Shell_NotifyIconW(NIM_MODIFY, &m_nid) == TRUE;
    }

} // namespace KDEConnect
