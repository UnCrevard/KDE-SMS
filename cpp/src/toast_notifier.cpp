#include "toast_notifier.h"
#include "theme_manager.h"
#include "i18n.h"

#include <cmath>
#include <cwchar>
#include <algorithm>
#include <vector>

namespace KDEConnect {

    namespace {

        constexpr const wchar_t* BUBBLE_CLASS_NAME = L"KDEConnectSmsBubbleWnd";
        constexpr int BUBBLE_WIDTH = 336;
        constexpr int BUBBLE_HEIGHT = 86;
        constexpr int BUBBLE_MARGIN_X = 14;
        constexpr int BUBBLE_MARGIN_BOTTOM = 14;
        constexpr int BUBBLE_GAP_Y = 8;
        constexpr size_t MAX_VISIBLE_BUBBLES = 3;
        constexpr UINT_PTR IDT_BUBBLE_DISMISS = 1;
        constexpr UINT BUBBLE_LIFETIME_MS = 5500;
        constexpr UINT BUBBLE_HOVER_EXTEND_MS = 2000;

        struct BubbleWindowState {
            HWND hwnd = nullptr;
            HWND appHwnd = nullptr;
            std::wstring sender;
            std::wstring body;
            int64_t threadId = 0;
            bool hovered = false;
            bool closeHovered = false;
            bool trackingLeave = false;
        };

        std::vector<BubbleWindowState*> g_activeBubbles;
        bool g_bubbleClassRegistered = false;

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
            bmi.bmiHeader.biHeight = -H;
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

                    float dBox = SdfRoundedBox(px, py, 16.0f, 13.5f, 13.5f, 10.5f, 5.5f);
                    float dTail = SdfTriangle(px, py, 7.0f, 22.0f, 5.0f, 29.0f, 14.5f, 23.0f);
                    float dBubble = std::min(dBox, dTail);

                    float bubbleAlpha = std::clamp(0.5f - dBubble, 0.0f, 1.0f);
                    if (bubbleAlpha <= 0.001f) {
                        pixels[y * W + x] = 0;
                        continue;
                    }

                    float gradT = std::clamp((py - 3.0f) / 24.0f, 0.0f, 1.0f);
                    float bR = 59.0f * (1.0f - gradT) + 29.0f * gradT;
                    float bG = 130.0f * (1.0f - gradT) + 78.0f * gradT;
                    float bB = 246.0f * (1.0f - gradT) + 216.0f * gradT;

                    float dDot1 = std::hypot(px - 10.0f, py - 13.5f) - 2.3f;
                    float dDot2 = std::hypot(px - 16.0f, py - 13.5f) - 2.3f;
                    float dDot3 = std::hypot(px - 22.0f, py - 13.5f) - 2.3f;
                    float dDots = std::min({ dDot1, dDot2, dDot3 });
                    float dotAlpha = std::clamp(0.5f - dDots, 0.0f, 1.0f);

                    float outR = bR * (1.0f - dotAlpha) + 255.0f * dotAlpha;
                    float outG = bG * (1.0f - dotAlpha) + 255.0f * dotAlpha;
                    float outB = bB * (1.0f - dotAlpha) + 255.0f * dotAlpha;

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

        void RepositionActiveBubbles() {
            RECT wa{};
            if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
                wa.left = 0;
                wa.top = 0;
                wa.right = GetSystemMetrics(SM_CXSCREEN);
                wa.bottom = GetSystemMetrics(SM_CYSCREEN);
            }

            int x = static_cast<int>(wa.right) - BUBBLE_WIDTH - BUBBLE_MARGIN_X;
            int count = static_cast<int>(g_activeBubbles.size());
            for (int i = 0; i < count; ++i) {
                BubbleWindowState* st = g_activeBubbles[static_cast<size_t>(i)];
                if (!st || !st->hwnd || !IsWindow(st->hwnd)) continue;
                int slotFromBottom = (count - 1) - i;
                int y = static_cast<int>(wa.bottom) - BUBBLE_MARGIN_BOTTOM - (slotFromBottom + 1) * BUBBLE_HEIGHT - slotFromBottom * BUBBLE_GAP_Y;
                SetWindowPos(st->hwnd, HWND_TOPMOST, x, y, BUBBLE_WIDTH, BUBBLE_HEIGHT,
                             SWP_NOACTIVATE | SWP_NOSIZE);
            }
        }

        LRESULT CALLBACK BubbleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            BubbleWindowState* st = reinterpret_cast<BubbleWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

            switch (msg) {
                case WM_NCCREATE: {
                    CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                    return TRUE;
                }

                case WM_MOUSEACTIVATE:
                    return MA_NOACTIVATE;

                case WM_MOUSEMOVE: {
                    if (st) {
                        if (!st->trackingLeave) {
                            TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                            TrackMouseEvent(&tme);
                            st->trackingLeave = true;
                        }
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        bool newCloseHover = (x >= BUBBLE_WIDTH - 28 && y <= 26);
                        if (!st->hovered || st->closeHovered != newCloseHover) {
                            st->hovered = true;
                            st->closeHovered = newCloseHover;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                    }
                    return 0;
                }

                case WM_MOUSELEAVE: {
                    if (st) {
                        st->trackingLeave = false;
                        st->hovered = false;
                        st->closeHovered = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }

                case WM_TIMER: {
                    if (wParam == IDT_BUBBLE_DISMISS && st) {
                        if (st->hovered) {
                            SetTimer(hwnd, IDT_BUBBLE_DISMISS, BUBBLE_HOVER_EXTEND_MS, nullptr);
                            return 0;
                        }
                        KillTimer(hwnd, IDT_BUBBLE_DISMISS);
                        DestroyWindow(hwnd);
                    }
                    return 0;
                }

                case WM_LBUTTONUP: {
                    if (st) {
                        int x = GET_X_LPARAM(lParam);
                        int y = GET_Y_LPARAM(lParam);
                        bool hitClose = (x >= BUBBLE_WIDTH - 28 && y <= 26);
                        if (!hitClose && st->appHwnd && IsWindow(st->appHwnd)) {
                            PostMessageW(st->appHwnd, WM_OPEN_SMS_THREAD, 0, static_cast<LPARAM>(st->threadId));
                        }
                    }
                    DestroyWindow(hwnd);
                    return 0;
                }

                case WM_RBUTTONUP: {
                    DestroyWindow(hwnd);
                    return 0;
                }

                case WM_ERASEBKGND:
                    return 1;

                case WM_PAINT: {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);

                    HDC memDc = CreateCompatibleDC(hdc);
                    HBITMAP memBmp = CreateCompatibleBitmap(hdc, BUBBLE_WIDTH, BUBBLE_HEIGHT);
                    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

                    bool isDark = ThemeManager::Instance().IsDarkActive();
                    COLORREF bgCol = isDark
                        ? (st && st->hovered ? RGB(33, 45, 66) : RGB(22, 32, 49))
                        : (st && st->hovered ? RGB(243, 248, 255) : RGB(255, 255, 255));
                    COLORREF borderCol = isDark
                        ? (st && st->hovered ? RGB(59, 130, 246) : RGB(56, 72, 96))
                        : (st && st->hovered ? RGB(59, 130, 246) : RGB(203, 213, 225));
                    COLORREF accentCol = RGB(59, 130, 246);
                    COLORREF titleCol = isDark ? RGB(248, 250, 252) : RGB(15, 23, 42);
                    COLORREF bodyCol = isDark ? RGB(196, 210, 228) : RGB(51, 65, 85);
                    COLORREF mutedCol = isDark ? RGB(148, 163, 184) : RGB(100, 116, 139);
                    COLORREF circleCol = isDark ? RGB(37, 52, 78) : RGB(226, 236, 252);

                    HBRUSH hBgBrush = CreateSolidBrush(bgCol);
                    HPEN hBorderPen = CreatePen(PS_SOLID, 1, borderCol);
                    HGDIOBJ oldBrush = SelectObject(memDc, hBgBrush);
                    HGDIOBJ oldPen = SelectObject(memDc, hBorderPen);
                    RoundRect(memDc, 0, 0, BUBBLE_WIDTH, BUBBLE_HEIGHT, 16, 16);
                    SelectObject(memDc, oldPen);
                    SelectObject(memDc, oldBrush);
                    DeleteObject(hBorderPen);
                    DeleteObject(hBgBrush);

                    // Left subtle accent strip inside rounded card
                    HBRUSH hAccentBrush = CreateSolidBrush(accentCol);
                    HPEN hNullPen = CreatePen(PS_NULL, 0, 0);
                    SelectObject(memDc, hAccentBrush);
                    SelectObject(memDc, hNullPen);
                    RoundRect(memDc, 6, 14, 10, BUBBLE_HEIGHT - 14, 4, 4);

                    // Circular badge on left with SMS speech icon
                    HBRUSH hCircleBrush = CreateSolidBrush(circleCol);
                    SelectObject(memDc, hCircleBrush);
                    Ellipse(memDc, 18, 23, 58, 63);
                    SelectObject(memDc, oldBrush);
                    SelectObject(memDc, oldPen);
                    DeleteObject(hCircleBrush);
                    DeleteObject(hNullPen);
                    DeleteObject(hAccentBrush);

                    HICON hIcon = ToastNotifier::Instance().GetSmsIcon();
                    if (hIcon) {
                        DrawIconEx(memDc, 24, 29, hIcon, 28, 28, 0, nullptr, DI_NORMAL);
                    }

                    SetBkMode(memDc, TRANSPARENT);

                    HFONT hTitleFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                    HFONT hBodyFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

                    std::wstring senderText = (st && !st->sender.empty()) ? st->sender : Tr("new_sms_title");
                    std::wstring bodyText = (st && !st->body.empty()) ? st->body : Tr("new_sms_body");

                    HGDIOBJ oldFont = SelectObject(memDc, hTitleFont);
                    SetTextColor(memDc, titleCol);
                    RECT rcTitle{ 68, 12, BUBBLE_WIDTH - 34, 32 };
                    DrawTextW(memDc, senderText.c_str(), -1, &rcTitle,
                              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                    // Close button '×' at top-right
                    SetTextColor(memDc, (st && st->closeHovered) ? titleCol : mutedCol);
                    RECT rcClose{ BUBBLE_WIDTH - 28, 8, BUBBLE_WIDTH - 8, 28 };
                    DrawTextW(memDc, L"\u00D7", 1, &rcClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDc, hBodyFont);
                    SetTextColor(memDc, bodyCol);
                    RECT rcBody{ 68, 35, BUBBLE_WIDTH - 14, BUBBLE_HEIGHT - 12 };
                    DrawTextW(memDc, bodyText.c_str(), -1, &rcBody,
                              DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);

                    SelectObject(memDc, oldFont);
                    DeleteObject(hTitleFont);
                    DeleteObject(hBodyFont);

                    BitBlt(hdc, 0, 0, BUBBLE_WIDTH, BUBBLE_HEIGHT, memDc, 0, 0, SRCCOPY);
                    SelectObject(memDc, oldBmp);
                    DeleteObject(memBmp);
                    DeleteDC(memDc);

                    EndPaint(hwnd, &ps);
                    return 0;
                }

                case WM_DESTROY: {
                    KillTimer(hwnd, IDT_BUBBLE_DISMISS);
                    auto it = std::find(g_activeBubbles.begin(), g_activeBubbles.end(), st);
                    if (it != g_activeBubbles.end()) {
                        g_activeBubbles.erase(it);
                    }
                    delete st;
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    RepositionActiveBubbles();
                    return 0;
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        void SpawnBubbleOnUiThread(HWND appHwnd, const std::wstring& sender, const std::wstring& body, int64_t threadId) {
            if (!appHwnd || !IsWindow(appHwnd)) return;

            HINSTANCE hInst = GetModuleHandleW(nullptr);
            if (!g_bubbleClassRegistered) {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(WNDCLASSEXW);
                wc.style = CS_HREDRAW | CS_VREDRAW;
                wc.lpfnWndProc = BubbleWndProc;
                wc.hInstance = hInst;
                wc.hCursor = LoadCursor(nullptr, IDC_HAND);
                wc.hbrBackground = nullptr;
                wc.lpszClassName = BUBBLE_CLASS_NAME;
                if (RegisterClassExW(&wc)) {
                    g_bubbleClassRegistered = true;
                }
            }
            if (!g_bubbleClassRegistered) return;

            while (g_activeBubbles.size() >= MAX_VISIBLE_BUBBLES) {
                BubbleWindowState* oldest = g_activeBubbles.front();
                if (oldest && oldest->hwnd && IsWindow(oldest->hwnd)) {
                    DestroyWindow(oldest->hwnd);
                } else {
                    g_activeBubbles.erase(g_activeBubbles.begin());
                }
            }

            BubbleWindowState* st = new BubbleWindowState{};
            st->appHwnd = appHwnd;
            st->sender = sender;
            st->body = body;
            st->threadId = threadId;

            HWND hPopup = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                BUBBLE_CLASS_NAME,
                L"",
                WS_POPUP,
                0, 0, BUBBLE_WIDTH, BUBBLE_HEIGHT,
                nullptr, nullptr, hInst, st
            );
            if (!hPopup) {
                delete st;
                return;
            }
            st->hwnd = hPopup;

            HRGN hRgn = CreateRoundRectRgn(0, 0, BUBBLE_WIDTH + 1, BUBBLE_HEIGHT + 1, 16, 16);
            if (hRgn) {
                SetWindowRgn(hPopup, hRgn, FALSE);
            }
            SetLayeredWindowAttributes(hPopup, 0, 244, LWA_ALPHA);

            g_activeBubbles.push_back(st);
            RepositionActiveBubbles();

            ShowWindow(hPopup, SW_SHOWNOACTIVATE);
            UpdateWindow(hPopup);
            SetTimer(hPopup, IDT_BUBBLE_DISMISS, BUBBLE_LIFETIME_MS, nullptr);
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
        std::vector<HWND> toClose;
        for (auto* st : g_activeBubbles) {
            if (st && st->hwnd && IsWindow(st->hwnd)) {
                toClose.push_back(st->hwnd);
            }
        }
        for (HWND h : toClose) {
            DestroyWindow(h);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingBubbles.clear();
        if (m_nidAdded) {
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            m_nidAdded = false;
        }
        if (m_smsIcon) {
            DestroyIcon(m_smsIcon);
            m_smsIcon = nullptr;
        }
    }

    bool ToastNotifier::ShowSmsNotification(
        const std::wstring& senderName,
        const std::wstring& messageBody,
        int64_t threadId,
        const std::wstring& avatarPath
    ) {
        if (ReadIniInt(L"App", L"EnableNotifications", 1) == 0) {
            return false;
        }

        HWND targetHwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_hwnd || !IsWindow(m_hwnd)) {
                return false;
            }
            targetHwnd = m_hwnd;
            PendingBubble b;
            b.sender = senderName.empty() ? L"KDE Connect SMS" : senderName;
            b.body = messageBody.empty() ? Tr("new_sms_body") : messageBody;
            b.threadId = threadId;
            b.avatarPath = avatarPath;
            m_pendingBubbles.push_back(std::move(b));
        }

        return PostMessageW(targetHwnd, WM_SHOW_SMS_BUBBLE, 0, 0) != FALSE;
    }

    void ToastNotifier::ProcessPendingBubbles() {
        std::vector<PendingBubble> batch;
        HWND appHwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            appHwnd = m_hwnd;
            batch.swap(m_pendingBubbles);
        }
        for (const auto& b : batch) {
            SpawnBubbleOnUiThread(appHwnd, b.sender, b.body, b.threadId);
        }
    }

} // namespace KDEConnect
