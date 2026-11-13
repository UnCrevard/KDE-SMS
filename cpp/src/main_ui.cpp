#include "common.h"
#include "theme_manager.h"
#include "direct2d_renderer.h"
#include "toast_notifier.h"
#include "daemon.h"

using namespace KDEConnect;

// Global state for Win32 UI window
static const wchar_t* WINDOW_CLASS_NAME = L"KDEConnectSMSWin32";
static Direct2DRenderer g_renderer;
static std::vector<Conversation> g_allConversations;
static std::vector<Conversation> g_visibleConversations;
static std::wstring g_searchFilter;
static int64_t g_selectedThreadId = -1;
static int g_hoveredSidebarIndex = -1;
static float g_sidebarWidth = 320.0f;
static float g_sidebarScroll = 0.0f;
static float g_threadScroll = 0.0f;
static std::vector<BubbleLayout> g_currentBubbleLayouts;

// Network bind IP
static std::wstring g_bindIp = L"";

// Child controls
static HWND g_hwndMain = nullptr;
static HWND g_hwndSearchEdit = nullptr;
static HWND g_hwndComposerEdit = nullptr;
static HWND g_hwndSendButton = nullptr;
static HWND g_hwndThemeButton = nullptr;
static HWND g_hwndSyncButton = nullptr;
static HWND g_hwndAttachButton = nullptr;
static HFONT g_hFont = nullptr;

#define IDC_SEARCH_EDIT      1001
#define IDC_COMPOSER_EDIT    1002
#define IDC_SEND_BUTTON      1003
#define IDC_THEME_BUTTON     1004
#define IDC_ATTACH_BUTTON    1005
#define IDC_SYNC_BUTTON      1006
#define IDC_SYNC_TIMER       1
#define WM_APP_REFRESH_CONVS (WM_APP + 1)

static std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::towlower);
    return out;
}

static Conversation* FindActiveConversation() {
    for (auto& c : g_visibleConversations) {
        if (c.threadId == g_selectedThreadId) return &c;
    }
    for (auto& c : g_allConversations) {
        if (c.threadId == g_selectedThreadId) return &c;
    }
    return nullptr;
}

static void ScrollThreadToBottom(HWND hwnd) {
    Conversation* active = FindActiveConversation();
    if (!active) {
        g_threadScroll = 0.0f;
        return;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float totalWidth = static_cast<float>(rc.right - rc.left);
    float totalHeight = static_cast<float>(rc.bottom - rc.top);
    float threadWidth = std::max(200.0f, totalWidth - g_sidebarWidth);
    float visibleThreadHeight = std::max(100.0f, totalHeight - 54.0f - 54.0f);

    float contentHeight = g_renderer.CalculateTotalThreadHeight(*active, threadWidth);
    g_threadScroll = std::max(0.0f, contentHeight - visibleThreadHeight);
}

static void ApplySearchFilter() {
    g_visibleConversations.clear();
    if (g_searchFilter.empty()) {
        g_visibleConversations = g_allConversations;
        return;
    }
    std::wstring q = ToLowerCopy(g_searchFilter);
    for (const auto& conv : g_allConversations) {
        if (ToLowerCopy(conv.contactName).find(q) != std::wstring::npos ||
            ToLowerCopy(conv.address).find(q) != std::wstring::npos ||
            ToLowerCopy(conv.snippet).find(q) != std::wstring::npos) {
            g_visibleConversations.push_back(conv);
        }
    }
}

static void ReloadConversations(HWND hwnd, bool autoScrollIfNewOrActive) {
    size_t prevSelectedCount = 0;
    if (Conversation* prev = FindActiveConversation()) {
        prevSelectedCount = prev->messages.size();
    }

    g_allConversations = SmsDaemon::Instance().LoadAllConversations();
    ApplySearchFilter();

    bool selectedExists = false;
    for (const auto& c : g_allConversations) {
        if (c.threadId == g_selectedThreadId) {
            selectedExists = true;
            break;
        }
    }

    bool selectionChanged = false;
    if (!selectedExists && !g_visibleConversations.empty()) {
        g_selectedThreadId = g_visibleConversations.front().threadId;
        selectionChanged = true;
        SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
    }

    size_t newSelectedCount = 0;
    if (Conversation* cur = FindActiveConversation()) {
        newSelectedCount = cur->messages.size();
    }

    if (autoScrollIfNewOrActive && (selectionChanged || newSelectedCount > prevSelectedCount)) {
        ScrollThreadToBottom(hwnd);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

void LayoutChildControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    // Search box in sidebar top
    if (g_hwndSearchEdit) {
        SetWindowPos(g_hwndSearchEdit, nullptr, 16, 13, static_cast<int>(g_sidebarWidth) - 32, 28, SWP_NOZORDER);
    }

    // Composer bar at bottom of thread view
    int composerHeight = 54;
    int composerY = height - composerHeight;
    int threadWidth = width - static_cast<int>(g_sidebarWidth);

    if (g_hwndAttachButton) {
        SetWindowPos(g_hwndAttachButton, nullptr, static_cast<int>(g_sidebarWidth) + 16, composerY + 9, 36, 36, SWP_NOZORDER);
    }

    if (g_hwndComposerEdit) {
        int editX = static_cast<int>(g_sidebarWidth) + 60;
        int editWidth = std::max(80, threadWidth - 160);
        SetWindowPos(g_hwndComposerEdit, nullptr, editX, composerY + 11, editWidth, 32, SWP_NOZORDER);
    }

    if (g_hwndSendButton) {
        int sendX = width - 88;
        SetWindowPos(g_hwndSendButton, nullptr, sendX, composerY + 9, 72, 36, SWP_NOZORDER);
    }

    if (g_hwndSyncButton) {
        SetWindowPos(g_hwndSyncButton, nullptr, width - 84, 11, 34, 32, SWP_NOZORDER);
    }

    if (g_hwndThemeButton) {
        SetWindowPos(g_hwndThemeButton, nullptr, width - 44, 11, 34, 32, SWP_NOZORDER);
    }
}

void OnSendMessage(HWND hwnd) {
    int length = GetWindowTextLengthW(g_hwndComposerEdit);
    if (length <= 0) return;

    std::wstring text(length + 1, 0);
    GetWindowTextW(g_hwndComposerEdit, &text[0], length + 1);
    text.resize(length);

    Conversation* active = FindActiveConversation();
    if (!active || active->address.empty()) {
        return;
    }

    // Send via daemon (host TLS or IPC to background daemon)
    SmsDaemon::Instance().SendOutgoingSms(active->address, text, active->threadId);

    // Clear composer edit box
    SetWindowTextW(g_hwndComposerEdit, L"");

    // Reload conversation and scroll to newest bubble
    ReloadConversations(hwnd, true);
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

            // Register live conversation update hook before starting daemon
            SmsDaemon::Instance().SetOnConversationsUpdated([]() {
                if (g_hwndMain && IsWindow(g_hwndMain)) {
                    PostMessageW(g_hwndMain, WM_APP_REFRESH_CONVS, 0, 0);
                }
            });

            // Start background sync daemon (or attach as IPC client if KDEConnectDaemon already runs)
            SmsDaemon::Instance().Start(g_bindIp);

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
                16, 13, 280, 28, hwnd, (HMENU)IDC_SEARCH_EDIT, GetModuleHandle(nullptr), nullptr
            );
            SendMessageW(g_hwndSearchEdit, 0x1501 /* EM_SETCUEBANNER */, FALSE, (LPARAM)L"Rechercher un contact ou SMS...");

            g_hwndComposerEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_MULTILINE,
                380, 500, 400, 32, hwnd, (HMENU)IDC_COMPOSER_EDIT, GetModuleHandle(nullptr), nullptr
            );

            g_hwndSendButton = CreateWindowExW(
                0, L"BUTTON", L"Envoyer",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                790, 500, 72, 36, hwnd, (HMENU)IDC_SEND_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndSyncButton = CreateWindowExW(
                0, L"BUTTON", L"↻",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                810, 11, 34, 32, hwnd, (HMENU)IDC_SYNC_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndThemeButton = CreateWindowExW(
                0, L"BUTTON", L"🌓",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                850, 11, 34, 32, hwnd, (HMENU)IDC_THEME_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndAttachButton = CreateWindowExW(
                0, L"BUTTON", L"📎",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                336, 500, 36, 36, hwnd, (HMENU)IDC_ATTACH_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            SendMessageW(g_hwndSearchEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndComposerEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSendButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSyncButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndThemeButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndAttachButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            LayoutChildControls(hwnd);
            ReloadConversations(hwnd, true);

            // Periodic sync check every 5s until conversations load (or keeps connection status fresh)
            SetTimer(hwnd, IDC_SYNC_TIMER, 5000, nullptr);
            return 0;
        }

        case WM_APP_REFRESH_CONVS: {
            ReloadConversations(hwnd, true);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == IDC_SYNC_TIMER) {
                if (g_allConversations.empty()) {
                    SmsDaemon::Instance().RequestConversationsSync();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
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
            float threadWidth = std::max(100.0f, totalWidth - g_sidebarWidth);

            g_renderer.BeginDraw();

            // 1. Render Left Sidebar (Conversations List)
            g_renderer.RenderSidebar(
                g_visibleConversations,
                g_selectedThreadId,
                g_hoveredSidebarIndex,
                g_sidebarWidth,
                totalHeight,
                g_sidebarScroll
            );

            // 2. Render Right Message Thread (Direct2D + Color Emoji)
            Conversation* activeConv = FindActiveConversation();

            if (activeConv) {
                float threadHeaderH = 54.0f;
                float composerH = 54.0f;
                float visibleThreadHeight = std::max(50.0f, totalHeight - threadHeaderH - composerH);

                g_renderer.RenderThread(
                    *activeConv,
                    g_sidebarWidth,
                    threadWidth,
                    visibleThreadHeight,
                    g_threadScroll,
                    g_currentBubbleLayouts
                );
            } else {
                std::wstring emptyMsg = L"Aucune conversation SMS chargee.\n\n"
                                        L"Statut : " + SmsDaemon::Instance().GetConnectionStatusText() + L"\n\n"
                                        L"Assurez-vous que KDE Connect est appaire sur le telephone,\n"
                                        L"que l'autorisation SMS est accordee, puis cliquez sur ↻.";
                g_renderer.RenderEmptyState(
                    g_sidebarWidth,
                    threadWidth,
                    totalHeight,
                    emptyMsg
                );
            }

            g_renderer.EndDraw();

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);

            RECT rc{};
            GetClientRect(hwnd, &rc);
            float totalWidth = static_cast<float>(rc.right - rc.left);
            float totalHeight = static_cast<float>(rc.bottom - rc.top);

            if (pt.x < static_cast<LONG>(g_sidebarWidth)) {
                float maxSidebarScroll = std::max(0.0f, static_cast<float>(g_visibleConversations.size()) * 68.0f - (totalHeight - 54.0f));
                g_sidebarScroll = std::clamp(g_sidebarScroll - (delta / 2.0f), 0.0f, maxSidebarScroll);
            } else {
                Conversation* active = FindActiveConversation();
                float maxThreadScroll = 0.0f;
                if (active) {
                    float threadWidth = std::max(200.0f, totalWidth - g_sidebarWidth);
                    float visibleThreadHeight = std::max(100.0f, totalHeight - 54.0f - 54.0f);
                    float contentHeight = g_renderer.CalculateTotalThreadHeight(*active, threadWidth);
                    maxThreadScroll = std::max(0.0f, contentHeight - visibleThreadHeight);
                }
                g_threadScroll = std::clamp(g_threadScroll - (delta / 2.0f), 0.0f, maxThreadScroll);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            // Clicked inside sidebar list area (below 54px search bar)
            if (x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                float itemH = 68.0f;
                int clickedIdx = static_cast<int>((static_cast<float>(y) - 54.0f + g_sidebarScroll) / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_visibleConversations.size())) {
                    g_selectedThreadId = g_visibleConversations[clickedIdx].threadId;
                    ScrollThreadToBottom(hwnd);
                    SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
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
            int wmCode = HIWORD(wParam);
            switch (wmId) {
                case IDC_SEARCH_EDIT:
                    if (wmCode == EN_CHANGE) {
                        int len = GetWindowTextLengthW(g_hwndSearchEdit);
                        std::wstring buf(len + 1, 0);
                        if (len > 0) {
                            GetWindowTextW(g_hwndSearchEdit, &buf[0], len + 1);
                        }
                        buf.resize(len);
                        g_searchFilter = buf;
                        g_sidebarScroll = 0.0f;
                        ApplySearchFilter();
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
                case IDC_SEND_BUTTON:
                    OnSendMessage(hwnd);
                    break;
                case IDC_SYNC_BUTTON:
                    SmsDaemon::Instance().RequestConversationsSync();
                    if (g_selectedThreadId > 0) {
                        SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
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
            KillTimer(hwnd, IDC_SYNC_TIMER);
            SmsDaemon::Instance().SetOnConversationsUpdated(nullptr);
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR pCmdLine, int nCmdShow) {
    if (pCmdLine && wcslen(pCmdLine) > 0) {
        std::wstring cmd(pCmdLine);
        size_t ipPos = cmd.find(L"--ip");
        if (ipPos == std::string::npos) ipPos = cmd.find(L"--bind");
        if (ipPos != std::string::npos) {
            size_t space = cmd.find_first_of(L" \t", ipPos);
            if (space != std::string::npos) {
                size_t valStart = cmd.find_first_not_of(L" \t", space);
                if (valStart != std::string::npos) {
                    size_t valEnd = cmd.find_first_of(L" \t\r\n", valStart);
                    g_bindIp = cmd.substr(valStart, valEnd - valStart);
                }
            }
        } else {
            size_t start = cmd.find_first_not_of(L" \t");
            if (start != std::string::npos && cmd[start] != L'-') {
                size_t end = cmd.find_first_of(L" \t\r\n", start);
                g_bindIp = cmd.substr(start, end - start);
            }
        }
    }

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
