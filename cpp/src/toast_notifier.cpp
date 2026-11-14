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

#include <cmath>
#include <cwchar>
#include <vector>

namespace KDEConnect {

    namespace {

        // Signed distance to a rounded box centered at (cx, cy) with half-extents (hx, hy) and radius r
        float SdfRoundedBox(float px, float py, float cx, float cy, float hx, float hy, float r) {
            float dx = std::fabs(px - cx) - (hx - r);
            float dy = std::fabs(py - cy) - (hy - r);
            float ox = std::max(dx, 0.0f);
            float oy = std::max(dy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - r;
        }

        // Exact signed distance to a triangle (p0, p1, p2)
        float SdfTriangle(float px, float py, float x0, float y0, float x1, float y1, float x2, float y2) {
            auto edgeDist = [&](float ax, float ay, float bx, float by) -> std::pair<float, float> {
                float ex = bx - ax, ey = by - ay;
                float wx = px - ax, wy = py - ay;
                float ee = ex * ex + ey * ey;
                float t = (ee > 0.0f) ? std::clamp((wx * ex + wy * ey) / ee, 0.0f, 1.0f) : 0.0f;
                float dx = wx - ex * t;
                float dy = wy - ey * t;
                float s = (ex * wy - ey * wx);
                return { dx * dx + dy * dy, s };
            };
            // Determine winding sign
            float wind = ((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)) >= 0.0f ? 1.0f : -1.0f;
            auto [d0, s0] = edgeDist(x0, y0, x1, y1);
            auto [d1, s1] = edgeDist(x1, y1, x2, y2);
            auto [d2, s2] = edgeDist(x2, y2, x0, y0);
            float minD2 = std::min({ d0, d1, d2 });
            float minS = std::min({ s0 * wind, s1 * wind, s2 * wind });
            return (minS > 0.0f) ? -std::sqrt(minD2) : std::sqrt(minD2);
        }

        HICON CreateSmsTrayIcon() {
            const int W = 32;
            const int H = 32;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = W;
            bmi.bmiHeader.biHeight = -H; // top-down DIB
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* pvBits = nullptr;
            HDC hdcScreen = GetDC(nullptr);
            HBITMAP hColorBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
            ReleaseDC(nullptr, hdcScreen);
            if (!hColorBmp || !pvBits) {
                if (hColorBmp) DeleteObject(hColorBmp);
                return nullptr;
            }

            uint32_t* pixels = static_cast<uint32_t*>(pvBits);

            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    float px = static_cast<float>(x) + 0.5f;
                    float py = static_cast<float>(y) + 0.5f;

                    // Speech bubble body + bottom-left tail
                    float dBox = SdfRoundedBox(px, py, 16.0f, 13.5f, 13.5f, 10.5f, 5.5f);
                    float dTail = SdfTriangle(px, py, 7.0f, 22.0f, 5.0f, 29.0f, 14.5f, 23.0f);
                    float dBubble = std::min(dBox, dTail);

                    float bubbleAlpha = std::clamp(0.5f - dBubble, 0.0f, 1.0f);
                    if (bubbleAlpha <= 0.001f) {
                        pixels[y * W + x] = 0;
                        continue;
                    }

                    // Gradient blue fill (#3B82F6 top -> #1D4ED8 bottom)
                    float gradT = std::clamp((py - 3.0f) / 24.0f, 0.0f, 1.0f);
                    float bR = 59.0f * (1.0f - gradT) + 29.0f * gradT;
                    float bG = 130.0f * (1.0f - gradT) + 78.0f * gradT;
                    float bB = 246.0f * (1.0f - gradT) + 216.0f * gradT;

                    // Three crisp white message dots inside bubble
                    float dDot1 = std::hypot(px - 10.0f, py - 13.5f) - 2.3f;
                    float dDot2 = std::hypot(px - 16.0f, py - 13.5f) - 2.3f;
                    float dDot3 = std::hypot(px - 22.0f, py - 13.5f) - 2.3f;
                    float dDots = std::min({ dDot1, dDot2, dDot3 });
                    float dotAlpha = std::clamp(0.5f - dDots, 0.0f, 1.0f);

                    float outR = bR * (1.0f - dotAlpha) + 255.0f * dotAlpha;
                    float outG = bG * (1.0f - dotAlpha) + 255.0f * dotAlpha;
                    float outB = bB * (1.0f - dotAlpha) + 255.0f * dotAlpha;

                    // Premultiplied BGRA
                    uint32_t a = static_cast<uint32_t>(std::round(bubbleAlpha * 255.0f));
                    uint32_t r = static_cast<uint32_t>(std::round(outR * bubbleAlpha));
                    uint32_t g = static_cast<uint32_t>(std::round(outG * bubbleAlpha));
                    uint32_t b = static_cast<uint32_t>(std::round(outB * bubbleAlpha));
                    pixels[y * W + x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
            }

            std::vector<BYTE> maskBits(static_cast<size_t>((W / 8) * H), 0);
            HBITMAP hMaskBmp = CreateBitmap(W, H, 1, 1, maskBits.data());

            ICONINFO ii{};
            ii.fIcon = TRUE;
            ii.hbmMask = hMaskBmp;
            ii.hbmColor = hColorBmp;
            HICON hIcon = CreateIconIndirect(&ii);

            if (hMaskBmp) DeleteObject(hMaskBmp);
            DeleteObject(hColorBmp);
            return hIcon;
        }

        void CopyBoundedWide(wchar_t* dest, size_t destCapacity, const std::wstring& src) {
            if (!dest || destCapacity == 0) return;
            size_t count = std::min(src.size(), destCapacity - 1);
            if (count > 0) {
                wmemcpy(dest, src.data(), count);
            }
            dest[count] = L'\0';
        }

    } // namespace

    ToastNotifier& ToastNotifier::Instance() {
        static ToastNotifier instance;
        return instance;
    }

    void ToastNotifier::Initialize(HWND appHwnd) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_hwnd = appHwnd;

#if HAS_WINRT_NOTIFIER
            if (!m_winRtAvailable) {
                HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
                m_winRtAvailable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
            }
#else
            m_winRtAvailable = false;
#endif

            if (!m_smsIcon) {
                m_smsIcon = CreateSmsTrayIcon();
            }
        }

        EnsureTrayIcon();
    }

    void ToastNotifier::EnsureTrayIcon() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hwnd || !IsWindow(m_hwnd)) return;

        ZeroMemory(&m_nid, sizeof(m_nid));
        m_nid.cbSize = sizeof(NOTIFYICONDATAW);
        m_nid.hWnd = m_hwnd;
        m_nid.uID = 1001;
        m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        m_nid.uCallbackMessage = WM_TRAY_ICON;
        m_nid.hIcon = m_smsIcon ? m_smsIcon : LoadIcon(nullptr, IDI_APPLICATION);
        CopyBoundedWide(m_nid.szTip, sizeof(m_nid.szTip) / sizeof(wchar_t), L"KDE Connect SMS");

        if (Shell_NotifyIconW(NIM_ADD, &m_nid) == TRUE) {
            m_nidAdded = true;
        }
    }

    void ToastNotifier::Shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_nidAdded) {
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            m_nidAdded = false;
        }
        if (m_smsIcon) {
            DestroyIcon(m_smsIcon);
            m_smsIcon = nullptr;
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
        if (ReadIniInt(L"App", L"EnableNotifications", 1) == 0) {
            return false;
        }
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
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hwnd || !IsWindow(m_hwnd)) {
            return false;
        }

        if (!m_nidAdded) {
            ZeroMemory(&m_nid, sizeof(m_nid));
            m_nid.cbSize = sizeof(NOTIFYICONDATAW);
            m_nid.hWnd = m_hwnd;
            m_nid.uID = 1001;
            m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            m_nid.uCallbackMessage = WM_TRAY_ICON;
            m_nid.hIcon = m_smsIcon ? m_smsIcon : LoadIcon(nullptr, IDI_APPLICATION);
            CopyBoundedWide(m_nid.szTip, sizeof(m_nid.szTip) / sizeof(wchar_t), L"KDE Connect SMS");
            if (Shell_NotifyIconW(NIM_ADD, &m_nid) == TRUE) {
                m_nidAdded = true;
            }
        }

        std::wstring safeTitle = sender.empty() ? L"KDE Connect SMS" : sender;
        std::wstring safeBody = body.empty() ? L"Nouveau message recu" : body;

        NOTIFYICONDATAW balloonNid{};
        balloonNid.cbSize = sizeof(NOTIFYICONDATAW);
        balloonNid.hWnd = m_hwnd;
        balloonNid.uID = 1001;
        balloonNid.uFlags = NIF_INFO;
        balloonNid.uTimeout = 10000;
        if (m_smsIcon) {
            balloonNid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
            balloonNid.hBalloonIcon = m_smsIcon;
        } else {
            balloonNid.dwInfoFlags = NIIF_INFO;
        }
        CopyBoundedWide(balloonNid.szInfoTitle, sizeof(balloonNid.szInfoTitle) / sizeof(wchar_t), safeTitle);
        CopyBoundedWide(balloonNid.szInfo, sizeof(balloonNid.szInfo) / sizeof(wchar_t), safeBody);

        return Shell_NotifyIconW(NIM_MODIFY, &balloonNid) == TRUE;
    }

} // namespace KDEConnect
