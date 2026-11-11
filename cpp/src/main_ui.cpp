#include "common.h"
#include "theme_manager.h"
#include "direct2d_renderer.h"
#include "toast_notifier.h"
#include "daemon.h"

using namespace KDEConnect;

// Global state for Win32 UI window
static const wchar_t* WINDOW_CLASS_NAME = L"KDEConnectSMSWin32";
static Direct2DRenderer g_renderer;
static std::vector<Conversation> g_conversations;
static int64_t g_selectedThreadId = 101;
static int g_hoveredSidebarIndex = -1;
static float g_sidebarWidth = 320.0f;
static float g_sidebarScroll = 0.0f;
static float g_threadScroll = 0.0f;
static std::vector<BubbleLayout> g_currentBubbleLayouts;

// Child controls
static HWND g_hwndMain = nullptr;
static HWND g_hwndSearchEdit = nullptr;
static HWND g_hwndComposerEdit = nullptr;
static HWND g_hwndSendButton = nullptr;
static HWND g_hwndThemeButton = nullptr;
static HWND g_hwndAttachButton = nullptr;
static HFONT g_hFont = nullptr;

#define IDC_SEARCH_EDIT   1001
#define IDC_COMPOSER_EDIT 1002
#define IDC_SEND_BUTTON   1003
#define IDC_THEME_BUTTON  1004
#define IDC_ATTACH_BUTTON 1005

void LayoutChildControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    // Search box in sidebar top
    if (g_hwndSearchEdit) {
        SetWindowPos(g_hwndSearchEdit, nullptr, 16, 14, static_cast<int>(g_sidebarWidth) - 32, 28, SWP_NOZORDER);
    }

    // Composer bar at bottom of thread view
    int composerHeight = 52;
    int composerY = height - composerHeight;
    int threadWidth = width - static_cast<int>(g_sidebarWidth);

    if (g_hwndAttachButton) {
        SetWindowPos(g_hwndAttachButton, nullptr, static_cast<int>(g_sidebarWidth) + 16, composerY + 8, 36, 36, SWP_NOZORDER);
    }

    if (g_hwndComposerEdit) {
        int editX = static_cast<int>(g_sidebarWidth) + 60;
        int editWidth = threadWidth - 160;
        SetWindowPos(g_hwndComposerEdit, nullptr, editX, composerY + 10, editWidth, 32, SWP_NOZORDER);
    }

    if (g_hwndSendButton) {
        int sendX = width - 88;
        SetWindowPos(g_hwndSendButton, nullptr, sendX, composerY + 8, 72, 36, SWP_NOZORDER);
    }

    if (g_hwndThemeButton) {
        SetWindowPos(g_hwndThemeButton, nullptr, width - 42, 10, 32, 32, SWP_NOZORDER);
    }
}

void OnSendMessage(HWND hwnd) {
    int length = GetWindowTextLengthW(g_hwndComposerEdit);
    if (length <= 0) return;

    std::wstring text(length + 1, 0);
    GetWindowTextW(g_hwndComposerEdit, &text[0], length + 1);
    text.resize(length);

    // Find active conversation address
    std::wstring targetAddress = L"";
    for (const auto& conv : g_conversations) {
        if (conv.threadId == g_selectedThreadId) {
            targetAddress = conv.address;
            break;
        }
    }

    if (targetAddress.empty()) {
        targetAddress = L"+1 (555) 019-2834";
    }

    // Send via daemon
    SmsDaemon::Instance().SendOutgoingSms(targetAddress, text);

    // Clear composer edit box
    SetWindowTextW(g_hwndComposerEdit, L"");

    // Reload conversation and trigger repaint
    g_conversations = SmsDaemon::Instance().LoadAllConversations();
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hwndMain = hwnd;

            // Initialize Theme Manager & detect dark/light theme
            ThemeManager::Instance().Initialize(hwnd);

            // Initialize Direct2D & DirectWrite engine
            g_renderer.Initialize(hwnd);

            // Initialize Windows Toast Notifier
            ToastNotifier::Instance().Initialize(hwnd);

            // Start background sync daemon
            SmsDaemon::Instance().Start();
            g_conversations = SmsDaemon::Instance().LoadAllConversations();

            // Create UI font
            g_hFont = CreateFontW(
                -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
            );

            // Child controls
            g_hwndSearchEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                16, 14, 280, 28, hwnd, (HMENU)IDC_SEARCH_EDIT, GetModuleHandle(nullptr), nullptr
            );

            g_hwndComposerEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_MULTILINE,
                380, 500, 400, 32, hwnd, (HMENU)IDC_COMPOSER_EDIT, GetModuleHandle(nullptr), nullptr
            );

            g_hwndSendButton = CreateWindowExW(
                0, L"BUTTON", L"Send",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                790, 500, 72, 36, hwnd, (HMENU)IDC_SEND_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndThemeButton = CreateWindowExW(
                0, L"BUTTON", L"🌓",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                850, 10, 32, 32, hwnd, (HMENU)IDC_THEME_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndAttachButton = CreateWindowExW(
                0, L"BUTTON", L"📎",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                336, 500, 36, 36, hwnd, (HMENU)IDC_ATTACH_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            SendMessageW(g_hwndSearchEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndComposerEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSendButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndThemeButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndAttachButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            LayoutChildControls(hwnd);
            return 0;
        }

        case WM_SIZE: {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            g_renderer.Resize(width, height);
            LayoutChildControls(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);
            float totalWidth = static_cast<float>(rc.right - rc.left);
            float totalHeight = static_cast<float>(rc.bottom - rc.top);
            float threadWidth = totalWidth - g_sidebarWidth;

            g_renderer.BeginDraw();

            // 1. Render Left Sidebar (Conversations List)
            g_renderer.RenderSidebar(
                g_conversations,
                g_selectedThreadId,
                g_hoveredSidebarIndex,
                g_sidebarWidth,
                totalHeight,
                g_sidebarScroll
            );

            // 2. Render Right Message Thread (Direct2D + Color Emoji)
            Conversation* activeConv = nullptr;
            for (auto& c : g_conversations) {
                if (c.threadId == g_selectedThreadId) {
                    activeConv = &c;
                    break;
                }
            }

            if (activeConv) {
                float threadHeaderH = 50.0f;
                float composerH = 54.0f;
                float visibleThreadHeight = totalHeight - threadHeaderH - composerH;

                g_renderer.RenderThread(
                    *activeConv,
                    g_sidebarWidth,
                    threadWidth,
                    visibleThreadHeight,
                    g_threadScroll,
                    g_currentBubbleLayouts
                );
            } else {
                g_renderer.RenderEmptyState(
                    g_sidebarWidth,
                    threadWidth,
                    totalHeight,
                    L"Select a conversation to start reading messages."
                );
            }

            g_renderer.EndDraw();

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            g_threadScroll = std::max(0.0f, g_threadScroll - (delta / 2.0f));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            // Clicked inside sidebar
            if (x < g_sidebarWidth) {
                float itemH = 68.0f;
                int clickedIdx = static_cast<int>((y + g_sidebarScroll) / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_conversations.size())) {
                    g_selectedThreadId = g_conversations[clickedIdx].threadId;
                    g_threadScroll = 0.0f;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }

        case WM_SETTINGCHANGE: {
            // Windows system light/dark theme change
            ThemeManager::Instance().HandleSettingChange(hwnd, lParam);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDC_SEND_BUTTON:
                    OnSendMessage(hwnd);
                    break;
                case IDC_THEME_BUTTON: {
                    // Manual theme toggle
                    bool currentlyDark = ThemeManager::Instance().IsDarkActive();
                    ThemeManager::Instance().SetMode(currentlyDark ? ThemeMode::Light : ThemeMode::Dark);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    break;
                }
                case IDC_ATTACH_BUTTON: {
                    // Open file dialog for MMS photo attachment
                    OPENFILENAMEW ofn{};
                    wchar_t szFile[MAX_PATH] = L"";
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.gif)\0*.png;*.jpg;*.jpeg;*.gif\0All Files\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        // Attach and prepare MMS preview
                    }
                    break;
                }
            }
            return 0;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            const auto& colors = ThemeManager::Instance().GetColors();
            SetTextColor(hdc, colors.win32TextColor);
            SetBkColor(hdc, colors.win32BgColor);
            static HBRUSH hbr = nullptr;
            if (hbr) DeleteObject(hbr);
            hbr = CreateSolidBrush(colors.win32BgColor);
            return (LRESULT)hbr;
        }

        case WM_DESTROY: {
            if (g_hFont) DeleteObject(g_hFont);
            ToastNotifier::Instance().Shutdown();
            SmsDaemon::Instance().Stop();
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int nCmdShow) {
    // Initialize COM for WIC and Shell
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        L"KDE Connect SMS - Windows Win32",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Standard Win32 Message Loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

// Fallback entry point for MinGW-w64 when -municode is omitted
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR /*lpCmdLine*/, int nCmdShow) {
    return wWinMain(hInstance, hPrevInstance, GetCommandLineW(), nCmdShow);
}

