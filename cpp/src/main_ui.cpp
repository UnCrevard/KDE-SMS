#include "common.h"
#include "theme_manager.h"
#include "direct2d_renderer.h"
#include "toast_notifier.h"
#include "daemon.h"
#include "i18n.h"
#include <richedit.h>
#include <shellapi.h>
#include <filesystem>
#include <map>
#include <set>
#include <cwctype>
#include <cstring>

using namespace KDEConnect;

// Global state for Win32 UI window
static const wchar_t* WINDOW_CLASS_NAME = L"KDEConnectSMSWin32";
static Direct2DRenderer g_renderer;
static std::vector<Conversation> g_allConversations;
static std::vector<Conversation> g_visibleConversations;
static std::vector<ContactSummary> g_allContacts;
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
static HWND g_hwndSettingsButton = nullptr;
static HWND g_hwndAttachButton = nullptr;
static HWND g_hwndEmojiButton = nullptr;
static HWND g_hwndConnTooltip = nullptr;
static HMODULE g_hMsftEdit = nullptr;
static bool g_composerIsRichEdit = false;
static bool g_lastPhoneConnected = false;
static std::wstring g_lastPhoneName;
static int g_lastBatteryLevel = -1;
static bool g_lastBatteryCharging = false;
static HFONT g_hFont = nullptr;
static HBRUSH g_editBrush = nullptr;
static COLORREF g_editBrushColor = RGB(0, 0, 0);
static HBRUSH g_btnBrush = nullptr;
static COLORREF g_btnBrushColor = RGB(0, 0, 0);
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
#define IDC_SETTINGS_BUTTON  1008
#define IDC_EMOJI_BUTTON     1009
#define IDC_SYNC_TIMER       1
#define IDC_DEBOUNCE_TIMER   2
#define IDC_CONN_POLL_TIMER  3
#define IDM_DELETE_CONV      2001
#define IDM_TRAY_SHOW         2002
#define IDM_TRAY_EXIT         2003
#define IDM_TRAY_SETTINGS     2004
#define WM_APP_REFRESH_CONVS  (WM_APP + 1)
#define WM_APP_RESTORE_WINDOW (WM_APP + 2)
#define WM_APP_RELOAD_INI     (WM_APP + 3)
#define WM_APP_BIND_ALERT     (WM_APP + 4)
#define WM_APP_CONN_CHANGED   (WM_APP + 5)

// Settings modal control IDs
#define IDC_SET_CONV_EDIT        3001
#define IDC_SET_CONV_BROWSE      3002
#define IDC_SET_RECV_EDIT        3003
#define IDC_SET_RECV_BROWSE      3004
#define IDC_SET_BIND_COMBO       3005
#define IDC_SET_DEVNAME_EDIT     3006
#define IDC_SET_THEME_COMBO      3007
#define IDC_SET_SYNC_COMBO       3008
#define IDC_SET_MINTRAY_CHK      3009
#define IDC_SET_NOTIFY_CHK       3010
#define IDC_SET_STARTHIDDEN_CHK  3011
#define IDC_SET_SAVE_BTN         3012
#define IDC_SET_CANCEL_BTN       3013
#define IDC_SET_SKIPDEL_CHK      3014
#define IDC_SET_MINTRAY_LBL      3015
#define IDC_SET_NOTIFY_LBL       3016
#define IDC_SET_STARTHIDDEN_LBL  3017
#define IDC_SET_SKIPDEL_LBL      3018
#define IDC_SET_LANG_COMBO       3019

// Forward declarations
void OnSendMessage(HWND hwnd);
void ShowSettingsDialog(HWND parent);
static void ApplySearchFilter();
static void CloseEmojiPicker();

static void ApplyComposerTheme() {
    if (!g_hwndComposerEdit || !IsWindow(g_hwndComposerEdit)) return;
    const auto& colors = ThemeManager::Instance().GetColors();
    if (g_composerIsRichEdit) {
        SendMessageW(g_hwndComposerEdit, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(colors.win32BgColor));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        cf.crTextColor = colors.win32TextColor;
        cf.yHeight = 200; // 10pt
        wcsncpy(cf.szFaceName, L"Segoe UI", LF_FACESIZE - 1);
        cf.szFaceName[LF_FACESIZE - 1] = L'\0';
        SendMessageW(g_hwndComposerEdit, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
        SendMessageW(g_hwndComposerEdit, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
    }
    InvalidateRect(g_hwndComposerEdit, nullptr, TRUE);
}

// =========================================================================
// Direct2D Color Emoji Picker Popup
// =========================================================================
namespace {

    struct EmojiCategory {
        const wchar_t* tabIcon;
        const wchar_t* items[32];
    };

    const EmojiCategory kEmojiCategories[] = {
        {
            L"\U0001F600",
            {
                L"\U0001F600", L"\U0001F603", L"\U0001F604", L"\U0001F601", L"\U0001F606", L"\U0001F605", L"\U0001F923", L"\U0001F602",
                L"\U0001F642", L"\U0001F609", L"\U0001F60A", L"\U0001F607", L"\U0001F970", L"\U0001F60D", L"\U0001F929", L"\U0001F618",
                L"\U0001F61C", L"\U0001F61D", L"\U0001F914", L"\U0001F928", L"\U0001F610", L"\U0001F644", L"\U0001F60F", L"\U0001F60C",
                L"\U0001F614", L"\U0001F62A", L"\U0001F634", L"\U0001F60E", L"\U0001F973", L"\U0001F622", L"\U0001F62D", L"\U0001F621"
            }
        },
        {
            L"\U0001F44D",
            {
                L"\U0001F44D", L"\U0001F44E", L"\U0001F44C", L"\u270C\uFE0F", L"\U0001F91E", L"\U0001F91F", L"\U0001F919", L"\U0001F44B",
                L"\U0001F44F", L"\U0001F64C", L"\U0001F64F", L"\U0001F91D", L"\U0001F4AA", L"\U0001F448", L"\U0001F449", L"\U0001F446",
                L"\U0001F447", L"\u261D\uFE0F", L"\u270A", L"\U0001F44A", L"\U0001F91B", L"\U0001F91C", L"\U0001F590\uFE0F", L"\u270B",
                L"\U0001F64B", L"\U0001F926", L"\U0001F937", L"\U0001F440", L"\U0001F495", L"\U0001F48B", L"\U0001F4AC", L"\U0001F4AD"
            }
        },
        {
            L"\u2764\uFE0F",
            {
                L"\u2764\uFE0F", L"\U0001F9E1", L"\U0001F49B", L"\U0001F49A", L"\U0001F499", L"\U0001F49C", L"\U0001F5A4", L"\U0001F90D",
                L"\U0001F494", L"\u2763\uFE0F", L"\U0001F496", L"\U0001F497", L"\U0001F493", L"\U0001F49E", L"\U0001F49D", L"\U0001F525",
                L"\u2728", L"\u2B50", L"\U0001F31F", L"\U0001F4AB", L"\U0001F4A5", L"\u2705", L"\u274C", L"\u2757",
                L"\u2753", L"\U0001F4AF", L"\U0001F514", L"\U0001F4A1", L"\U0001F308", L"\u2600\uFE0F", L"\U0001F319", L"\u26A1"
            }
        },
        {
            L"\U0001F389",
            {
                L"\U0001F389", L"\U0001F38A", L"\U0001F381", L"\U0001F382", L"\U0001F388", L"\U0001F3C6", L"\U0001F947", L"\U0001F3B5",
                L"\U0001F3B6", L"\U0001F37B", L"\U0001F377", L"\U0001F942", L"\u2615", L"\U0001F355", L"\U0001F354", L"\U0001F35F",
                L"\U0001F32E", L"\U0001F363", L"\U0001F370", L"\U0001F36B", L"\U0001F36A", L"\U0001F369", L"\U0001F34E", L"\U0001F353",
                L"\U0001F339", L"\U0001F33B", L"\U0001F335", L"\U0001F436", L"\U0001F431", L"\U0001F43B", L"\U0001F98A", L"\U0001F427"
            }
        },
        {
            L"\U0001F697",
            {
                L"\U0001F697", L"\U0001F695", L"\U0001F68C", L"\U0001F686", L"\u2708\uFE0F", L"\U0001F680", L"\U0001F6B2", L"\U0001F3E0",
                L"\U0001F3E2", L"\U0001F3D6\uFE0F", L"\U0001F30D", L"\U0001F4F1", L"\U0001F4BB", L"\u231A", L"\U0001F4F7", L"\U0001F3AC",
                L"\U0001F3AE", L"\U0001F512", L"\U0001F511", L"\U0001F4CC", L"\U0001F4CE", L"\U0001F4C5", L"\u23F0", L"\U0001F4E9",
                L"\U0001F4B0", L"\U0001F4B3", L"\U0001F6E0\uFE0F", L"\U0001F3AF", L"\u26BD", L"\U0001F3C0", L"\U0001F3BE", L"\U0001F3C3"
            }
        }
    };

    constexpr int EMOJI_CAT_COUNT = static_cast<int>(sizeof(kEmojiCategories) / sizeof(kEmojiCategories[0]));
    constexpr int EMOJI_COLS = 8;
    constexpr int EMOJI_ROWS = 4;
    constexpr int EMOJI_CELL_W = 38;
    constexpr int EMOJI_CELL_H = 36;
    constexpr int EMOJI_PAD_X = 8;
    constexpr int EMOJI_HEADER_H = 26;
    constexpr int EMOJI_TABS_H = 32;
    constexpr int EMOJI_POPUP_W = EMOJI_PAD_X * 2 + EMOJI_COLS * EMOJI_CELL_W;
    constexpr int EMOJI_POPUP_H = EMOJI_HEADER_H + EMOJI_TABS_H + EMOJI_ROWS * EMOJI_CELL_H + 8;

    struct EmojiPopupState {
        HWND hwnd = nullptr;
        int activeCategory = 0;
        int hoveredCell = -1;
        int hoveredTab = -1;
        bool closeHovered = false;
        bool trackingLeave = false;
        ID2D1Factory1* d2dFactory = nullptr;
        IDWriteFactory2* dwriteFactory = nullptr;
        IDWriteTextFormat* emojiFont = nullptr;
        IDWriteTextFormat* tabFont = nullptr;
        IDWriteTextFormat* titleFont = nullptr;
        ID2D1HwndRenderTarget* rt = nullptr;
    };

    EmojiPopupState g_emojiPopup;
    bool g_emojiClassRegistered = false;

    template<class T>
    void SafeReleaseLocal(T** pp) {
        if (*pp) {
            (*pp)->Release();
            *pp = nullptr;
        }
    }

    void ReleaseEmojiPopupResources() {
        SafeReleaseLocal(&g_emojiPopup.rt);
        SafeReleaseLocal(&g_emojiPopup.emojiFont);
        SafeReleaseLocal(&g_emojiPopup.tabFont);
        SafeReleaseLocal(&g_emojiPopup.titleFont);
        SafeReleaseLocal(&g_emojiPopup.dwriteFactory);
        SafeReleaseLocal(&g_emojiPopup.d2dFactory);
    }

    bool EnsureEmojiPopupRenderResources(HWND hwnd) {
        if (!g_emojiPopup.d2dFactory) {
            D2D1_FACTORY_OPTIONS opts{};
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, reinterpret_cast<void**>(&g_emojiPopup.d2dFactory)))) {
                return false;
            }
        }
        if (!g_emojiPopup.dwriteFactory) {
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(&g_emojiPopup.dwriteFactory)))) {
                return false;
            }
        }
        if (!g_emojiPopup.emojiFont) {
            g_emojiPopup.dwriteFactory->CreateTextFormat(
                L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &g_emojiPopup.emojiFont
            );
            if (g_emojiPopup.emojiFont) {
                g_emojiPopup.emojiFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_emojiPopup.emojiFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (!g_emojiPopup.tabFont) {
            g_emojiPopup.dwriteFactory->CreateTextFormat(
                L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &g_emojiPopup.tabFont
            );
            if (g_emojiPopup.tabFont) {
                g_emojiPopup.tabFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                g_emojiPopup.tabFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (!g_emojiPopup.titleFont) {
            g_emojiPopup.dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &g_emojiPopup.titleFont
            );
            if (g_emojiPopup.titleFont) {
                g_emojiPopup.titleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        if (!g_emojiPopup.rt && hwnd) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left), static_cast<UINT32>(rc.bottom - rc.top));
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(hwnd, sz);
            if (FAILED(g_emojiPopup.d2dFactory->CreateHwndRenderTarget(props, hwndProps, &g_emojiPopup.rt))) {
                return false;
            }
        }
        return g_emojiPopup.rt != nullptr;
    }

    LRESULT CALLBACK EmojiPopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_ERASEBKGND:
                return 1;

            case WM_MOUSEMOVE: {
                if (!g_emojiPopup.trackingLeave) {
                    TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    g_emojiPopup.trackingLeave = true;
                }
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                bool newClose = (x >= EMOJI_POPUP_W - 28 && y < EMOJI_HEADER_H);
                int newTab = -1;
                if (y >= EMOJI_HEADER_H && y < EMOJI_HEADER_H + EMOJI_TABS_H) {
                    int tabW = (EMOJI_POPUP_W - EMOJI_PAD_X * 2) / EMOJI_CAT_COUNT;
                    int tx = (x - EMOJI_PAD_X) / std::max(1, tabW);
                    if (x >= EMOJI_PAD_X && tx >= 0 && tx < EMOJI_CAT_COUNT) {
                        newTab = tx;
                    }
                }
                int newCell = -1;
                int gridTop = EMOJI_HEADER_H + EMOJI_TABS_H;
                if (x >= EMOJI_PAD_X && x < EMOJI_PAD_X + EMOJI_COLS * EMOJI_CELL_W &&
                    y >= gridTop && y < gridTop + EMOJI_ROWS * EMOJI_CELL_H) {
                    int col = (x - EMOJI_PAD_X) / EMOJI_CELL_W;
                    int row = (y - gridTop) / EMOJI_CELL_H;
                    int idx = row * EMOJI_COLS + col;
                    if (idx >= 0 && idx < 32) {
                        newCell = idx;
                    }
                }

                if (newClose != g_emojiPopup.closeHovered ||
                    newTab != g_emojiPopup.hoveredTab ||
                    newCell != g_emojiPopup.hoveredCell) {
                    g_emojiPopup.closeHovered = newClose;
                    g_emojiPopup.hoveredTab = newTab;
                    g_emojiPopup.hoveredCell = newCell;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            case WM_MOUSELEAVE: {
                g_emojiPopup.trackingLeave = false;
                g_emojiPopup.closeHovered = false;
                g_emojiPopup.hoveredTab = -1;
                g_emojiPopup.hoveredCell = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONUP: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                if (x >= EMOJI_POPUP_W - 28 && y < EMOJI_HEADER_H) {
                    CloseEmojiPicker();
                    return 0;
                }
                if (y >= EMOJI_HEADER_H && y < EMOJI_HEADER_H + EMOJI_TABS_H) {
                    int tabW = (EMOJI_POPUP_W - EMOJI_PAD_X * 2) / EMOJI_CAT_COUNT;
                    int tx = (x - EMOJI_PAD_X) / std::max(1, tabW);
                    if (x >= EMOJI_PAD_X && tx >= 0 && tx < EMOJI_CAT_COUNT) {
                        g_emojiPopup.activeCategory = tx;
                        g_emojiPopup.hoveredCell = -1;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                int gridTop = EMOJI_HEADER_H + EMOJI_TABS_H;
                if (x >= EMOJI_PAD_X && x < EMOJI_PAD_X + EMOJI_COLS * EMOJI_CELL_W &&
                    y >= gridTop && y < gridTop + EMOJI_ROWS * EMOJI_CELL_H) {
                    int col = (x - EMOJI_PAD_X) / EMOJI_CELL_W;
                    int row = (y - gridTop) / EMOJI_CELL_H;
                    int idx = row * EMOJI_COLS + col;
                    if (idx >= 0 && idx < 32 && g_hwndComposerEdit && IsWindow(g_hwndComposerEdit)) {
                        const wchar_t* emoji = kEmojiCategories[g_emojiPopup.activeCategory].items[idx];
                        if (emoji) {
                            SendMessageW(g_hwndComposerEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(emoji));
                            SetFocus(g_hwndComposerEdit);
                        }
                    }
                }
                return 0;
            }

            case WM_PAINT: {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                if (EnsureEmojiPopupRenderResources(hwnd)) {
                    const ThemeColors& colors = ThemeManager::Instance().GetColors();
                    g_emojiPopup.rt->BeginDraw();
                    g_emojiPopup.rt->Clear(colors.sidebarBackground);

                    ID2D1SolidColorBrush* brBorder = nullptr;
                    ID2D1SolidColorBrush* brHover = nullptr;
                    ID2D1SolidColorBrush* brSel = nullptr;
                    ID2D1SolidColorBrush* brText = nullptr;
                    ID2D1SolidColorBrush* brMuted = nullptr;
                    ID2D1SolidColorBrush* brAccent = nullptr;
                    g_emojiPopup.rt->CreateSolidColorBrush(colors.dividerColor, &brBorder);
                    g_emojiPopup.rt->CreateSolidColorBrush(colors.sidebarHover, &brHover);
                    g_emojiPopup.rt->CreateSolidColorBrush(colors.sidebarSelected, &brSel);
                    g_emojiPopup.rt->CreateSolidColorBrush(colors.textPrimary, &brText);
                    g_emojiPopup.rt->CreateSolidColorBrush(colors.textSecondary, &brMuted);
                    g_emojiPopup.rt->CreateSolidColorBrush(colors.accent, &brAccent);

                    // Outer border
                    g_emojiPopup.rt->DrawRectangle(
                        D2D1::RectF(0.5f, 0.5f, EMOJI_POPUP_W - 0.5f, EMOJI_POPUP_H - 0.5f),
                        brBorder, 1.0f
                    );

                    // Header text + Close button
                    std::wstring headerTitle = Tr("emoji_picker_title");
                    if (g_emojiPopup.titleFont && brMuted) {
                        g_emojiPopup.rt->DrawText(
                            headerTitle.c_str(),
                            static_cast<UINT32>(headerTitle.size()),
                            g_emojiPopup.titleFont,
                            D2D1::RectF(10.0f, 2.0f, EMOJI_POPUP_W - 30.0f, static_cast<float>(EMOJI_HEADER_H)),
                            brMuted
                        );
                        D2D1_RECT_F closeRc = D2D1::RectF(EMOJI_POPUP_W - 26.0f, 2.0f, EMOJI_POPUP_W - 4.0f, static_cast<float>(EMOJI_HEADER_H));
                        if (g_emojiPopup.closeHovered && brHover) {
                            g_emojiPopup.rt->FillRoundedRectangle(D2D1::RoundedRect(closeRc, 4.0f, 4.0f), brHover);
                        }
                        g_emojiPopup.rt->DrawText(L"\u00D7", 1, g_emojiPopup.tabFont ? g_emojiPopup.tabFont : g_emojiPopup.titleFont, closeRc, g_emojiPopup.closeHovered ? brText : brMuted);
                    }

                    // Category tabs
                    float tabW = static_cast<float>(EMOJI_POPUP_W - EMOJI_PAD_X * 2) / static_cast<float>(EMOJI_CAT_COUNT);
                    for (int t = 0; t < EMOJI_CAT_COUNT; ++t) {
                        float left = static_cast<float>(EMOJI_PAD_X) + static_cast<float>(t) * tabW;
                        D2D1_RECT_F tabRc = D2D1::RectF(left + 2.0f, static_cast<float>(EMOJI_HEADER_H) + 2.0f, left + tabW - 2.0f, static_cast<float>(EMOJI_HEADER_H + EMOJI_TABS_H) - 2.0f);
                        if (t == g_emojiPopup.activeCategory && brSel) {
                            g_emojiPopup.rt->FillRoundedRectangle(D2D1::RoundedRect(tabRc, 6.0f, 6.0f), brSel);
                            if (brAccent) {
                                g_emojiPopup.rt->FillRectangle(D2D1::RectF(tabRc.left + 8.0f, tabRc.bottom - 2.5f, tabRc.right - 8.0f, tabRc.bottom), brAccent);
                            }
                        } else if (t == g_emojiPopup.hoveredTab && brHover) {
                            g_emojiPopup.rt->FillRoundedRectangle(D2D1::RoundedRect(tabRc, 6.0f, 6.0f), brHover);
                        }
                        if (g_emojiPopup.tabFont && brText) {
                            const wchar_t* ico = kEmojiCategories[t].tabIcon;
                            g_emojiPopup.rt->DrawText(
                                ico, static_cast<UINT32>(wcslen(ico)),
                                g_emojiPopup.tabFont, tabRc, brText,
                                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
                            );
                        }
                    }

                    // Separator line
                    float gridTop = static_cast<float>(EMOJI_HEADER_H + EMOJI_TABS_H);
                    if (brBorder) {
                        g_emojiPopup.rt->DrawLine(
                            D2D1::Point2F(6.0f, gridTop),
                            D2D1::Point2F(static_cast<float>(EMOJI_POPUP_W) - 6.0f, gridTop),
                            brBorder, 1.0f
                        );
                    }

                    // Emoji grid
                    for (int i = 0; i < 32; ++i) {
                        int r = i / EMOJI_COLS;
                        int c = i % EMOJI_COLS;
                        float cx = static_cast<float>(EMOJI_PAD_X + c * EMOJI_CELL_W);
                        float cy = gridTop + static_cast<float>(r * EMOJI_CELL_H) + 2.0f;
                        D2D1_RECT_F cellRc = D2D1::RectF(cx + 1.0f, cy + 1.0f, cx + EMOJI_CELL_W - 1.0f, cy + EMOJI_CELL_H - 1.0f);
                        if (i == g_emojiPopup.hoveredCell && brHover) {
                            g_emojiPopup.rt->FillRoundedRectangle(D2D1::RoundedRect(cellRc, 6.0f, 6.0f), brHover);
                        }
                        if (g_emojiPopup.emojiFont && brText) {
                            const wchar_t* item = kEmojiCategories[g_emojiPopup.activeCategory].items[i];
                            g_emojiPopup.rt->DrawText(
                                item, static_cast<UINT32>(wcslen(item)),
                                g_emojiPopup.emojiFont, cellRc, brText,
                                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
                            );
                        }
                    }

                    SafeReleaseLocal(&brBorder);
                    SafeReleaseLocal(&brHover);
                    SafeReleaseLocal(&brSel);
                    SafeReleaseLocal(&brText);
                    SafeReleaseLocal(&brMuted);
                    SafeReleaseLocal(&brAccent);

                    HRESULT hr = g_emojiPopup.rt->EndDraw();
                    if (hr == D2DERR_RECREATE_TARGET) {
                        SafeReleaseLocal(&g_emojiPopup.rt);
                    }
                }
                EndPaint(hwnd, &ps);
                return 0;
            }

            case WM_DESTROY: {
                ReleaseEmojiPopupResources();
                g_emojiPopup.hwnd = nullptr;
                g_emojiPopup.hoveredCell = -1;
                g_emojiPopup.hoveredTab = -1;
                g_emojiPopup.closeHovered = false;
                g_emojiPopup.trackingLeave = false;
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

} // namespace

static void CloseEmojiPicker() {
    if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
        DestroyWindow(g_emojiPopup.hwnd);
        g_emojiPopup.hwnd = nullptr;
    }
}

static void PositionEmojiPicker(HWND parent) {
    if (!g_emojiPopup.hwnd || !IsWindow(g_emojiPopup.hwnd) || !parent) return;
    RECT btnRc{};
    if (g_hwndEmojiButton && GetWindowRect(g_hwndEmojiButton, &btnRc)) {
        int x = btnRc.left - 20;
        int y = btnRc.top - EMOJI_POPUP_H - 6;
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            if (x + EMOJI_POPUP_W > wa.right - 8) x = wa.right - EMOJI_POPUP_W - 8;
            if (x < wa.left + 8) x = wa.left + 8;
            if (y < wa.top + 8) y = btnRc.bottom + 6;
        }
        SetWindowPos(g_emojiPopup.hwnd, HWND_TOPMOST, x, y, EMOJI_POPUP_W, EMOJI_POPUP_H, SWP_NOACTIVATE);
    }
}

static void ToggleEmojiPicker(HWND parent) {
    if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
        CloseEmojiPicker();
        return;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* kEmojiPopupClass = L"KDEConnectEmojiPopup";
    if (!g_emojiClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = EmojiPopupWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kEmojiPopupClass;
        if (RegisterClassExW(&wc)) {
            g_emojiClassRegistered = true;
        }
    }
    if (!g_emojiClassRegistered) return;

    HWND hPopup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kEmojiPopupClass,
        L"",
        WS_POPUP,
        0, 0, EMOJI_POPUP_W, EMOJI_POPUP_H,
        parent, nullptr, hInst, nullptr
    );
    if (!hPopup) return;
    g_emojiPopup.hwnd = hPopup;

    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hPopup, 33, &cornerPref, sizeof(cornerPref));

    PositionEmojiPicker(parent);
    ShowWindow(hPopup, SW_SHOWNOACTIVATE);
    UpdateWindow(hPopup);
}

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

static void UpdateMainWindowTitle(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    std::wstring base = Tr("app_title_base");
    std::wstring endpoint = SmsDaemon::Instance().GetBoundEndpointSummary();
    std::wstring bindErr = SmsDaemon::Instance().GetLastBindError();
    std::wstring title = base;
    if (!bindErr.empty()) {
        title += L" " + Tr("title_net_error");
    } else if (SmsDaemon::Instance().IsClientMode()) {
        title += endpoint.empty() ? L" [Daemon IPC]" : (L" [IPC - " + endpoint + L"]");
    } else if (!endpoint.empty()) {
        title += L" [" + endpoint + L"]";
    }
    SetWindowTextW(hwnd, title.c_str());
}

static int GetConnectionIndicatorWidth(bool isConnected, int batteryLevel, bool isCharging) {
    bool hasBattery = (isConnected && batteryLevel >= 0 && batteryLevel <= 100);
    if (!hasBattery) return 34;
    if (isCharging) return (batteryLevel >= 100) ? 94 : 88;
    return (batteryLevel >= 100) ? 84 : 78;
}

static RECT GetConnectionIndicatorRect(HWND hwnd) {
    RECT rc{};
    if (hwnd && IsWindow(hwnd)) {
        GetClientRect(hwnd, &rc);
    }
    int width = rc.right - rc.left;
    int badgeW = GetConnectionIndicatorWidth(
        SmsDaemon::Instance().IsPhoneConnected(),
        SmsDaemon::Instance().GetPhoneBatteryLevel(),
        SmsDaemon::Instance().IsPhoneCharging()
    );
    return RECT{ width - 208 - badgeW, 11, width - 208, 43 };
}

static std::wstring BuildConnectionTooltipText() {
    if (SmsDaemon::Instance().IsPhoneConnected()) {
        std::wstring devName = SmsDaemon::Instance().GetConnectedDeviceName();
        std::wstring text = !devName.empty()
            ? TrFmt("phone_status_connected_named", devName)
            : Tr("phone_status_connected");
        int batt = SmsDaemon::Instance().GetPhoneBatteryLevel();
        if (batt >= 0 && batt <= 100) {
            std::wstring pctStr = std::to_wstring(batt);
            text += L" \u2014 " + (SmsDaemon::Instance().IsPhoneCharging()
                ? TrFmt("battery_pct_charging", pctStr)
                : TrFmt("battery_pct", pctStr));
        }
        return text;
    }
    return Tr("phone_status_disconnected");
}

static void UpdateConnectionTooltip(HWND hwnd) {
    if (!g_hwndConnTooltip || !IsWindow(g_hwndConnTooltip) || !hwnd) return;
    std::wstring tipText = BuildConnectionTooltipText();
    RECT badgeRc = GetConnectionIndicatorRect(hwnd);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(TOOLINFOW);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = hwnd;
    ti.uId = 1;
    ti.rect = badgeRc;
    ti.lpszText = const_cast<LPWSTR>(tipText.c_str());
    SendMessageW(g_hwndConnTooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(g_hwndConnTooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

static void RefreshConnectionUi(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    g_lastPhoneConnected = SmsDaemon::Instance().IsPhoneConnected();
    g_lastPhoneName = SmsDaemon::Instance().GetConnectedDeviceName();
    g_lastBatteryLevel = SmsDaemon::Instance().GetPhoneBatteryLevel();
    g_lastBatteryCharging = SmsDaemon::Instance().IsPhoneCharging();
    UpdateMainWindowTitle(hwnd);
    UpdateConnectionTooltip(hwnd);
    if (g_allConversations.empty()) {
        g_lastEmptyStatus = SmsDaemon::Instance().GetConnectionStatusText();
        InvalidateRect(hwnd, nullptr, FALSE);
    } else {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        RECT headerOnly{ static_cast<LONG>(g_sidebarWidth), 0, rc.right, 54 };
        InvalidateRect(hwnd, &headerOnly, FALSE);
    }
}

static void ApplyLanguageToMainUi(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (g_hwndSearchEdit) {
        std::wstring ph = Tr("search_placeholder");
        SendMessageW(g_hwndSearchEdit, 0x1501 /* EM_SETCUEBANNER */, FALSE, reinterpret_cast<LPARAM>(ph.c_str()));
    }
    if (g_hwndSendButton) {
        SetWindowTextW(g_hwndSendButton, Tr("btn_send").c_str());
    }
    if (g_hwndSettingsButton) {
        SetWindowTextW(g_hwndSettingsButton, Tr("btn_settings").c_str());
    }
    if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
        InvalidateRect(g_emojiPopup.hwnd, nullptr, FALSE);
    }
    UpdateMainWindowTitle(hwnd);
    UpdateConnectionTooltip(hwnd);
    g_lastEmptyStatus = SmsDaemon::Instance().GetConnectionStatusText();
    ApplySearchFilter();
    InvalidateRect(hwnd, nullptr, FALSE);
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

static std::wstring ExtractDigitsLocal(const std::wstring& s) {
    std::wstring digits;
    for (wchar_t ch : s) {
        if (ch >= L'0' && ch <= L'9') digits.push_back(ch);
    }
    return digits;
}

static bool IsMultiRecipientAddressLocal(const std::wstring& address) {
    return address.find(L',') != std::wstring::npos || address.find(L';') != std::wstring::npos;
}

static std::wstring TrimWideLocal(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) {
        s.pop_back();
    }
    return s;
}

static std::wstring CanonicalSinglePhoneKeyLocal(const std::wstring& phone) {
    std::wstring trimmed = TrimWideLocal(phone);
    if (trimmed.empty() || IsMultiRecipientAddressLocal(trimmed)) return L"";

    std::wstring check = trimmed;
    if (check.size() > 4 && ToLowerCopy(check.substr(0, 4)) == L"tel:") {
        check = TrimWideLocal(check.substr(4));
    }

    bool hasAlpha = false;
    for (wchar_t ch : check) {
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || ::iswalpha(ch)) {
            hasAlpha = true;
            break;
        }
    }

    if (!hasAlpha) {
        std::wstring digits = ExtractDigitsLocal(check);
        if (!digits.empty()) {
            if (digits.size() >= 8) {
                return L"suf:" + ((digits.size() > 9) ? digits.substr(digits.size() - 9) : digits);
            }
            return L"exact:" + digits;
        }
    }
    return L"text:" + ToLowerCopy(check);
}

static bool ContactsSnapshotEqual(const std::vector<ContactSummary>& a, const std::vector<ContactSummary>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].phone != b[i].phone || a[i].avatarPath != b[i].avatarPath) {
            return false;
        }
    }
    return true;
}

static bool ConversationsSnapshotEqual(const std::vector<Conversation>& a, const std::vector<Conversation>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].threadId != b[i].threadId ||
            a[i].lastTimestamp != b[i].lastTimestamp ||
            a[i].messages.size() != b[i].messages.size() ||
            a[i].contactName != b[i].contactName ||
            a[i].avatarPath != b[i].avatarPath ||
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
    std::wstring qDigits = ExtractDigitsLocal(g_searchFilter);

    // Map all existing single-recipient conversations by canonical phone key
    // so searching a contact never opens or creates a duplicate synthetic conversation.
    std::map<std::wstring, size_t> convIndexByPhone;
    for (size_t i = 0; i < g_allConversations.size(); ++i) {
        std::wstring key = CanonicalSinglePhoneKeyLocal(g_allConversations[i].address);
        if (!key.empty() && convIndexByPhone.find(key) == convIndexByPhone.end()) {
            convIndexByPhone[key] = i;
        }
    }

    std::set<int64_t> visibleThreadIds;
    std::set<std::wstring> coveredContactPhones;

    for (const auto& conv : g_allConversations) {
        bool matched = (ToLowerCopy(conv.contactName).find(q) != std::wstring::npos ||
                        ToLowerCopy(conv.address).find(q) != std::wstring::npos ||
                        ToLowerCopy(conv.snippet).find(q) != std::wstring::npos);

        if (!matched && qDigits.size() >= 3) {
            if (ExtractDigitsLocal(conv.address).find(qDigits) != std::wstring::npos) {
                matched = true;
            }
        }
        if (!matched) {
            for (const auto& m : conv.messages) {
                if (ToLowerCopy(m.body).find(q) != std::wstring::npos) {
                    matched = true;
                    break;
                }
            }
        }

        if (matched) {
            g_visibleConversations.push_back(conv);
            visibleThreadIds.insert(conv.threadId);
        }
    }

    // Append matching phonebook contacts not already linked to any existing conversation thread
    for (size_t i = 0; i < g_allContacts.size(); ++i) {
        const auto& c = g_allContacts[i];
        bool matched = (ToLowerCopy(c.name).find(q) != std::wstring::npos ||
                        ToLowerCopy(c.phone).find(q) != std::wstring::npos);
        if (!matched && qDigits.size() >= 3) {
            if (ExtractDigitsLocal(c.phone).find(qDigits) != std::wstring::npos) {
                matched = true;
            }
        }
        if (!matched) continue;

        std::wstring key = CanonicalSinglePhoneKeyLocal(c.phone);
        if (!key.empty()) {
            auto existingIt = convIndexByPhone.find(key);
            if (existingIt != convIndexByPhone.end()) {
                const Conversation& existingConv = g_allConversations[existingIt->second];
                if (visibleThreadIds.find(existingConv.threadId) == visibleThreadIds.end()) {
                    g_visibleConversations.push_back(existingConv);
                    visibleThreadIds.insert(existingConv.threadId);
                }
                continue;
            }
            if (coveredContactPhones.find(key) != coveredContactPhones.end()) {
                continue;
            }
            coveredContactPhones.insert(key);
        }

        Conversation contactEntry;
        contactEntry.threadId = -100000 - static_cast<int64_t>(i);
        contactEntry.contactName = c.name;
        contactEntry.address = c.phone;
        contactEntry.avatarPath = c.avatarPath;
        contactEntry.snippet = TrFmt("contact_prefix", c.phone);
        contactEntry.lastTimestamp = 0;
        g_visibleConversations.push_back(std::move(contactEntry));
    }
}

static void ReloadConversations(HWND hwnd, bool autoScrollIfNewOrActive, bool forceRepaint = false) {
    size_t prevSelectedCount = 0;
    std::wstring activePhoneKey;
    if (Conversation* prev = FindActiveConversation()) {
        prevSelectedCount = prev->messages.size();
        activePhoneKey = CanonicalSinglePhoneKeyLocal(prev->address);
    }

    std::vector<Conversation> fresh = SmsDaemon::Instance().LoadAllConversations();
    std::vector<ContactSummary> freshContacts = SmsDaemon::Instance().GetSyncedContacts();

    int64_t resolvedSelectedId = g_selectedThreadId;
    if (resolvedSelectedId > 0) {
        resolvedSelectedId = SmsDaemon::Instance().ResolveMergedThreadId(resolvedSelectedId);
    }
    if (resolvedSelectedId != g_selectedThreadId) {
        g_selectedThreadId = resolvedSelectedId;
        forceRepaint = true;
    }

    bool listChanged = !ConversationsSnapshotEqual(g_allConversations, fresh) ||
                       !ContactsSnapshotEqual(g_allContacts, freshContacts);
    if (!listChanged && !forceRepaint) {
        return;
    }

    g_allConversations = std::move(fresh);
    g_allContacts = std::move(freshContacts);

    // If the user had a synthetic contact selected and a real conversation now exists for that number, switch to it
    if (g_selectedThreadId <= -100000 && !activePhoneKey.empty()) {
        for (const auto& c : g_allConversations) {
            if (CanonicalSinglePhoneKeyLocal(c.address) == activePhoneKey) {
                g_selectedThreadId = c.threadId;
                break;
            }
        }
    }

    ApplySearchFilter();
    ClampSidebarScroll(hwnd);

    bool selectedExists = false;
    for (const auto& c : g_allConversations) {
        if (c.threadId == g_selectedThreadId) {
            selectedExists = true;
            break;
        }
    }
    if (!selectedExists && !activePhoneKey.empty()) {
        for (const auto& c : g_allConversations) {
            if (CanonicalSinglePhoneKeyLocal(c.address) == activePhoneKey) {
                g_selectedThreadId = c.threadId;
                selectedExists = true;
                break;
            }
        }
    }
    if (!selectedExists && g_selectedThreadId <= -100000) {
        for (const auto& c : g_visibleConversations) {
            if (c.threadId == g_selectedThreadId) {
                selectedExists = true;
                break;
            }
        }
    }

    bool selectionChanged = false;
    if (!selectedExists) {
        if (!g_visibleConversations.empty()) {
            g_selectedThreadId = g_visibleConversations.front().threadId;
            selectionChanged = true;
            if (g_selectedThreadId > 0) {
                SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
            }
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
    if (target.threadId <= 0) return;

    bool skipConfirm = (ReadIniInt(L"App", L"SkipDeleteConfirmation", 0) != 0);
    if (askConfirm && !skipConfirm) {
        std::wstring label = target.contactName.empty() ? target.address : target.contactName;
        std::wstring prompt = TrFmt("delete_confirm_prompt", label);
        if (MessageBoxW(hwnd, prompt.c_str(), Tr("delete_confirm_title").c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) {
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
    pfd->SetOkButtonLabel(Tr("folder_picker_ok").c_str());

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
    std::wstring msg = TrFmt("first_run_msg", GetSettingsIniPath(), defaultDir);

    std::wstring chosenDir = defaultDir;
    int res = MessageBoxW(nullptr, msg.c_str(), Tr("first_run_title").c_str(), MB_OKCANCEL | MB_ICONINFORMATION);
    if (res == IDOK) {
        std::wstring picked;
        if (PromptSelectStorageFolder(nullptr, Tr("first_run_picker_title"), GetAppDataDirectory(), picked)) {
            chosenDir = picked;
        }
    }

    SetConversationsDirectory(chosenDir);
    if (ReadIniString(L"Storage", L"ReceivedFilesDir", L"").empty()) {
        SetReceivedFilesDirectory(GetDefaultReceivedFilesDirectory());
    }
}

static void ConfigureSyncTimer(HWND hwnd) {
    if (!hwnd) return;
    KillTimer(hwnd, IDC_SYNC_TIMER);
    int intervalSec = ReadIniInt(L"App", L"AutoSyncIntervalSec", 5);
    if (intervalSec > 0) {
        intervalSec = std::clamp(intervalSec, 2, 3600);
        SetTimer(hwnd, IDC_SYNC_TIMER, static_cast<UINT>(intervalSec * 1000), nullptr);
    }
}

struct SettingsModalState {
    HWND hwnd = nullptr;
    HWND convEdit = nullptr;
    HWND recvEdit = nullptr;
    HWND bindCombo = nullptr;
    HWND devNameEdit = nullptr;
    HWND themeCombo = nullptr;
    HWND langCombo = nullptr;
    HWND syncCombo = nullptr;
    HWND minTrayChk = nullptr;
    HWND notifyChk = nullptr;
    HWND startHiddenChk = nullptr;
    HWND skipDeleteChk = nullptr;

    std::vector<std::wstring> bindIpValues;
    std::vector<int> syncSecValues;
    int selectedBindIdx = 0;

    std::wstring resConvDir;
    std::wstring resRecvDir;
    std::wstring resBindIp;
    std::wstring resDeviceName;
    ThemeMode resThemeMode = ThemeMode::System;
    AppLanguage resLanguage = AppLanguage::Auto;
    int resSyncSec = 5;
    int resMinToTray = 1;
    int resEnableNotifs = 1;
    int resStartHidden = 0;
    int resSkipDeleteConfirm = 0;

    HBRUSH bgBrush = nullptr;
    HBRUSH inputBrush = nullptr;
    bool saved = false;
    bool closed = false;
};

static std::wstring GetWindowTextSafe(HWND hCtrl) {
    if (!hCtrl) return L"";
    int len = GetWindowTextLengthW(hCtrl);
    if (len <= 0) return L"";
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hCtrl, &text[0], len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

struct DarkComboState {
    WNDPROC origProc = nullptr;
    bool hovered = false;
    bool tracking = false;
};

struct DarkComboListState {
    WNDPROC origProc = nullptr;
};

static const wchar_t* kPropComboState = L"KDEComboState";
static const wchar_t* kPropComboListState = L"KDEComboListState";

static LRESULT CALLBACK DarkComboBoxProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DarkComboState* cs = reinterpret_cast<DarkComboState*>(GetPropW(hwnd, kPropComboState));
    WNDPROC origProc = cs ? cs->origProc : nullptr;

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (cs) {
                if (!cs->tracking) {
                    TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    cs->tracking = true;
                }
                if (!cs->hovered) {
                    cs->hovered = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            break;
        }

        case WM_MOUSELEAVE: {
            if (cs) {
                cs->tracking = false;
                if (cs->hovered) {
                    cs->hovered = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            break;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE: {
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            if (w > 0 && h > 0) {
                HDC memDc = CreateCompatibleDC(hdc);
                HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

                const auto& colors = ThemeManager::Instance().GetColors();
                bool isDark = ThemeManager::Instance().IsDarkActive();
                bool isFocused = (GetFocus() == hwnd);
                bool isDropped = (SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0);
                bool isEnabled = (IsWindowEnabled(hwnd) != FALSE);
                bool isHovered = (cs && cs->hovered);

                HBRUSH outerBr = CreateSolidBrush(colors.win32BgColor);
                FillRect(memDc, &rc, outerBr);
                DeleteObject(outerBr);

                COLORREF fillBg = isDark
                    ? ((isHovered || isDropped) ? RGB(38, 51, 73) : RGB(30, 41, 59))
                    : ((isHovered || isDropped) ? RGB(248, 250, 252) : RGB(255, 255, 255));
                COLORREF borderCol = (isFocused || isDropped)
                    ? RGB(59, 130, 246)
                    : (isDark
                        ? (isHovered ? RGB(100, 116, 139) : RGB(61, 75, 98))
                        : (isHovered ? RGB(148, 163, 184) : RGB(203, 213, 225)));
                COLORREF textCol = isEnabled
                    ? colors.win32TextColor
                    : (isDark ? RGB(100, 116, 139) : RGB(148, 163, 184));

                HBRUSH boxBr = CreateSolidBrush(fillBg);
                HPEN boxPen = CreatePen(PS_SOLID, 1, borderCol);
                HGDIOBJ oldBr = SelectObject(memDc, boxBr);
                HGDIOBJ oldPen = SelectObject(memDc, boxPen);
                RoundRect(memDc, 0, 0, w, h, 8, 8);
                SelectObject(memDc, oldPen);
                SelectObject(memDc, oldBr);
                DeleteObject(boxPen);
                DeleteObject(boxBr);

                int sel = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
                if (sel != CB_ERR) {
                    int len = static_cast<int>(SendMessageW(hwnd, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0));
                    if (len > 0) {
                        std::wstring selText(static_cast<size_t>(len) + 1, L'\0');
                        SendMessageW(hwnd, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(&selText[0]));
                        selText.resize(static_cast<size_t>(len));

                        HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
                        HGDIOBJ oldFont = hFont ? SelectObject(memDc, hFont) : nullptr;
                        SetBkMode(memDc, TRANSPARENT);
                        SetTextColor(memDc, textCol);
                        RECT textRc{ 10, 0, std::max(14, w - 28), h };
                        DrawTextW(memDc, selText.c_str(), static_cast<int>(selText.size()), &textRc,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        if (oldFont) SelectObject(memDc, oldFont);
                    }
                }

                int cx = w - 15;
                int cy = h / 2;
                COLORREF chevCol = isDark ? RGB(203, 213, 225) : RGB(71, 85, 105);
                HPEN chevPen = CreatePen(PS_SOLID, 2, chevCol);
                HGDIOBJ oldChevPen = SelectObject(memDc, chevPen);
                MoveToEx(memDc, cx - 4, cy - 2, nullptr);
                LineTo(memDc, cx, cy + 2);
                LineTo(memDc, cx + 5, cy - 3);
                SelectObject(memDc, oldChevPen);
                DeleteObject(chevPen);

                BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
                SelectObject(memDc, oldBmp);
                DeleteObject(memBmp);
                DeleteDC(memDc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCDESTROY: {
            RemovePropW(hwnd, kPropComboState);
            if (origProc) {
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origProc));
            }
            delete cs;
            return origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
    return origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK DarkComboListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DarkComboListState* ls = reinterpret_cast<DarkComboListState*>(GetPropW(hwnd, kPropComboListState));
    WNDPROC origProc = ls ? ls->origProc : nullptr;

    if (msg == WM_NCPAINT) {
        LRESULT res = origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
        if (HDC hdcWin = GetWindowDC(hwnd)) {
            RECT wrc{};
            if (GetWindowRect(hwnd, &wrc)) {
                RECT frameRc{ 0, 0, wrc.right - wrc.left, wrc.bottom - wrc.top };
                bool isDark = ThemeManager::Instance().IsDarkActive();
                HBRUSH br = CreateSolidBrush(isDark ? RGB(71, 85, 105) : RGB(203, 213, 225));
                FrameRect(hdcWin, &frameRc, br);
                DeleteObject(br);
            }
            ReleaseDC(hwnd, hdcWin);
        }
        return res;
    }

    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, kPropComboListState);
        if (origProc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(origProc));
        }
        delete ls;
        return origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void AttachModernComboBox(HWND hCombo) {
    if (!hCombo || !IsWindow(hCombo)) return;
    SendMessageW(hCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 22);
    SendMessageW(hCombo, CB_SETITEMHEIGHT, 0, 26);

    if (!GetPropW(hCombo, kPropComboState)) {
        DarkComboState* cs = new DarkComboState{};
        cs->origProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hCombo, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DarkComboBoxProc))
        );
        SetPropW(hCombo, kPropComboState, cs);
    }

    COMBOBOXINFO cbi{};
    cbi.cbSize = sizeof(cbi);
    if (GetComboBoxInfo(hCombo, &cbi) && cbi.hwndList && IsWindow(cbi.hwndList)) {
        ThemeManager::Instance().ApplyToControl(cbi.hwndList);
        if (!GetPropW(cbi.hwndList, kPropComboListState)) {
            DarkComboListState* ls = new DarkComboListState{};
            ls->origProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(cbi.hwndList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(DarkComboListProc))
            );
            SetPropW(cbi.hwndList, kPropComboListState, ls);
        }
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsModalState* st = reinterpret_cast<SettingsModalState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }

        case WM_COMMAND: {
            if (!st) break;
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            auto toggleCheckbox = [](HWND hChk) {
                if (!hChk) return;
                LRESULT cur = SendMessageW(hChk, BM_GETCHECK, 0, 0);
                SendMessageW(hChk, BM_SETCHECK, (cur == BST_CHECKED) ? BST_UNCHECKED : BST_CHECKED, 0);
                SetFocus(hChk);
            };
            if (id == IDC_SET_MINTRAY_LBL && code == STN_CLICKED) {
                toggleCheckbox(st->minTrayChk);
                return 0;
            } else if (id == IDC_SET_NOTIFY_LBL && code == STN_CLICKED) {
                toggleCheckbox(st->notifyChk);
                return 0;
            } else if (id == IDC_SET_STARTHIDDEN_LBL && code == STN_CLICKED) {
                toggleCheckbox(st->startHiddenChk);
                return 0;
            } else if (id == IDC_SET_SKIPDEL_LBL && code == STN_CLICKED) {
                toggleCheckbox(st->skipDeleteChk);
                return 0;
            } else if ((id == IDC_SET_BIND_COMBO || id == IDC_SET_THEME_COMBO || id == IDC_SET_LANG_COMBO || id == IDC_SET_SYNC_COMBO) &&
                       (code == CBN_SELCHANGE || code == CBN_DROPDOWN || code == CBN_CLOSEUP)) {
                if (id == IDC_SET_BIND_COMBO && code == CBN_SELCHANGE) {
                    int sel = static_cast<int>(SendMessageW(st->bindCombo, CB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(st->bindIpValues.size())) {
                        st->selectedBindIdx = sel;
                        st->resBindIp = st->bindIpValues[static_cast<size_t>(sel)];
                    }
                }
                if (HWND hCombo = reinterpret_cast<HWND>(lParam)) {
                    InvalidateRect(hCombo, nullptr, FALSE);
                }
                return 0;
            } else if (id == IDC_SET_CONV_BROWSE) {
                std::wstring cur = GetWindowTextSafe(st->convEdit);
                std::wstring picked;
                if (PromptSelectStorageFolder(hwnd, Tr("picker_conv_title"), cur.empty() ? GetMessagesDirectory() : cur, picked)) {
                    SetWindowTextW(st->convEdit, picked.c_str());
                }
                return 0;
            } else if (id == IDC_SET_RECV_BROWSE) {
                std::wstring cur = GetWindowTextSafe(st->recvEdit);
                std::wstring picked;
                if (PromptSelectStorageFolder(hwnd, Tr("picker_recv_title"), cur.empty() ? GetReceivedFilesDirectory() : cur, picked)) {
                    SetWindowTextW(st->recvEdit, picked.c_str());
                }
                return 0;
            } else if (id == IDC_SET_SAVE_BTN || id == IDOK) {
                st->resConvDir = GetWindowTextSafe(st->convEdit);
                st->resRecvDir = GetWindowTextSafe(st->recvEdit);
                st->resDeviceName = GetWindowTextSafe(st->devNameEdit);

                int bindSel = static_cast<int>(SendMessageW(st->bindCombo, CB_GETCURSEL, 0, 0));
                if (bindSel == CB_ERR) {
                    bindSel = st->selectedBindIdx;
                }
                if (bindSel >= 0 && bindSel < static_cast<int>(st->bindIpValues.size())) {
                    st->resBindIp = st->bindIpValues[static_cast<size_t>(bindSel)];
                }

                int themeSel = static_cast<int>(SendMessageW(st->themeCombo, CB_GETCURSEL, 0, 0));
                if (themeSel >= 0 && themeSel <= 2) {
                    st->resThemeMode = static_cast<ThemeMode>(themeSel);
                }

                int langSel = static_cast<int>(SendMessageW(st->langCombo, CB_GETCURSEL, 0, 0));
                if (langSel >= 0 && langSel <= static_cast<int>(AppLanguage::Portuguese)) {
                    st->resLanguage = static_cast<AppLanguage>(langSel);
                }

                int syncSel = static_cast<int>(SendMessageW(st->syncCombo, CB_GETCURSEL, 0, 0));
                if (syncSel >= 0 && syncSel < static_cast<int>(st->syncSecValues.size())) {
                    st->resSyncSec = st->syncSecValues[static_cast<size_t>(syncSel)];
                }

                st->resMinToTray = (SendMessageW(st->minTrayChk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                st->resEnableNotifs = (SendMessageW(st->notifyChk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                st->resStartHidden = (SendMessageW(st->startHiddenChk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                st->resSkipDeleteConfirm = (SendMessageW(st->skipDeleteChk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

                st->saved = true;
                st->closed = true;
                DestroyWindow(hwnd);
                return 0;
            } else if (id == IDC_SET_CANCEL_BTN || id == IDCANCEL) {
                st->saved = false;
                st->closed = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_ERASEBKGND: {
            if (st && st->bgBrush) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wParam), &rc, st->bgBrush);
                return 1;
            }
            break;
        }

        case WM_CTLCOLORDLG: {
            if (st && st->bgBrush) {
                return reinterpret_cast<LRESULT>(st->bgBrush);
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            if (st && st->bgBrush) {
                const auto& colors = ThemeManager::Instance().GetColors();
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdc, colors.win32TextColor);
                SetBkColor(hdc, colors.win32BgColor);
                SetBkMode(hdc, OPAQUE);
                return reinterpret_cast<LRESULT>(st->bgBrush);
            }
            break;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mis && mis->CtlType == ODT_COMBOBOX) {
                mis->itemHeight = 26;
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_COMBOBOX && dis->hwndItem) {
                const auto& colors = ThemeManager::Instance().GetColors();
                bool isDark = ThemeManager::Instance().IsDarkActive();
                bool isSelected = ((dis->itemState & ODS_SELECTED) != 0) && ((dis->itemState & ODS_COMBOBOXEDIT) == 0);

                COLORREF bg = isSelected
                    ? RGB(37, 99, 235)
                    : (isDark ? RGB(30, 41, 59) : RGB(255, 255, 255));
                COLORREF fg = isSelected ? RGB(255, 255, 255) : colors.win32TextColor;

                HBRUSH hBr = CreateSolidBrush(bg);
                FillRect(dis->hDC, &dis->rcItem, hBr);
                DeleteObject(hBr);

                if (dis->itemID != static_cast<UINT>(-1)) {
                    int len = static_cast<int>(SendMessageW(dis->hwndItem, CB_GETLBTEXTLEN, dis->itemID, 0));
                    if (len > 0) {
                        std::wstring txt(static_cast<size_t>(len) + 1, L'\0');
                        SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(&txt[0]));
                        txt.resize(static_cast<size_t>(len));

                        HFONT hCtrlFont = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
                        HGDIOBJ oldFont = hCtrlFont ? SelectObject(dis->hDC, hCtrlFont) : nullptr;
                        int oldBk = SetBkMode(dis->hDC, TRANSPARENT);
                        COLORREF oldFg = SetTextColor(dis->hDC, fg);

                        RECT textRc = dis->rcItem;
                        textRc.left += 10;
                        textRc.right -= 8;
                        DrawTextW(dis->hDC, txt.c_str(), static_cast<int>(txt.size()), &textRc,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                        SetTextColor(dis->hDC, oldFg);
                        SetBkMode(dis->hDC, oldBk);
                        if (oldFont) SelectObject(dis->hDC, oldFont);
                    }
                }
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (st && st->inputBrush) {
                const auto& colors = ThemeManager::Instance().GetColors();
                bool isDark = ThemeManager::Instance().IsDarkActive();
                COLORREF inputBg = isDark ? RGB(30, 41, 59) : RGB(255, 255, 255);
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdc, colors.win32TextColor);
                SetBkColor(hdc, inputBg);
                return reinterpret_cast<LRESULT>(st->inputBrush);
            }
            break;
        }

        case WM_CLOSE: {
            if (st) {
                st->saved = false;
                st->closed = true;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            if (st) {
                if (st->bgBrush) {
                    DeleteObject(st->bgBrush);
                    st->bgBrush = nullptr;
                }
                if (st->inputBrush) {
                    DeleteObject(st->inputBrush);
                    st->inputBrush = nullptr;
                }
                st->closed = true;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowSettingsDialog(HWND parent) {
    CloseEmojiPicker();
    static bool classRegistered = false;
    const wchar_t* kSettingsClass = L"KDEConnectSettingsModal";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSettingsClass;
        wc.hIcon = ToastNotifier::Instance().GetSmsIcon();
        if (RegisterClassExW(&wc)) {
            classRegistered = true;
        }
    }

    const int clientW = 540;
    const int clientH = 598;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT winRc{ 0, 0, clientW, clientH };
    AdjustWindowRectEx(&winRc, style, FALSE, 0);
    int winW = winRc.right - winRc.left;
    int winH = winRc.bottom - winRc.top;

    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (parent && IsWindowVisible(parent) && !IsIconic(parent)) {
        RECT parentRc{};
        if (GetWindowRect(parent, &parentRc)) {
            posX = parentRc.left + ((parentRc.right - parentRc.left) - winW) / 2;
            posY = parentRc.top + ((parentRc.bottom - parentRc.top) - winH) / 2;
        }
    }

    SettingsModalState state{};
    const auto& dlgColors = ThemeManager::Instance().GetColors();
    bool dlgDark = ThemeManager::Instance().IsDarkActive();
    state.bgBrush = CreateSolidBrush(dlgColors.win32BgColor);
    state.inputBrush = CreateSolidBrush(dlgDark ? RGB(30, 41, 59) : RGB(255, 255, 255));

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kSettingsClass,
        Tr("settings_title").c_str(),
        style,
        posX, posY, winW, winH,
        parent, nullptr, hInst, &state
    );
    if (!hDlg) {
        if (state.bgBrush) DeleteObject(state.bgBrush);
        if (state.inputBrush) DeleteObject(state.inputBrush);
        return;
    }
    state.hwnd = hDlg;
    ThemeManager::Instance().ApplyToWindow(hDlg);

    auto addCtrl = [&](const wchar_t* cls, const std::wstring& text, DWORD ctrlStyle, DWORD exStyle, int x, int y, int w, int h, int id) -> HWND {
        HWND hChild = CreateWindowExW(
            exStyle, cls, text.c_str(), WS_CHILD | WS_VISIBLE | ctrlStyle,
            x, y, w, h, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr
        );
        if (hChild && g_hFont) {
            SendMessageW(hChild, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
        }
        return hChild;
    };

    // Group 1: Storage folders
    addCtrl(L"BUTTON", Tr("grp_storage"), BS_GROUPBOX, 0, 18, 12, 504, 134, 0);
    addCtrl(L"STATIC", Tr("lbl_conv_dir"), 0, 0, 32, 34, 476, 18, 0);
    state.convEdit = addCtrl(L"EDIT", GetMessagesDirectory(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 54, 376, 24, IDC_SET_CONV_EDIT);
    addCtrl(L"BUTTON", Tr("btn_browse"), BS_PUSHBUTTON | WS_TABSTOP, 0, 416, 53, 92, 26, IDC_SET_CONV_BROWSE);

    addCtrl(L"STATIC", Tr("lbl_recv_dir"), 0, 0, 32, 86, 476, 18, 0);
    state.recvEdit = addCtrl(L"EDIT", GetReceivedFilesDirectory(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 106, 376, 24, IDC_SET_RECV_EDIT);
    addCtrl(L"BUTTON", Tr("btn_browse"), BS_PUSHBUTTON | WS_TABSTOP, 0, 416, 105, 92, 26, IDC_SET_RECV_BROWSE);

    // Group 2: Network interface & device name
    addCtrl(L"BUTTON", Tr("grp_network"), BS_GROUPBOX, 0, 18, 156, 504, 150, 0);
    addCtrl(L"STATIC", Tr("lbl_bind_ip"), 0, 0, 32, 178, 476, 18, 0);
    state.bindCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, 32, 198, 476, 180, IDC_SET_BIND_COMBO);

    state.bindIpValues.push_back(L"");
    SendMessageW(state.bindCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr("bind_all_interfaces").c_str()));
    std::wstring configuredBind = TrimWideLocal(ReadIniString(L"Network", L"BindIp", L""));
    if (configuredBind.empty()) {
        configuredBind = TrimWideLocal(ReadIniString(L"Network", L"BindUP", L""));
    }
    if (configuredBind.empty()) {
        configuredBind = g_bindIp;
    }
    if (configuredBind == L"0.0.0.0") configuredBind.clear();
    state.resBindIp = configuredBind;

    std::vector<std::wstring> localIps = ProtocolHandler::GetLocalIpAddresses();
    int selectedBindIdx = 0;
    for (const auto& ip : localIps) {
        state.bindIpValues.push_back(ip);
        SendMessageW(state.bindCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ip.c_str()));
        if (!configuredBind.empty() && ip == configuredBind) {
            selectedBindIdx = static_cast<int>(state.bindIpValues.size()) - 1;
        }
    }
    if (!configuredBind.empty() && selectedBindIdx == 0) {
        state.bindIpValues.push_back(configuredBind);
        std::wstring customLabel = TrFmt("bind_custom_unavail", configuredBind);
        SendMessageW(state.bindCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(customLabel.c_str()));
        selectedBindIdx = static_cast<int>(state.bindIpValues.size()) - 1;
    }
    state.selectedBindIdx = selectedBindIdx;
    SendMessageW(state.bindCombo, CB_SETCURSEL, static_cast<WPARAM>(selectedBindIdx), 0);

    std::wstring activeEndpoint = SmsDaemon::Instance().GetBoundEndpointSummary();
    std::wstring activeBindErr = SmsDaemon::Instance().GetLastBindError();
    std::wstring iniBindLabel = configuredBind.empty() ? L"0.0.0.0 (auto)" : configuredBind;
    std::wstring bindStatusLine;
    if (!activeBindErr.empty()) {
        bindStatusLine = TrFmt("bind_status_err", iniBindLabel, activeBindErr);
    } else {
        bindStatusLine = TrFmt("bind_status_ok", activeEndpoint.empty() ? Tr("status_inactive") : activeEndpoint, iniBindLabel);
    }
    addCtrl(L"STATIC", bindStatusLine, SS_ENDELLIPSIS, 0, 32, 228, 476, 18, 0);

    addCtrl(L"STATIC", Tr("lbl_device_name"), 0, 0, 32, 250, 476, 18, 0);
    state.devNameEdit = addCtrl(L"EDIT", GetConfiguredDeviceName(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 270, 476, 24, IDC_SET_DEVNAME_EDIT);

    // Group 3: Application preferences
    addCtrl(L"BUTTON", Tr("grp_app_prefs"), BS_GROUPBOX, 0, 18, 316, 504, 222, 0);

    addCtrl(L"STATIC", Tr("lbl_theme"), 0, 0, 32, 340, 170, 20, 0);
    state.themeCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, 210, 337, 298, 140, IDC_SET_THEME_COMBO);
    SendMessageW(state.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr("theme_system").c_str()));
    SendMessageW(state.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr("theme_light").c_str()));
    SendMessageW(state.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr("theme_dark").c_str()));
    SendMessageW(state.themeCombo, CB_SETCURSEL, static_cast<WPARAM>(ThemeManager::Instance().GetMode()), 0);

    addCtrl(L"STATIC", Tr("lbl_language"), 0, 0, 32, 370, 170, 20, 0);
    state.langCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, 210, 367, 298, 180, IDC_SET_LANG_COMBO);
    for (int i = 0; i <= static_cast<int>(AppLanguage::Portuguese); ++i) {
        std::wstring langLabel = I18n::GetLanguageDisplayName(static_cast<AppLanguage>(i));
        SendMessageW(state.langCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(langLabel.c_str()));
    }
    SendMessageW(state.langCombo, CB_SETCURSEL, static_cast<WPARAM>(I18n::Instance().GetConfiguredLanguage()), 0);

    addCtrl(L"STATIC", Tr("lbl_sync"), 0, 0, 32, 400, 170, 20, 0);
    state.syncCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 0, 210, 397, 298, 160, IDC_SET_SYNC_COMBO);
    const std::pair<int, const char*> syncOptions[] = {
        { 3, "sync_3s" },
        { 5, "sync_5s" },
        { 10, "sync_10s" },
        { 30, "sync_30s" },
        { 60, "sync_60s" },
        { 0, "sync_manual" }
    };
    int curSyncSec = ReadIniInt(L"App", L"AutoSyncIntervalSec", 5);
    int syncSelIdx = 1;
    for (size_t i = 0; i < sizeof(syncOptions) / sizeof(syncOptions[0]); ++i) {
        state.syncSecValues.push_back(syncOptions[i].first);
        std::wstring optStr = Tr(syncOptions[i].second);
        SendMessageW(state.syncCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(optStr.c_str()));
        if (syncOptions[i].first == curSyncSec) {
            syncSelIdx = static_cast<int>(i);
        }
    }
    SendMessageW(state.syncCombo, CB_SETCURSEL, static_cast<WPARAM>(syncSelIdx), 0);

    auto addCheckRow = [&](int y, int chkId, int lblId, const std::wstring& labelText, bool checked) -> HWND {
        HWND hChk = addCtrl(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, y + 1, 18, 20, chkId);
        SendMessageW(hChk, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        addCtrl(L"STATIC", labelText, SS_NOTIFY, 0, 56, y + 2, 452, 18, lblId);
        return hChk;
    };

    state.minTrayChk = addCheckRow(432, IDC_SET_MINTRAY_CHK, IDC_SET_MINTRAY_LBL,
        Tr("chk_min_tray"),
        ReadIniInt(L"App", L"MinimizeToTrayOnClose", 1) != 0);

    state.notifyChk = addCheckRow(458, IDC_SET_NOTIFY_CHK, IDC_SET_NOTIFY_LBL,
        Tr("chk_notifs"),
        ReadIniInt(L"App", L"EnableNotifications", 1) != 0);

    state.startHiddenChk = addCheckRow(484, IDC_SET_STARTHIDDEN_CHK, IDC_SET_STARTHIDDEN_LBL,
        Tr("chk_start_hidden"),
        ReadIniInt(L"App", L"StartHidden", 0) != 0);

    state.skipDeleteChk = addCheckRow(510, IDC_SET_SKIPDEL_CHK, IDC_SET_SKIPDEL_LBL,
        Tr("chk_skip_del"),
        ReadIniInt(L"App", L"SkipDeleteConfirmation", 0) != 0);

    // Bottom status & actions
    std::wstring iniDisplay = L"INI : " + GetSettingsIniPath();
    addCtrl(L"STATIC", iniDisplay, SS_PATHELLIPSIS, 0, 18, 556, 290, 20, 0);
    addCtrl(L"BUTTON", Tr("btn_save"), BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 316, 550, 100, 30, IDC_SET_SAVE_BTN);
    addCtrl(L"BUTTON", Tr("btn_cancel"), BS_PUSHBUTTON | WS_TABSTOP, 0, 422, 550, 100, 30, IDC_SET_CANCEL_BTN);

    ThemeManager::Instance().ApplyToWindow(hDlg);
    AttachModernComboBox(state.bindCombo);
    AttachModernComboBox(state.themeCombo);
    AttachModernComboBox(state.langCombo);
    AttachModernComboBox(state.syncCombo);

    if (parent) EnableWindow(parent, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetForegroundWindow(hDlg);

    MSG modalMsg{};
    while (!state.closed && GetMessageW(&modalMsg, nullptr, 0, 0) > 0) {
        if (modalMsg.message == WM_KEYDOWN && modalMsg.wParam == VK_ESCAPE) {
            SendMessageW(hDlg, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(hDlg, &modalMsg)) {
            TranslateMessage(&modalMsg);
            DispatchMessageW(&modalMsg);
        }
    }
    if (modalMsg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(modalMsg.wParam));
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        if (IsWindowVisible(parent)) {
            SetForegroundWindow(parent);
        }
    }

    if (state.saved) {
        std::wstring prevConvDir = GetMessagesDirectory();
        SetConversationsDirectory(state.resConvDir);
        SetReceivedFilesDirectory(state.resRecvDir);

        g_bindIp = TrimWideLocal(state.resBindIp);
        if (g_bindIp == L"0.0.0.0") g_bindIp.clear();
        bool iniWriteOk = WriteIniString(L"Network", L"BindIp", g_bindIp.c_str());
        DeleteIniKey(L"Network", L"BindUP");
        std::wstring persistedBind = TrimWideLocal(ReadIniString(L"Network", L"BindIp", L""));
        if (persistedBind == L"0.0.0.0") persistedBind.clear();

        SetConfiguredDeviceName(state.resDeviceName);

        WriteIniInt(L"App", L"AutoSyncIntervalSec", state.resSyncSec);
        WriteIniInt(L"App", L"MinimizeToTrayOnClose", state.resMinToTray);
        WriteIniInt(L"App", L"EnableNotifications", state.resEnableNotifs);
        WriteIniInt(L"App", L"StartHidden", state.resStartHidden);
        WriteIniInt(L"App", L"SkipDeleteConfirmation", state.resSkipDeleteConfirm);

        I18n::Instance().SetLanguage(state.resLanguage);
        ThemeManager::Instance().SetMode(state.resThemeMode);
        ApplyComposerTheme();
        if (parent) {
            ConfigureSyncTimer(parent);
        }

        bool reloadOk = SmsDaemon::Instance().ApplySettingsReload(g_bindIp, state.resDeviceName);
        if (prevConvDir != GetMessagesDirectory()) {
            SmsDaemon::Instance().RequestConversationsSync();
        }
        if (parent) {
            ApplyLanguageToMainUi(parent);
            ReloadConversations(parent, true, true);
            InvalidateRect(parent, nullptr, FALSE);
        }
        if (!iniWriteOk || persistedBind != g_bindIp) {
            std::wstring iniErr = TrFmt("ini_write_error_msg", GetSettingsIniPath());
            MessageBoxW(parent, iniErr.c_str(), Tr("ini_write_error_title").c_str(), MB_OK | MB_ICONERROR);
        }
        if (!reloadOk) {
            std::wstring bindErr = SmsDaemon::Instance().GetLastBindError();
            std::wstring errMsg = TrFmt(
                "settings_bind_failed_msg",
                GetSettingsIniPath(),
                bindErr.empty() ? Tr("settings_bind_fallback_err") : bindErr
            );
            MessageBoxW(parent, errMsg.c_str(), Tr("bind_alert_title").c_str(), MB_OK | MB_ICONWARNING);
        }
    }
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

    // Composer bar at bottom of thread view: [Attach] [Emoji] [Composer] [Send]
    int composerHeight = 54;
    int composerY = height - composerHeight;
    int threadWidth = width - static_cast<int>(g_sidebarWidth);

    if (g_hwndAttachButton) {
        SetWindowPos(g_hwndAttachButton, nullptr, static_cast<int>(g_sidebarWidth) + 14, composerY + 9, 36, 36, SWP_NOZORDER);
    }

    if (g_hwndEmojiButton) {
        SetWindowPos(g_hwndEmojiButton, nullptr, static_cast<int>(g_sidebarWidth) + 54, composerY + 9, 36, 36, SWP_NOZORDER);
    }

    if (g_hwndComposerEdit) {
        int editX = static_cast<int>(g_sidebarWidth) + 96;
        int editWidth = std::max(80, threadWidth - 202);
        SetWindowPos(g_hwndComposerEdit, nullptr, editX, composerY + 11, editWidth, 32, SWP_NOZORDER);
    }

    if (g_hwndSendButton) {
        int sendX = width - 98;
        SetWindowPos(g_hwndSendButton, nullptr, sendX, composerY + 9, 82, 36, SWP_NOZORDER);
    }

    if (g_hwndSettingsButton) {
        SetWindowPos(g_hwndSettingsButton, nullptr, width - 200, 11, 110, 32, SWP_NOZORDER);
    }

    if (g_hwndSyncButton) {
        SetWindowPos(g_hwndSyncButton, nullptr, width - 84, 11, 34, 32, SWP_NOZORDER);
    }

    if (g_hwndThemeButton) {
        SetWindowPos(g_hwndThemeButton, nullptr, width - 44, 11, 34, 32, SWP_NOZORDER);
    }

    if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
        PositionEmojiPicker(hwnd);
    }
    UpdateConnectionTooltip(hwnd);
}

static void ClearPendingAttachment(HWND hwnd) {
    g_pendingAttachmentPath.clear();
    g_pendingAttachmentName.clear();
    if (g_hwndAttachButton) {
        SetWindowTextW(g_hwndAttachButton, L"\U0001F4CE");
    }
    if (hwnd) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void OnSendMessage(HWND hwnd) {
    CloseEmojiPicker();
    int length = GetWindowTextLengthW(g_hwndComposerEdit);
    std::wstring text;
    if (length > 0) {
        text.resize(length + 1, 0);
        GetWindowTextW(g_hwndComposerEdit, &text[0], length + 1);
        text.resize(length);

        // Normalize RichEdit CR line breaks to LF
        std::wstring normalized;
        normalized.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\r') {
                if (i + 1 < text.size() && text[i + 1] == L'\n') {
                    ++i;
                }
                normalized.push_back(L'\n');
            } else {
                normalized.push_back(text[i]);
            }
        }
        text.swap(normalized);

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

    bool wasSyntheticContact = (active->threadId <= -100000);
    std::wstring targetAddress = active->address;
    std::wstring targetKey = CanonicalSinglePhoneKeyLocal(targetAddress);
    int64_t preferredThreadId = -1;
    if (!wasSyntheticContact) {
        preferredThreadId = SmsDaemon::Instance().ResolveMergedThreadId(active->threadId);
    } else if (!targetKey.empty()) {
        for (const auto& c : g_allConversations) {
            if (CanonicalSinglePhoneKeyLocal(c.address) == targetKey) {
                preferredThreadId = c.threadId;
                break;
            }
        }
    }

    // Send via daemon (host TLS or IPC to background daemon)
    bool sentOk = SmsDaemon::Instance().SendOutgoingSms(targetAddress, text, preferredThreadId, g_pendingAttachmentPath);
    if (!sentOk) {
        MessageBoxW(
            hwnd,
            Tr("send_error_msg").c_str(),
            Tr("send_error_title").c_str(),
            MB_OK | MB_ICONERROR
        );
        return;
    }

    // Clear composer edit box and pending attachment
    SetWindowTextW(g_hwndComposerEdit, L"");
    ClearPendingAttachment(nullptr);

    // Reload conversation and keep focus on newly created or existing thread
    ReloadConversations(hwnd, true, true);
    if (wasSyntheticContact) {
        for (const auto& c : g_allConversations) {
            if ((!targetKey.empty() && CanonicalSinglePhoneKeyLocal(c.address) == targetKey) || c.address == targetAddress) {
                g_selectedThreadId = c.threadId;
                ScrollThreadToBottom(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }
    }
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

            // Register live conversation & phone connection update hooks before starting daemon
            SmsDaemon::Instance().SetOnConversationsUpdated([]() {
                if (g_hwndMain && IsWindow(g_hwndMain)) {
                    PostMessageW(g_hwndMain, WM_APP_REFRESH_CONVS, 0, 0);
                }
            });
            SmsDaemon::Instance().SetOnConnectionChanged([]() {
                if (g_hwndMain && IsWindow(g_hwndMain)) {
                    PostMessageW(g_hwndMain, WM_APP_CONN_CHANGED, 0, 0);
                }
            });

            // Start direct standalone KDE Connect protocol on configured BindIp
            SmsDaemon::Instance().Start(g_bindIp);
            g_lastPhoneConnected = SmsDaemon::Instance().IsPhoneConnected();
            g_lastPhoneName = SmsDaemon::Instance().GetConnectedDeviceName();
            g_lastBatteryLevel = SmsDaemon::Instance().GetPhoneBatteryLevel();
            g_lastBatteryCharging = SmsDaemon::Instance().IsPhoneCharging();
            UpdateMainWindowTitle(hwnd);
            if (!SmsDaemon::Instance().GetProtocol().IsListening()) {
                PostMessageW(hwnd, WM_APP_BIND_ALERT, 0, 0);
            }

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
            std::wstring searchPh = Tr("search_placeholder");
            SendMessageW(g_hwndSearchEdit, 0x1501 /* EM_SETCUEBANNER */, FALSE, reinterpret_cast<LPARAM>(searchPh.c_str()));

            // Load msftedit.dll so Win+. Emoji Panel & TSF work natively inside the composer
            if (!g_hMsftEdit) {
                g_hMsftEdit = LoadLibraryW(L"msftedit.dll");
            }
            if (g_hMsftEdit) {
                g_hwndComposerEdit = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                    380, 500, 400, 32, hwnd, (HMENU)IDC_COMPOSER_EDIT, GetModuleHandle(nullptr), nullptr
                );
                g_composerIsRichEdit = (g_hwndComposerEdit != nullptr);
            }
            if (!g_hwndComposerEdit) {
                g_hwndComposerEdit = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                    380, 500, 400, 32, hwnd, (HMENU)IDC_COMPOSER_EDIT, GetModuleHandle(nullptr), nullptr
                );
                g_composerIsRichEdit = false;
            }
            if (g_composerIsRichEdit) {
                SendMessageW(g_hwndComposerEdit, EM_EXLIMITTEXT, 0, 65535);
                SendMessageW(g_hwndComposerEdit, EM_SETEVENTMASK, 0, ENM_CHANGE);
            }
            g_origComposerProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(g_hwndComposerEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ComposerEditProc))
            );

            g_hwndSendButton = CreateWindowExW(
                0, L"BUTTON", Tr("btn_send").c_str(),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                790, 500, 82, 36, hwnd, (HMENU)IDC_SEND_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndSettingsButton = CreateWindowExW(
                0, L"BUTTON", Tr("btn_settings").c_str(),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                674, 11, 110, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BUTTON)), GetModuleHandle(nullptr), nullptr
            );

            g_hwndSyncButton = CreateWindowExW(
                0, L"BUTTON", L"\u21BB",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                810, 11, 34, 32, hwnd, (HMENU)IDC_SYNC_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndThemeButton = CreateWindowExW(
                0, L"BUTTON", L"\U0001F313",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                850, 11, 34, 32, hwnd, (HMENU)IDC_THEME_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndAttachButton = CreateWindowExW(
                0, L"BUTTON", L"\U0001F4CE",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                336, 500, 36, 36, hwnd, (HMENU)IDC_ATTACH_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            g_hwndEmojiButton = CreateWindowExW(
                0, L"BUTTON", L"\U0001F60A",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                376, 500, 36, 36, hwnd, (HMENU)IDC_EMOJI_BUTTON, GetModuleHandle(nullptr), nullptr
            );

            SendMessageW(g_hwndSearchEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndComposerEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSendButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSettingsButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSyncButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndThemeButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndAttachButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndEmojiButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            ThemeManager::Instance().ApplyThemeToChildren(hwnd);
            ApplyComposerTheme();

            // Tooltip for the top-right green/red phone connection status icon
            g_hwndConnTooltip = CreateWindowExW(
                WS_EX_TOPMOST,
                TOOLTIPS_CLASSW,
                nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd,
                nullptr,
                GetModuleHandle(nullptr),
                nullptr
            );
            if (g_hwndConnTooltip) {
                ThemeManager::Instance().ApplyToControl(g_hwndConnTooltip);
                std::wstring initTip = BuildConnectionTooltipText();
                TOOLINFOW ti{};
                ti.cbSize = sizeof(TOOLINFOW);
                ti.uFlags = TTF_SUBCLASS;
                ti.hwnd = hwnd;
                ti.uId = 1;
                ti.rect = GetConnectionIndicatorRect(hwnd);
                ti.lpszText = const_cast<LPWSTR>(initTip.c_str());
                SendMessageW(g_hwndConnTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
                SendMessageW(g_hwndConnTooltip, TTM_SETMAXTIPWIDTH, 0, 320);
                ThemeManager::Instance().RegisterCallback("ConnTooltip", [](bool, const ThemeColors&) {
                    if (g_hwndConnTooltip && IsWindow(g_hwndConnTooltip)) {
                        ThemeManager::Instance().ApplyToControl(g_hwndConnTooltip);
                    }
                });
            }

            LayoutChildControls(hwnd);
            ReloadConversations(hwnd, true, true);

            // Configure periodic sync check from INI and lightweight connection poll timer
            ConfigureSyncTimer(hwnd);
            SetTimer(hwnd, IDC_CONN_POLL_TIMER, 2000, nullptr);
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

        case WM_APP_CONN_CHANGED: {
            RefreshConnectionUi(hwnd);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == IDC_DEBOUNCE_TIMER) {
                KillTimer(hwnd, IDC_DEBOUNCE_TIMER);
                RefreshConnectionUi(hwnd);
                ReloadConversations(hwnd, true);
            } else if (wParam == IDC_SYNC_TIMER) {
                if (SmsDaemon::Instance().IsClientMode()) {
                    SmsDaemon::Instance().RefreshClientStatus();
                }
                SmsDaemon::Instance().RequestConversationsSync();
                bool nowConn = SmsDaemon::Instance().IsPhoneConnected();
                std::wstring nowDev = SmsDaemon::Instance().GetConnectedDeviceName();
                int nowBatt = SmsDaemon::Instance().GetPhoneBatteryLevel();
                bool nowChg = SmsDaemon::Instance().IsPhoneCharging();
                if (nowConn != g_lastPhoneConnected || nowDev != g_lastPhoneName ||
                    nowBatt != g_lastBatteryLevel || nowChg != g_lastBatteryCharging) {
                    RefreshConnectionUi(hwnd);
                } else {
                    UpdateMainWindowTitle(hwnd);
                }
                if (g_allConversations.empty()) {
                    std::wstring status = SmsDaemon::Instance().GetConnectionStatusText();
                    if (status != g_lastEmptyStatus) {
                        g_lastEmptyStatus = status;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            } else if (wParam == IDC_CONN_POLL_TIMER) {
                if (SmsDaemon::Instance().IsClientMode()) {
                    SmsDaemon::Instance().RefreshClientStatus();
                }
                bool nowConn = SmsDaemon::Instance().IsPhoneConnected();
                std::wstring nowDev = SmsDaemon::Instance().GetConnectedDeviceName();
                int nowBatt = SmsDaemon::Instance().GetPhoneBatteryLevel();
                bool nowChg = SmsDaemon::Instance().IsPhoneCharging();
                if (nowConn != g_lastPhoneConnected || nowDev != g_lastPhoneName ||
                    nowBatt != g_lastBatteryLevel || nowChg != g_lastBatteryCharging) {
                    RefreshConnectionUi(hwnd);
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

            bool phoneConnected = SmsDaemon::Instance().IsPhoneConnected();
            int batteryLevel = SmsDaemon::Instance().GetPhoneBatteryLevel();
            bool isCharging = SmsDaemon::Instance().IsPhoneCharging();
            float indicatorW = static_cast<float>(GetConnectionIndicatorWidth(phoneConnected, batteryLevel, isCharging));
            float headerRightReserve = 208.0f + indicatorW + 10.0f;

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
                    g_currentBubbleLayouts,
                    headerRightReserve
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
                std::wstring emptyMsg = TrFmt("empty_state_msg", g_lastEmptyStatus);
                g_renderer.RenderEmptyState(
                    g_sidebarWidth,
                    threadWidth,
                    totalHeight,
                    emptyMsg
                );
            }

            g_renderer.RenderConnectionIndicator(
                totalWidth - 208.0f,
                54.0f,
                phoneConnected,
                batteryLevel,
                isCharging
            );

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

        case WM_MOVE: {
            if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
                PositionEmojiPicker(hwnd);
            }
            return 0;
        }

        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND nextActive = reinterpret_cast<HWND>(lParam);
                if (nextActive != g_emojiPopup.hwnd) {
                    CloseEmojiPicker();
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            CloseEmojiPicker();
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            RECT rc{};
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            // Click on green/red phone connection indicator icon in header -> broadcast/refresh status
            RECT badgeRc = GetConnectionIndicatorRect(hwnd);
            if (x >= badgeRc.left && x <= badgeRc.right && y >= badgeRc.top && y <= badgeRc.bottom) {
                if (SmsDaemon::Instance().IsClientMode()) {
                    SmsDaemon::Instance().RefreshClientStatus();
                } else {
                    SmsDaemon::Instance().GetProtocol().BroadcastIdentity();
                }
                SmsDaemon::Instance().RequestConversationsSync();
                RefreshConnectionUi(hwnd);
                return 0;
            }

            // Click on [x] button inside the pending attachment preview bar
            if (!g_pendingAttachmentName.empty() &&
                x >= width - 40 && x <= width - 6 &&
                y >= height - 84 && y <= height - 54) {
                ClearPendingAttachment(hwnd);
                return 0;
            }

            // Clicked inside thread area: open image or video attachment when clicking a media bubble
            if (x >= static_cast<int>(g_sidebarWidth) && y >= 54 && y < height - 54) {
                float fx = static_cast<float>(x);
                float fy = static_cast<float>(y);
                for (auto it = g_currentBubbleLayouts.rbegin(); it != g_currentBubbleLayouts.rend(); ++it) {
                    const auto& b = *it;
                    if (!b.hasAttachment || b.attachmentPath.empty()) continue;
                    bool hitImage = (b.imageRect.right > b.imageRect.left &&
                                     fx >= b.imageRect.left && fx <= b.imageRect.right &&
                                     fy >= b.imageRect.top && fy <= b.imageRect.bottom);
                    bool hitBubble = (fx >= b.bubbleRect.left && fx <= b.bubbleRect.right &&
                                      fy >= b.bubbleRect.top && fy <= b.bubbleRect.bottom);
                    if (hitImage || hitBubble) {
                        ShellExecuteW(hwnd, L"open", b.attachmentPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                }
                return 0;
            }

            // Clicked inside sidebar list area (below 54px search bar)
            if (x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                float itemH = 68.0f;
                float relY = static_cast<float>(y) - 54.0f + g_sidebarScroll;
                int clickedIdx = static_cast<int>(relY / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_visibleConversations.size())) {
                    float rowLocalY = relY - static_cast<float>(clickedIdx) * itemH;
                    int64_t clickedId = g_visibleConversations[clickedIdx].threadId;
                    bool hitDeleteBtn = (clickedId > 0 &&
                                         x >= static_cast<int>(g_sidebarWidth) - 32 &&
                                         x <= static_cast<int>(g_sidebarWidth) - 6 &&
                                         rowLocalY >= 33.0f && rowLocalY <= 59.0f);
                    if (hitDeleteBtn) {
                        PerformDeleteConversation(hwnd, clickedIdx, true);
                    } else {
                        int64_t nextTid = clickedId;
                        if (nextTid <= -100000) {
                            std::wstring key = CanonicalSinglePhoneKeyLocal(g_visibleConversations[clickedIdx].address);
                            if (!key.empty()) {
                                for (const auto& c : g_allConversations) {
                                    if (CanonicalSinglePhoneKeyLocal(c.address) == key) {
                                        nextTid = c.threadId;
                                        break;
                                    }
                                }
                            }
                        } else if (nextTid > 0) {
                            nextTid = SmsDaemon::Instance().ResolveMergedThreadId(nextTid);
                        }
                        g_selectedThreadId = nextTid;
                        ScrollThreadToBottom(hwnd);
                        if (g_selectedThreadId > 0) {
                            SmsDaemon::Instance().RequestThreadSync(g_selectedThreadId);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            CloseEmojiPicker();
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            if (x >= 0 && x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                float itemH = 68.0f;
                int clickedIdx = static_cast<int>((static_cast<float>(y) - 54.0f + g_sidebarScroll) / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_visibleConversations.size()) &&
                    g_visibleConversations[clickedIdx].threadId > 0) {
                    HMENU hMenu = CreatePopupMenu();
                    if (hMenu) {
                        AppendMenuW(hMenu, MF_STRING, IDM_DELETE_CONV, Tr("menu_delete_conv").c_str());
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
            ApplyComposerTheme();
            if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
                InvalidateRect(g_emojiPopup.hwnd, nullptr, FALSE);
            }
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
                case IDC_SETTINGS_BUTTON:
                    ShowSettingsDialog(hwnd);
                    break;
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
                    ApplyComposerTheme();
                    if (g_emojiPopup.hwnd && IsWindow(g_emojiPopup.hwnd)) {
                        InvalidateRect(g_emojiPopup.hwnd, nullptr, FALSE);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
                case IDC_EMOJI_BUTTON: {
                    ToggleEmojiPicker(hwnd);
                    break;
                }
                case IDC_ATTACH_BUTTON: {
                    CloseEmojiPicker();
                    // Build double-null-terminated Win32 filter dynamically using localized labels
                    std::wstring filterBuf = Tr("filter_media");
                    filterBuf.push_back(L'\0');
                    filterBuf += L"*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov";
                    filterBuf.push_back(L'\0');
                    filterBuf += Tr("filter_all");
                    filterBuf.push_back(L'\0');
                    filterBuf += L"*.*";
                    filterBuf.push_back(L'\0');
                    filterBuf.push_back(L'\0');

                    OPENFILENAMEW ofn{};
                    wchar_t szFile[MAX_PATH] = L"";
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                    ofn.lpstrFilter = filterBuf.c_str();
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameW(&ofn)) {
                        g_pendingAttachmentPath = szFile;
                        g_pendingAttachmentName = std::filesystem::path(szFile).filename().wstring();
                        if (g_hwndAttachButton) {
                            SetWindowTextW(g_hwndAttachButton, L"\U0001F4CE\u2713");
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

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
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

        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            bool isDark = ThemeManager::Instance().IsDarkActive();
            COLORREF chromeBg = isDark ? RGB(31, 41, 59) : RGB(242, 245, 250);
            const auto& colors = ThemeManager::Instance().GetColors();
            SetTextColor(hdc, colors.win32TextColor);
            SetBkColor(hdc, chromeBg);
            if (!g_btnBrush || g_btnBrushColor != chromeBg) {
                if (g_btnBrush) DeleteObject(g_btnBrush);
                g_btnBrushColor = chromeBg;
                g_btnBrush = CreateSolidBrush(g_btnBrushColor);
            }
            return (LRESULT)g_btnBrush;
        }

        case WM_CLOSE: {
            SaveWindowBoundsToIni(hwnd);
            if (!g_forceExit && ReadIniInt(L"App", L"MinimizeToTrayOnClose", 1) != 0) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_SHOW_SMS_BUBBLE: {
            ToastNotifier::Instance().ProcessPendingBubbles();
            return 0;
        }

        case WM_APP_RESTORE_WINDOW: {
            RestoreFromTray(hwnd);
            std::wstring iniBind = TrimWideLocal(ReadIniString(L"Network", L"BindIp", L""));
            if (iniBind.empty()) {
                iniBind = TrimWideLocal(ReadIniString(L"Network", L"BindUP", L""));
            }
            if (iniBind == L"0.0.0.0") iniBind.clear();
            std::wstring activeBind = SmsDaemon::Instance().GetProtocol().GetEffectiveBindIp();
            if (iniBind != activeBind || !SmsDaemon::Instance().GetProtocol().IsListening()) {
                g_bindIp = iniBind;
                bool reloadOk = SmsDaemon::Instance().ApplySettingsReload(g_bindIp, GetConfiguredDeviceName());
                UpdateMainWindowTitle(hwnd);
                g_lastEmptyStatus = SmsDaemon::Instance().GetConnectionStatusText();
                InvalidateRect(hwnd, nullptr, FALSE);
                if (!reloadOk) {
                    PostMessageW(hwnd, WM_APP_BIND_ALERT, 0, 0);
                }
            }
            return 0;
        }

        case WM_APP_BIND_ALERT: {
            std::wstring err = SmsDaemon::Instance().GetLastBindError();
            if (!err.empty()) {
                std::wstring msg = TrFmt("bind_alert_msg", g_bindIp.empty() ? L"0.0.0.0" : g_bindIp, err);
                MessageBoxW(hwnd, msg.c_str(), Tr("bind_alert_title").c_str(), MB_OK | MB_ICONWARNING);
            }
            return 0;
        }

        case WM_APP_RELOAD_INI: {
            RestoreFromTray(hwnd);
            g_bindIp = TrimWideLocal(ReadIniString(L"Network", L"BindIp", L""));
            if (g_bindIp.empty()) {
                g_bindIp = TrimWideLocal(ReadIniString(L"Network", L"BindUP", L""));
            }
            if (g_bindIp == L"0.0.0.0") g_bindIp.clear();
            bool reloadOk = SmsDaemon::Instance().ApplySettingsReload(g_bindIp, GetConfiguredDeviceName());
            UpdateMainWindowTitle(hwnd);
            g_lastEmptyStatus = SmsDaemon::Instance().GetConnectionStatusText();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!reloadOk) {
                std::wstring err = SmsDaemon::Instance().GetLastBindError();
                if (!err.empty()) {
                    MessageBoxW(hwnd, err.c_str(), Tr("bind_alert_title").c_str(), MB_OK | MB_ICONWARNING);
                }
            }
            return 0;
        }

        case WM_OPEN_SMS_THREAD: {
            RestoreFromTray(hwnd);
            int64_t targetTid = static_cast<int64_t>(lParam);
            if (targetTid > 0) {
                targetTid = SmsDaemon::Instance().ResolveMergedThreadId(targetTid);
                g_selectedThreadId = targetTid;
                ReloadConversations(hwnd, true, true);
                ScrollThreadToBottom(hwnd);
                SmsDaemon::Instance().RequestThreadSync(targetTid);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_TRAY_ICON: {
            UINT eventMsg = LOWORD(static_cast<DWORD>(lParam));
            if (eventMsg == WM_LBUTTONUP || eventMsg == WM_LBUTTONDBLCLK) {
                RestoreFromTray(hwnd);
            } else if (eventMsg == WM_RBUTTONUP || eventMsg == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                if (hMenu) {
                    bool visible = (IsWindowVisible(hwnd) != FALSE && !IsIconic(hwnd));
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? Tr("tray_hide").c_str() : Tr("tray_open").c_str());
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SETTINGS, Tr("tray_settings").c_str());
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, Tr("tray_quit").c_str());
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
                    } else if (cmd == IDM_TRAY_SETTINGS) {
                        ShowSettingsDialog(hwnd);
                    } else if (cmd == IDM_TRAY_EXIT) {
                        g_forceExit = true;
                        DestroyWindow(hwnd);
                    }
                }
            }
            return 0;
        }

        case WM_DESTROY: {
            CloseEmojiPicker();
            SaveWindowBoundsToIni(hwnd);
            KillTimer(hwnd, IDC_SYNC_TIMER);
            KillTimer(hwnd, IDC_DEBOUNCE_TIMER);
            KillTimer(hwnd, IDC_CONN_POLL_TIMER);
            SmsDaemon::Instance().SetOnConversationsUpdated(nullptr);
            SmsDaemon::Instance().SetOnConnectionChanged(nullptr);
            ThemeManager::Instance().UnregisterCallback("ConnTooltip");
            if (g_hwndConnTooltip) {
                DestroyWindow(g_hwndConnTooltip);
                g_hwndConnTooltip = nullptr;
            }
            if (g_origComposerProc && g_hwndComposerEdit) {
                SetWindowLongPtrW(g_hwndComposerEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origComposerProc));
                g_origComposerProc = nullptr;
            }
            if (g_hMsftEdit) {
                FreeLibrary(g_hMsftEdit);
                g_hMsftEdit = nullptr;
            }
            if (g_editBrush) {
                DeleteObject(g_editBrush);
                g_editBrush = nullptr;
            }
            if (g_btnBrush) {
                DeleteObject(g_btnBrush);
                g_btnBrush = nullptr;
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int nCmdShow) {
    bool hasCliBindIp = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = TrimWideLocal(argv[i] ? argv[i] : L"");
            if (arg.empty()) continue;
            if ((arg == L"--ip" || arg == L"--bind" || arg == L"-b") && (i + 1 < argc)) {
                g_bindIp = TrimWideLocal(argv[++i]);
                hasCliBindIp = true;
            } else if (arg.rfind(L"--ip=", 0) == 0) {
                g_bindIp = TrimWideLocal(arg.substr(5));
                hasCliBindIp = true;
            } else if (arg.rfind(L"--bind=", 0) == 0) {
                g_bindIp = TrimWideLocal(arg.substr(7));
                hasCliBindIp = true;
            } else if (arg[0] != L'-') {
                g_bindIp = arg;
                hasCliBindIp = true;
            }
        }
        LocalFree(argv);
    }
    if (g_bindIp == L"0.0.0.0") {
        g_bindIp.clear();
    }

    // Initialize COM for WIC, Shell & Folder Picker
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Initialize multi-language i18n (defaults to OS UI language)
    I18n::Instance().Initialize();

    // First-run prompt for conversation storage folder + create local INI
    PromptFirstRunStorageSetup();

    if (hasCliBindIp) {
        WriteIniString(L"Network", L"BindIp", g_bindIp.c_str());
    } else {
        g_bindIp = TrimWideLocal(ReadIniString(L"Network", L"BindIp", L""));
        if (g_bindIp.empty()) {
            g_bindIp = TrimWideLocal(ReadIniString(L"Network", L"BindUP", L""));
        }
        if (g_bindIp == L"0.0.0.0") g_bindIp.clear();
    }

    // Reuse already-running KDEConnectSMS instance (even if minimized to tray)
    if (HWND hExisting = FindWindowW(WINDOW_CLASS_NAME, nullptr)) {
        PostMessageW(hExisting, hasCliBindIp ? WM_APP_RELOAD_INI : WM_APP_RESTORE_WINDOW, 0, 0);
        CoUninitialize();
        return 0;
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
        Tr("app_title_base").c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, initWidth, initHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return 1;

    int initialShow = (ReadIniInt(L"App", L"StartHidden", 0) != 0) ? SW_HIDE : nCmdShow;
    ShowWindow(hwnd, initialShow);
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
