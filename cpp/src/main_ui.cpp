#include "common.h"
#include "theme_manager.h"
#include "direct2d_renderer.h"
#include "toast_notifier.h"
#include "daemon.h"
#include <filesystem>

using namespace KDEConnect;

// Global state for Win32 UI window
static const wchar_t* WINDOW_CLASS_NAME = L"KDEConnectSMSWin32";
static Direct2DRenderer g_renderer;
static std::vector<Conversation> g_allConversations;
static std::vector<Conversation> g_visibleConversations;
static std::wstring g_searchFilter;
static std::wstring g_lastEmptyStatus;
static int64_t g_selectedThreadId = -1;
static int g_hoveredSidebarIndex = -1;
static bool g_trackingMouseLeave = false;
static float g_sidebarWidth = 320.0f;
static float g_sidebarScroll = 0.0f;
static float g_threadScroll = 0.0f;
static std::vector<BubbleLayout> g_currentBubbleLayouts;
static std::wstring g_pendingAttachmentPath;
static std::wstring g_pendingAttachmentName;

// Network bind IP
static std::wstring g_bindIp = L"";

// Child controls & cached GDI resources
static HWND g_hwndMain = nullptr;
static HWND g_hwndSearchEdit = nullptr;
static HWND g_hwndComposerEdit = nullptr;
static HWND g_hwndSendButton = nullptr;
static HWND g_hwndThemeButton = nullptr;
static HWND g_hwndSyncButton = nullptr;
static HWND g_hwndFolderButton = nullptr;
static HWND g_hwndAttachButton = nullptr;
static HFONT g_hFont = nullptr;
static HBRUSH g_editBrush = nullptr;
static COLORREF g_editBrushColor = RGB(0, 0, 0);
static WNDPROC g_origComposerProc = nullptr;
static bool g_suppressNextEnterChar = false;
static bool g_forceExit = false;
static UINT g_msgTaskbarCreated = 0;

#define IDC_SEARCH_EDIT      1001
#define IDC_COMPOSER_EDIT    1002
#define IDC_SEND_BUTTON      1003
#define IDC_THEME_BUTTON     1004
#define IDC_ATTACH_BUTTON    1005
#define IDC_SYNC_BUTTON      1006
#define IDC_FOLDER_BUTTON    1007
#define IDC_SYNC_TIMER       1
#define IDC_DEBOUNCE_TIMER   2
#define IDM_DELETE_CONV      2001
#define IDM_TRAY_SHOW        2002
#define IDM_TRAY_EXIT        2003
#define WM_APP_REFRESH_CONVS (WM_APP + 1)

// Forward declaration
void OnSendMessage(HWND hwnd);

static void SaveWindowBoundsToIni(HWND hwnd) {
    RECT rc{};
    if (hwnd && !IsIconic(hwnd) && GetWindowRect(hwnd, &rc)) {
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w >= 480 && h >= 360) {
            WriteIniInt(L"Window", L"Width", w);
            WriteIniInt(L"Window", L"Height", h);
        }
    }
}

static void RestoreFromTray(HWND hwnd) {
    if (!hwnd) return;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    SetForegroundWindow(hwnd);
}

static LRESULT CALLBACK ComposerEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        if ((GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            g_suppressNextEnterChar = true;
            if (g_hwndMain) {
                OnSendMessage(g_hwndMain);
            }
            return 0;
        }
        g_suppressNextEnterChar = false;
    } else if (msg == WM_CHAR && (wParam == L'\r' || wParam == L'\n')) {
        if (g_suppressNextEnterChar) {
            g_suppressNextEnterChar = false;
            return 0;
        }
    }
    return CallWindowProcW(g_origComposerProc, hwnd, msg, wParam, lParam);
}

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

static bool ConversationsSnapshotEqual(const std::vector<Conversation>& a, const std::vector<Conversation>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].threadId != b[i].threadId ||
            a[i].lastTimestamp != b[i].lastTimestamp ||
            a[i].messages.size() != b[i].messages.size() ||
            a[i].contactName != b[i].contactName ||
            a[i].snippet != b[i].snippet) {
            return false;
        }
    }
    return true;
}

static void ClampSidebarScroll(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float totalHeight = static_cast<float>(rc.bottom - rc.top);
    float maxSidebarScroll = std::max(0.0f, static_cast<float>(g_visibleConversations.size()) * 68.0f - (totalHeight - 54.0f));
    g_sidebarScroll = std::clamp(g_sidebarScroll, 0.0f, maxSidebarScroll);
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
    float extraBarH = g_pendingAttachmentName.empty() ? 0.0f : 30.0f;
    float visibleThreadHeight = std::max(100.0f, totalHeight - 54.0f - 54.0f - extraBarH);

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

static void ReloadConversations(HWND hwnd, bool autoScrollIfNewOrActive, bool forceRepaint = false) {
    size_t prevSelectedCount = 0;
    if (Conversation* prev = FindActiveConversation()) {
        prevSelectedCount = prev->messages.size();
    }

    std::vector<Conversation> fresh = SmsDaemon::Instance().LoadAllConversations();
    bool listChanged = !ConversationsSnapshotEqual(g_allConversations, fresh);
    if (!listChanged && !forceRepaint) {
        return;
    }

    g_allConversations = std::move(fresh);
    ApplySearchFilter();
    ClampSidebarScroll(hwnd);

    bool selectedExists = false;
    for (const auto& c : g_allConversations) {
        if (c.threadId == g_selectedThreadId) {
            selectedExists = true;
            break;
        }
    }

    bool selectionChanged = false;
    if (!selectedExists) {
        if (!g_visibleConversations.empty()) {
            g_selectedThreadId = g_visibleConversations.front().threadId;
            selectionChanged = true;
            SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
        } else {
            g_selectedThreadId = -1;
            selectionChanged = true;
        }
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

static void PerformDeleteConversation(HWND hwnd, int idx, bool askConfirm) {
    if (idx < 0 || idx >= static_cast<int>(g_visibleConversations.size())) return;

    Conversation target = g_visibleConversations[idx];
    if (askConfirm) {
        std::wstring label = target.contactName.empty() ? target.address : target.contactName;
        std::wstring prompt = L"Effacer la conversation avec \"" + label + L"\" de la liste ?";
        if (MessageBoxW(hwnd, prompt.c_str(), L"Supprimer la conversation", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }

    SmsDaemon::Instance().DeleteConversation(target.threadId);
    g_hoveredSidebarIndex = -1;
    ReloadConversations(hwnd, true, true);
}

static bool PromptSelectStorageFolder(HWND owner, const std::wstring& title, const std::wstring& initialDir, std::wstring& outPath) {
    IFileOpenDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (FAILED(hr) || !pfd) return false;

    DWORD options = 0;
    if (SUCCEEDED(pfd->GetOptions(&options))) {
        pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    }
    pfd->SetTitle(title.c_str());
    pfd->SetOkButtonLabel(L"Choisir ce dossier");

    if (!initialDir.empty()) {
        IShellItem* pInitItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr, IID_PPV_ARGS(&pInitItem))) && pInitItem) {
            pfd->SetFolder(pInitItem);
            pInitItem->Release();
        }
    }

    bool chosen = false;
    if (SUCCEEDED(pfd->Show(owner))) {
        IShellItem* psiResult = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psiResult)) && psiResult) {
            PWSTR pszFilePath = nullptr;
            if (SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath)) && pszFilePath) {
                outPath = pszFilePath;
                CoTaskMemFree(pszFilePath);
                chosen = !outPath.empty();
            }
            psiResult->Release();
        }
    }
    pfd->Release();
    return chosen;
}

static void PromptFirstRunStorageSetup() {
    if (HasConfiguredConversationsDirectory()) return;

    std::wstring defaultDir = GetDefaultConversationsDirectory();
    std::wstring msg = L"Bienvenue dans KDE Connect SMS !\n\n"
                       L"Pour ce premier demarrage, choisissez le dossier ou stocker vos conversations.\n"
                       L"Les reglages de l'application seront enregistres dans :\n"
                       + GetSettingsIniPath() + L"\n\n"
                       L"• OK : Parcourir et choisir un dossier\n"
                       L"• Annuler : Utiliser le dossier par defaut :\n  " + defaultDir;

    std::wstring chosenDir = defaultDir;
    int res = MessageBoxW(nullptr, msg.c_str(), L"Premier demarrage - Stockage des conversations", MB_OKCANCEL | MB_ICONINFORMATION);
    if (res == IDOK) {
        std::wstring picked;
        if (PromptSelectStorageFolder(nullptr, L"Choisir le dossier de stockage des conversations", GetAppDataDirectory(), picked)) {
            chosenDir = picked;
        }
    }

    SetConversationsDirectory(chosenDir);
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

    if (g_hwndFolderButton) {
        SetWindowPos(g_hwndFolderButton, nullptr, width - 124, 11, 34, 32, SWP_NOZORDER);
    }

    if (g_hwndSyncButton) {
        SetWindowPos(g_hwndSyncButton, nullptr, width - 84, 11, 34, 32, SWP_NOZORDER);
    }

    if (g_hwndThemeButton) {
        SetWindowPos(g_hwndThemeButton, nullptr, width - 44, 11, 34, 32, SWP_NOZORDER);
    }
}

static void ClearPendingAttachment(HWND hwnd) {
    g_pendingAttachmentPath.clear();
    g_pendingAttachmentName.clear();
    if (g_hwndAttachButton) {
        SetWindowTextW(g_hwndAttachButton, L"📎");
    }
    if (hwnd) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void OnSendMessage(HWND hwnd) {
    int length = GetWindowTextLengthW(g_hwndComposerEdit);
    std::wstring text;
    if (length > 0) {
        text.resize(length + 1, 0);
        GetWindowTextW(g_hwndComposerEdit, &text[0], length + 1);
        text.resize(length);

        // Trim trailing CR/LF/whitespace
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'\t')) {
            text.pop_back();
        }
    }

    if (text.empty() && g_pendingAttachmentPath.empty()) {
        if (length > 0) {
            SetWindowTextW(g_hwndComposerEdit, L"");
        }
        return;
    }

    Conversation* active = FindActiveConversation();
    if (!active || active->address.empty()) {
        return;
    }

    // Send via daemon (host TLS or IPC to background daemon)
    bool sentOk = SmsDaemon::Instance().SendOutgoingSms(active->address, text, active->threadId, g_pendingAttachmentPath);
    if (!sentOk) {
        MessageBoxW(
            hwnd,
            L"Impossible d'envoyer le message ou l'image.\nVerifiez que le telephone est connecte et appaire, et que le fichier image est accessible.",
            L"Echec d'envoi",
            MB_OK | MB_ICONERROR
        );
        return;
    }

    // Clear composer edit box and pending image
    SetWindowTextW(g_hwndComposerEdit, L"");
    ClearPendingAttachment(nullptr);

    // Reload conversation and scroll to newest bubble
    ReloadConversations(hwnd, true, true);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hwndMain = hwnd;

            // Initialize Theme Manager & detect dark/light theme
            ThemeManager::Instance().Initialize(hwnd);

            // Initialize Direct2D & DirectWrite engine
            g_renderer.Initialize(hwnd);

            // Register Explorer taskbar restart notification
            g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

            // Initialize Windows Toast Notifier & SMS Tray Icon
            ToastNotifier::Instance().Initialize(hwnd);
            if (HICON hSmsIcon = ToastNotifier::Instance().GetSmsIcon()) {
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmsIcon));
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hSmsIcon));
            }

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
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
                380, 500, 400, 32, hwnd, (HMENU)IDC_COMPOSER_EDIT, GetModuleHandle(nullptr), nullptr
            );
            g_origComposerProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(g_hwndComposerEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ComposerEditProc))
            );

            g_hwndSendButton = CreateWindowExW(
                0, L"BUTTON", L"Envoyer",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                790, 500, 72, 36, hwnd, (HMENU)IDC_SEND_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndFolderButton = CreateWindowExW(
                0, L"BUTTON", L"📁",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                770, 11, 34, 32, hwnd, (HMENU)IDC_FOLDER_BUTTON, GetModuleHandle(nullptr), nullptr
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
            SendMessageW(g_hwndFolderButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSyncButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndThemeButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndAttachButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            LayoutChildControls(hwnd);
            ReloadConversations(hwnd, true, true);

            // Periodic sync check every 5s when no conversations are loaded yet
            SetTimer(hwnd, IDC_SYNC_TIMER, 5000, nullptr);
            return 0;
        }

        case WM_ERASEBKGND: {
            // Direct2D paints the entire client area; suppress GDI background erase to prevent flicker
            return 1;
        }

        case WM_APP_REFRESH_CONVS: {
            // Coalesce bursts of incoming packet/disk updates into a single smooth UI refresh
            SetTimer(hwnd, IDC_DEBOUNCE_TIMER, 120, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == IDC_DEBOUNCE_TIMER) {
                KillTimer(hwnd, IDC_DEBOUNCE_TIMER);
                ReloadConversations(hwnd, true);
            } else if (wParam == IDC_SYNC_TIMER) {
                if (g_allConversations.empty()) {
                    SmsDaemon::Instance().RequestConversationsSync();
                    std::wstring status = SmsDaemon::Instance().GetConnectionStatusText();
                    if (status != g_lastEmptyStatus) {
                        g_lastEmptyStatus = status;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
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

                if (!g_pendingAttachmentName.empty()) {
                    g_renderer.RenderPendingAttachmentBar(
                        g_sidebarWidth,
                        threadWidth,
                        totalHeight - composerH,
                        g_pendingAttachmentName
                    );
                }
            } else {
                g_lastEmptyStatus = SmsDaemon::Instance().GetConnectionStatusText();
                std::wstring emptyMsg = L"Aucune conversation SMS chargee.\n\n"
                                        L"Statut : " + g_lastEmptyStatus + L"\n\n"
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

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            if (!g_trackingMouseLeave) {
                TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_trackingMouseLeave = true;
            }

            int newHover = -1;
            if (x >= 0 && x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                int idx = static_cast<int>((static_cast<float>(y) - 54.0f + g_sidebarScroll) / 68.0f);
                if (idx >= 0 && idx < static_cast<int>(g_visibleConversations.size())) {
                    newHover = idx;
                }
            }

            if (newHover != g_hoveredSidebarIndex) {
                g_hoveredSidebarIndex = newHover;
                RECT rc{};
                GetClientRect(hwnd, &rc);
                RECT sidebarOnly{ 0, 54, static_cast<LONG>(g_sidebarWidth), rc.bottom };
                InvalidateRect(hwnd, &sidebarOnly, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            g_trackingMouseLeave = false;
            if (g_hoveredSidebarIndex != -1) {
                g_hoveredSidebarIndex = -1;
                RECT rc{};
                GetClientRect(hwnd, &rc);
                RECT sidebarOnly{ 0, 54, static_cast<LONG>(g_sidebarWidth), rc.bottom };
                InvalidateRect(hwnd, &sidebarOnly, FALSE);
            }
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
                RECT sidebarOnly{ 0, 54, static_cast<LONG>(g_sidebarWidth), rc.bottom };
                InvalidateRect(hwnd, &sidebarOnly, FALSE);
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
                RECT threadOnly{ static_cast<LONG>(g_sidebarWidth), 54, rc.right, rc.bottom - 54 };
                InvalidateRect(hwnd, &threadOnly, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            RECT rc{};
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            // Click on [x] button inside the pending attachment preview bar
            if (!g_pendingAttachmentName.empty() &&
                x >= width - 40 && x <= width - 6 &&
                y >= height - 84 && y <= height - 54) {
                ClearPendingAttachment(hwnd);
                return 0;
            }

            // Clicked inside sidebar list area (below 54px search bar)
            if (x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                float itemH = 68.0f;
                float relY = static_cast<float>(y) - 54.0f + g_sidebarScroll;
                int clickedIdx = static_cast<int>(relY / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_visibleConversations.size())) {
                    float rowLocalY = relY - static_cast<float>(clickedIdx) * itemH;
                    bool hitDeleteBtn = (x >= static_cast<int>(g_sidebarWidth) - 32 &&
                                         x <= static_cast<int>(g_sidebarWidth) - 6 &&
                                         rowLocalY >= 33.0f && rowLocalY <= 59.0f);
                    if (hitDeleteBtn) {
                        PerformDeleteConversation(hwnd, clickedIdx, true);
                    } else {
                        g_selectedThreadId = g_visibleConversations[clickedIdx].threadId;
                        ScrollThreadToBottom(hwnd);
                        SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            if (x >= 0 && x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                float itemH = 68.0f;
                int clickedIdx = static_cast<int>((static_cast<float>(y) - 54.0f + g_sidebarScroll) / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_visibleConversations.size())) {
                    HMENU hMenu = CreatePopupMenu();
                    if (hMenu) {
                        AppendMenuW(hMenu, MF_STRING, IDM_DELETE_CONV, L"Supprimer la conversation");
                        POINT screenPt{ x, y };
                        ClientToScreen(hwnd, &screenPt);
                        UINT cmd = TrackPopupMenu(
                            hMenu,
                            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                            screenPt.x,
                            screenPt.y,
                            0,
                            hwnd,
                            nullptr
                        );
                        DestroyMenu(hMenu);
                        if (cmd == IDM_DELETE_CONV) {
                            PerformDeleteConversation(hwnd, clickedIdx, false);
                        }
                    }
                }
            }
            return 0;
        }

        case WM_SETTINGCHANGE: {
            // Windows system light/dark theme change
            ThemeManager::Instance().HandleSettingChange(hwnd, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
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
                        RECT rc{};
                        GetClientRect(hwnd, &rc);
                        RECT sidebarOnly{ 0, 54, static_cast<LONG>(g_sidebarWidth), rc.bottom };
                        InvalidateRect(hwnd, &sidebarOnly, FALSE);
                    }
                    break;
                case IDC_SEND_BUTTON:
                    OnSendMessage(hwnd);
                    break;
                case IDC_FOLDER_BUTTON: {
                    std::wstring curDir = GetMessagesDirectory();
                    std::wstring picked;
                    if (PromptSelectStorageFolder(hwnd, L"Choisir le dossier de stockage des conversations", curDir, picked)) {
                        SetConversationsDirectory(picked);
                        SmsDaemon::Instance().ReloadStorageDirectory();
                        SmsDaemon::Instance().RequestConversationsSync();
                        ReloadConversations(hwnd, true, true);
                    }
                    break;
                }
                case IDC_SYNC_BUTTON:
                    SmsDaemon::Instance().RequestConversationsSync();
                    if (g_selectedThreadId > 0) {
                        SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
                    }
                    break;
                case IDC_THEME_BUTTON: {
                    // Manual theme toggle
                    bool currentlyDark = ThemeManager::Instance().IsDarkActive();
                    ThemeManager::Instance().SetMode(currentlyDark ? ThemeMode::Light : ThemeMode::Dark);
                    InvalidateRect(hwnd, nullptr, FALSE);
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
                    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp)\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp\0Tous les fichiers (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameW(&ofn)) {
                        g_pendingAttachmentPath = szFile;
                        g_pendingAttachmentName = std::filesystem::path(szFile).filename().wstring();
                        if (g_hwndAttachButton) {
                            SetWindowTextW(g_hwndAttachButton, L"📎✓");
                        }
                        if (g_hwndComposerEdit) {
                            SetFocus(g_hwndComposerEdit);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
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
            if (!g_editBrush || g_editBrushColor != colors.win32BgColor) {
                if (g_editBrush) DeleteObject(g_editBrush);
                g_editBrushColor = colors.win32BgColor;
                g_editBrush = CreateSolidBrush(g_editBrushColor);
            }
            return (LRESULT)g_editBrush;
        }

        case WM_CLOSE: {
            SaveWindowBoundsToIni(hwnd);
            if (!g_forceExit) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_TRAY_ICON: {
            UINT eventMsg = LOWORD(static_cast<DWORD>(lParam));
            if (eventMsg == WM_LBUTTONUP || eventMsg == WM_LBUTTONDBLCLK || eventMsg == NIN_BALLOONUSERCLICK) {
                RestoreFromTray(hwnd);
            } else if (eventMsg == WM_RBUTTONUP || eventMsg == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                if (hMenu) {
                    bool visible = (IsWindowVisible(hwnd) != FALSE && !IsIconic(hwnd));
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? L"Masquer la fenetre" : L"Ouvrir KDE Connect SMS");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Quitter");
                    SetMenuDefaultItem(hMenu, IDM_TRAY_SHOW, FALSE);

                    SetForegroundWindow(hwnd);
                    UINT cmd = TrackPopupMenu(
                        hMenu,
                        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                        pt.x, pt.y, 0, hwnd, nullptr
                    );
                    PostMessageW(hwnd, WM_NULL, 0, 0);
                    DestroyMenu(hMenu);

                    if (cmd == IDM_TRAY_SHOW) {
                        if (visible) {
                            ShowWindow(hwnd, SW_HIDE);
                        } else {
                            RestoreFromTray(hwnd);
                        }
                    } else if (cmd == IDM_TRAY_EXIT) {
                        g_forceExit = true;
                        DestroyWindow(hwnd);
                    }
                }
            }
            return 0;
        }

        case WM_DESTROY: {
            SaveWindowBoundsToIni(hwnd);
            KillTimer(hwnd, IDC_SYNC_TIMER);
            KillTimer(hwnd, IDC_DEBOUNCE_TIMER);
            SmsDaemon::Instance().SetOnConversationsUpdated(nullptr);
            if (g_origComposerProc && g_hwndComposerEdit) {
                SetWindowLongPtrW(g_hwndComposerEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origComposerProc));
                g_origComposerProc = nullptr;
            }
            if (g_editBrush) {
                DeleteObject(g_editBrush);
                g_editBrush = nullptr;
            }
            if (g_hFont) DeleteObject(g_hFont);
            ToastNotifier::Instance().Shutdown();
            SmsDaemon::Instance().Stop();
            PostQuitMessage(0);
            return 0;
        }

        default:
            if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated) {
                ToastNotifier::Instance().EnsureTrayIcon();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR pCmdLine, int nCmdShow) {
    if (pCmdLine && wcslen(pCmdLine) > 0) {
        std::wstring cmd(pCmdLine);
        size_t ipPos = cmd.find(L"--ip");
        if (ipPos == std::wstring::npos) ipPos = cmd.find(L"--bind");
        if (ipPos != std::wstring::npos) {
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

    // Initialize COM for WIC, Shell & Folder Picker
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // First-run prompt for conversation storage folder + create local INI in current directory
    PromptFirstRunStorageSetup();

    if (g_bindIp.empty()) {
        g_bindIp = ReadIniString(L"Network", L"BindIp", L"");
    } else {
        WriteIniString(L"Network", L"BindIp", g_bindIp.c_str());
    }

    int initWidth = std::max(640, ReadIniInt(L"Window", L"Width", 960));
    int initHeight = std::max(480, ReadIniInt(L"Window", L"Height", 680));

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
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        L"KDE Connect SMS - Windows Win32",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, initWidth, initHeight,
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
