#include "common.h"
#include "daemon.h"
#include "toast_notifier.h"
#include <iostream>

using namespace KDEConnect;

int wmain(int argc, wchar_t* argv[]) {
    // Console / Headless background service mode for KDE Connect SMS daemon
    std::wcout << L"==================================================" << std::endl;
    std::wcout << L"  KDE Connect SMS Sync Daemon (Windows Win32 Native)" << std::endl;
    std::wcout << L"==================================================" << std::endl;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ToastNotifier::Instance().Initialize(nullptr);

    SmsDaemon& daemon = SmsDaemon::Instance();
    std::wcout << L"[*] Monitoring folder: " << daemon.GetWatchDirectory() << std::endl;
    std::wcout << L"[*] Listening on UDP 1716 & TCP 1716 for KDE Connect..." << std::endl;

    if (!daemon.Start()) {
        std::wcerr << L"[!] Failed to start sync daemon." << std::endl;
        return 1;
    }

    std::wcout << L"[+] Daemon active and syncing. Press Ctrl+C to terminate." << std::endl;

    // Simple event wait loop
    HANDLE hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForSingleObject(hStopEvent, INFINITE);

    daemon.Stop();
    ToastNotifier::Instance().Shutdown();
    CoUninitialize();
    return 0;
}
