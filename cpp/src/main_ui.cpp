#include "common.h"
#include "theme_manager.h"
#include "direct2d_renderer.h"
#include "toast_notifier.h"
#include "daemon.h"
#include <shellapi.h>
#include <filesystem>
#include <map>
#include <set>
#include <cwctype>

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
#define IDC_SETTINGS_BUTTON  1008
#define IDC_SYNC_TIMER       1
#define IDC_DEBOUNCE_TIMER   2
#define IDM_DELETE_CONV      2001
#define IDM_TRAY_SHOW         2002
#define IDM_TRAY_EXIT         2003
#define IDM_TRAY_SETTINGS     2004
#define WM_APP_REFRESH_CONVS  (WM_APP + 1)
#define WM_APP_RESTORE_WINDOW (WM_APP + 2)
#define WM_APP_RELOAD_INI     (WM_APP + 3)
#define WM_APP_BIND_ALERT     (WM_APP + 4)

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

// Forward declarations
void OnSendMessage(HWND hwnd);
void ShowSettingsDialog(HWND parent);

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
    std::wstring base = L"KDE Connect SMS - Windows Win32";
    std::wstring endpoint = SmsDaemon::Instance().GetBoundEndpointSummary();
    std::wstring bindErr = SmsDaemon::Instance().GetLastBindError();
    std::wstring title = base;
    if (!bindErr.empty()) {
        title += L" [Erreur reseau]";
    } else if (SmsDaemon::Instance().IsClientMode()) {
        title += endpoint.empty() ? L" [Daemon IPC]" : (L" [IPC - " + endpoint + L"]");
    } else if (!endpoint.empty()) {
        title += L" [" + endpoint + L"]";
    }
    SetWindowTextW(hwnd, title.c_str());
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
        contactEntry.snippet = L"Contact \u00B7 " + c.phone;
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
    int resSyncSec = 5;
    int resMinToTray = 1;
    int resEnableNotifs = 1;
    int resStartHidden = 0;
    int resSkipDeleteConfirm = 0;

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
            if (id == IDC_SET_BIND_COMBO && code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(st->bindCombo, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(st->bindIpValues.size())) {
                    st->selectedBindIdx = sel;
                    st->resBindIp = st->bindIpValues[static_cast<size_t>(sel)];
                }
                return 0;
            } else if (id == IDC_SET_CONV_BROWSE) {
                std::wstring cur = GetWindowTextSafe(st->convEdit);
                std::wstring picked;
                if (PromptSelectStorageFolder(hwnd, L"Choisir le dossier des conversations", cur.empty() ? GetMessagesDirectory() : cur, picked)) {
                    SetWindowTextW(st->convEdit, picked.c_str());
                }
                return 0;
            } else if (id == IDC_SET_RECV_BROWSE) {
                std::wstring cur = GetWindowTextSafe(st->recvEdit);
                std::wstring picked;
                if (PromptSelectStorageFolder(hwnd, L"Choisir le dossier des fichiers recus", cur.empty() ? GetReceivedFilesDirectory() : cur, picked)) {
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

        case WM_CLOSE: {
            if (st) {
                st->saved = false;
                st->closed = true;
            }
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            if (st) st->closed = true;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowSettingsDialog(HWND parent) {
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
    const int clientH = 568;
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
    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kSettingsClass,
        L"Reglages - KDE Connect SMS",
        style,
        posX, posY, winW, winH,
        parent, nullptr, hInst, &state
    );
    if (!hDlg) return;
    state.hwnd = hDlg;

    auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD ctrlStyle, DWORD exStyle, int x, int y, int w, int h, int id) -> HWND {
        HWND hChild = CreateWindowExW(
            exStyle, cls, text, WS_CHILD | WS_VISIBLE | ctrlStyle,
            x, y, w, h, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr
        );
        if (hChild && g_hFont) {
            SendMessageW(hChild, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
        }
        return hChild;
    };

    // Group 1: Storage folders
    addCtrl(L"BUTTON", L"Dossiers d'enregistrement", BS_GROUPBOX, 0, 18, 12, 504, 134, 0);
    addCtrl(L"STATIC", L"Dossier des conversations (historique SMS) :", 0, 0, 32, 34, 476, 18, 0);
    state.convEdit = addCtrl(L"EDIT", GetMessagesDirectory().c_str(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 54, 376, 24, IDC_SET_CONV_EDIT);
    addCtrl(L"BUTTON", L"Parcourir...", BS_PUSHBUTTON | WS_TABSTOP, 0, 416, 53, 92, 26, IDC_SET_CONV_BROWSE);

    addCtrl(L"STATIC", L"Dossier des fichiers recus (pieces jointes MMS) :", 0, 0, 32, 86, 476, 18, 0);
    state.recvEdit = addCtrl(L"EDIT", GetReceivedFilesDirectory().c_str(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 106, 376, 24, IDC_SET_RECV_EDIT);
    addCtrl(L"BUTTON", L"Parcourir...", BS_PUSHBUTTON | WS_TABSTOP, 0, 416, 105, 92, 26, IDC_SET_RECV_BROWSE);

    // Group 2: Network interface & device name
    addCtrl(L"BUTTON", L"Reseau KDE Connect", BS_GROUPBOX, 0, 18, 156, 504, 150, 0);
    addCtrl(L"STATIC", L"Interface reseau a utiliser (Bind IPv4) :", 0, 0, 32, 178, 476, 18, 0);
    state.bindCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 32, 198, 476, 180, IDC_SET_BIND_COMBO);

    state.bindIpValues.push_back(L"");
    SendMessageW(state.bindCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Toutes les interfaces (0.0.0.0 - automatique)"));
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
        std::wstring customLabel = configuredBind + L" (configuree - non detectee)";
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
        bindStatusLine = L"INI : " + iniBindLabel + L" | Erreur : " + activeBindErr;
    } else {
        bindStatusLine = L"Ecoute active : " + (activeEndpoint.empty() ? L"inactive" : activeEndpoint) +
                         L"  |  INI BindIp : " + iniBindLabel;
    }
    addCtrl(L"STATIC", bindStatusLine.c_str(), SS_ENDELLIPSIS, 0, 32, 228, 476, 18, 0);

    addCtrl(L"STATIC", L"Nom de cet ordinateur sur KDE Connect :", 0, 0, 32, 250, 476, 18, 0);
    state.devNameEdit = addCtrl(L"EDIT", GetConfiguredDeviceName().c_str(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 270, 476, 24, IDC_SET_DEVNAME_EDIT);

    // Group 3: Application preferences
    addCtrl(L"BUTTON", L"Reglages de l'application", BS_GROUPBOX, 0, 18, 316, 504, 192, 0);

    addCtrl(L"STATIC", L"Theme de l'interface :", 0, 0, 32, 340, 170, 20, 0);
    state.themeCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 210, 337, 298, 140, IDC_SET_THEME_COMBO);
    SendMessageW(state.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Systeme (suivre Windows)"));
    SendMessageW(state.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Clair"));
    SendMessageW(state.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Sombre"));
    SendMessageW(state.themeCombo, CB_SETCURSEL, static_cast<WPARAM>(ThemeManager::Instance().GetMode()), 0);

    addCtrl(L"STATIC", L"Synchronisation auto :", 0, 0, 32, 370, 170, 20, 0);
    state.syncCombo = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 210, 367, 298, 160, IDC_SET_SYNC_COMBO);
    const std::pair<int, const wchar_t*> syncOptions[] = {
        { 3, L"Toutes les 3 secondes" },
        { 5, L"Toutes les 5 secondes (par defaut)" },
        { 10, L"Toutes les 10 secondes" },
        { 30, L"Toutes les 30 secondes" },
        { 60, L"Toutes les 60 secondes" },
        { 0, L"Manuelle uniquement (desactivee)" }
    };
    int curSyncSec = ReadIniInt(L"App", L"AutoSyncIntervalSec", 5);
    int syncSelIdx = 1;
    for (size_t i = 0; i < sizeof(syncOptions) / sizeof(syncOptions[0]); ++i) {
        state.syncSecValues.push_back(syncOptions[i].first);
        SendMessageW(state.syncCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(syncOptions[i].second));
        if (syncOptions[i].first == curSyncSec) {
            syncSelIdx = static_cast<int>(i);
        }
    }
    SendMessageW(state.syncCombo, CB_SETCURSEL, static_cast<WPARAM>(syncSelIdx), 0);

    state.minTrayChk = addCtrl(L"BUTTON", L"Reduire dans la zone de notification lors de la fermeture", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 402, 476, 22, IDC_SET_MINTRAY_CHK);
    SendMessageW(state.minTrayChk, BM_SETCHECK, ReadIniInt(L"App", L"MinimizeToTrayOnClose", 1) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    state.notifyChk = addCtrl(L"BUTTON", L"Activer les notifications bureau pour les nouveaux SMS", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 428, 476, 22, IDC_SET_NOTIFY_CHK);
    SendMessageW(state.notifyChk, BM_SETCHECK, ReadIniInt(L"App", L"EnableNotifications", 1) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    state.startHiddenChk = addCtrl(L"BUTTON", L"Demarrer l'application reduite dans la zone de notification", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 454, 476, 22, IDC_SET_STARTHIDDEN_CHK);
    SendMessageW(state.startHiddenChk, BM_SETCHECK, ReadIniInt(L"App", L"StartHidden", 0) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    state.skipDeleteChk = addCtrl(L"BUTTON", L"Ne pas demander de confirmation lors de la suppression d'une conversation", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 480, 476, 22, IDC_SET_SKIPDEL_CHK);
    SendMessageW(state.skipDeleteChk, BM_SETCHECK, ReadIniInt(L"App", L"SkipDeleteConfirmation", 0) != 0 ? BST_CHECKED : BST_UNCHECKED, 0);

    // Bottom status & actions
    std::wstring iniDisplay = L"INI : " + GetSettingsIniPath();
    addCtrl(L"STATIC", iniDisplay.c_str(), SS_PATHELLIPSIS, 0, 18, 526, 290, 20, 0);
    addCtrl(L"BUTTON", L"Enregistrer", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 316, 520, 100, 30, IDC_SET_SAVE_BTN);
    addCtrl(L"BUTTON", L"Annuler", BS_PUSHBUTTON | WS_TABSTOP, 0, 422, 520, 100, 30, IDC_SET_CANCEL_BTN);

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

        ThemeManager::Instance().SetMode(state.resThemeMode);
        if (parent) {
            ConfigureSyncTimer(parent);
        }

        bool reloadOk = SmsDaemon::Instance().ApplySettingsReload(g_bindIp, state.resDeviceName);
        if (prevConvDir != GetMessagesDirectory()) {
            SmsDaemon::Instance().RequestConversationsSync();
        }
        if (parent) {
            UpdateMainWindowTitle(parent);
            g_lastEmptyStatus = SmsDaemon::Instance().GetConnectionStatusText();
            ReloadConversations(parent, true, true);
            InvalidateRect(parent, nullptr, FALSE);
        }
        if (!iniWriteOk || persistedBind != g_bindIp) {
            std::wstring iniErr = L"Impossible d'enregistrer l'adresse BindIp dans :\n" + GetSettingsIniPath() +
                                  L"\n\nVerifiez que le dossier est accessible en ecriture et que le fichier n'est pas verrouille.";
            MessageBoxW(parent, iniErr.c_str(), L"Erreur d'ecriture INI", MB_OK | MB_ICONERROR);
        }
        if (!reloadOk) {
            std::wstring bindErr = SmsDaemon::Instance().GetLastBindError();
            std::wstring errMsg = L"Les reglages ont ete enregistres dans :\n" + GetSettingsIniPath() +
                                  L"\n\nCependant, la liaison reseau sur l'adresse choisie a echoue :\n\n" +
                                  (bindErr.empty() ? L"Impossible de binder UDP/TCP sur le port 1716." : bindErr);
            MessageBoxW(parent, errMsg.c_str(), L"Erreur de liaison reseau", MB_OK | MB_ICONWARNING);
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

    if (g_hwndSettingsButton) {
        SetWindowPos(g_hwndSettingsButton, nullptr, width - 180, 11, 90, 32, SWP_NOZORDER);
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
            L"Impossible d'envoyer le message ou l'image.\nVerifiez que le telephone est connecte et appaire, et que le fichier image est accessible.",
            L"Echec d'envoi",
            MB_OK | MB_ICONERROR
        );
        return;
    }

    // Clear composer edit box and pending image
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

            // Register live conversation update hook before starting daemon
            SmsDaemon::Instance().SetOnConversationsUpdated([]() {
                if (g_hwndMain && IsWindow(g_hwndMain)) {
                    PostMessageW(g_hwndMain, WM_APP_REFRESH_CONVS, 0, 0);
                }
            });

            // Start direct standalone KDE Connect protocol on configured BindIp
            SmsDaemon::Instance().Start(g_bindIp);
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

            g_hwndSettingsButton = CreateWindowExW(
                0, L"BUTTON", L"\u2699 Reglages",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                674, 11, 90, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BUTTON)), GetModuleHandle(nullptr), nullptr
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
            SendMessageW(g_hwndSettingsButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndSyncButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndThemeButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessageW(g_hwndAttachButton, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            LayoutChildControls(hwnd);
            ReloadConversations(hwnd, true, true);

            // Configure periodic sync check from INI
            ConfigureSyncTimer(hwnd);
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
                UpdateMainWindowTitle(hwnd);
                ReloadConversations(hwnd, true);
            } else if (wParam == IDC_SYNC_TIMER) {
                SmsDaemon::Instance().RequestConversationsSync();
                UpdateMainWindowTitle(hwnd);
                if (g_allConversations.empty()) {
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
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            if (x >= 0 && x < static_cast<int>(g_sidebarWidth) && y >= 54) {
                float itemH = 68.0f;
                int clickedIdx = static_cast<int>((static_cast<float>(y) - 54.0f + g_sidebarScroll) / itemH);
                if (clickedIdx >= 0 && clickedIdx < static_cast<int>(g_visibleConversations.size()) &&
                    g_visibleConversations[clickedIdx].threadId > 0) {
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
                std::wstring msg = L"Impossible d'ouvrir l'ecoute KDE Connect sur l'adresse configuree (" +
                                   (g_bindIp.empty() ? L"0.0.0.0" : g_bindIp) + L":1716) :\n\n" + err +
                                   L"\n\nOuvrez \"Reglages\" pour choisir une interface reseau valide ou fermez l'application qui utilise deja le port 1716.";
                MessageBoxW(hwnd, msg.c_str(), L"Erreur de liaison reseau", MB_OK | MB_ICONWARNING);
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
                    MessageBoxW(hwnd, err.c_str(), L"Erreur de liaison reseau", MB_OK | MB_ICONWARNING);
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
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? L"Masquer la fenetre" : L"Ouvrir KDE Connect SMS");
                    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SETTINGS, L"Reglages...");
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
        L"KDE Connect SMS - Windows Win32",
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
