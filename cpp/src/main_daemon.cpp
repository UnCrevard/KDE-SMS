#include "common.h"
#include "daemon.h"
#include "toast_notifier.h"
#include <iostream>
#include <vector>
#include <string>

using namespace KDEConnect;

void PrintUsage() {
    std::wcout << L"Usage:\n"
               << L"  KDEConnectDaemon.exe [OPTIONS] [IP_ADDRESS]\n\n"
               << L"Options:\n"
               << L"  -b, --bind <IP>    Specify network interface IPv4 to bind on (e.g. 192.168.1.45)\n"
               << L"  --ip <IP>          Alias for --bind\n"
               << L"  -h, --help         Show this help message\n\n"
               << L"Examples:\n"
               << L"  KDEConnectDaemon.exe 192.168.1.45\n"
               << L"  KDEConnectDaemon.exe --bind 192.168.1.45\n"
               << L"  KDEConnectDaemon.exe\n" << std::endl;
}

int wmain(int argc, wchar_t* argv[]) {
    std::wcout << L"==================================================" << std::endl;
    std::wcout << L"  KDE Connect SMS Sync Daemon (Windows Win32 Native)" << std::endl;
    std::wcout << L"==================================================" << std::endl;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ToastNotifier::Instance().Initialize(nullptr);

    // Initialize Winsock to enumerate network adapters
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    std::vector<std::wstring> availableIps = ProtocolHandler::GetLocalIpAddresses();

    std::wstring targetBindIp = L"";

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help" || arg == L"/?") {
            PrintUsage();
            return 0;
        } else if ((arg == L"-b" || arg == L"--bind" || arg == L"--ip") && (i + 1 < argc)) {
            targetBindIp = argv[++i];
        } else if (!arg.empty() && arg[0] != L'-') {
            targetBindIp = arg;
        }
    }

    // Display available local adapters
    std::wcout << L"[*] Detected Local Network Adapters (IPv4):" << std::endl;
    if (availableIps.empty()) {
        std::wcout << L"    (None detected, fallback to 127.0.0.1)" << std::endl;
    } else {
        for (size_t i = 0; i < availableIps.size(); ++i) {
            std::wcout << L"    [" << (i + 1) << L"] " << availableIps[i];
            if (availableIps[i] == targetBindIp) {
                std::wcout << L"  <-- SELECTED";
            }
            std::wcout << std::endl;
        }
    }

    if (targetBindIp.empty()) {
        std::wcout << L"[*] No specific IP provided. Binding to 0.0.0.0 (all interfaces)." << std::endl;
        std::wcout << L"[*] TIP: If invisible to phone, pass your Wi-Fi IP as argument:" << std::endl;
        if (!availableIps.empty()) {
            std::wcout << L"    bin\\KDEConnectDaemon.exe " << availableIps[0] << std::endl;
        } else {
            std::wcout << L"    bin\\KDEConnectDaemon.exe 192.168.1.XX" << std::endl;
        }
    } else {
        std::wcout << L"[+] Explicitly binding to network interface: " << targetBindIp << std::endl;
    }

    SmsDaemon& daemon = SmsDaemon::Instance();
    std::wcout << L"[*] Monitoring folder: " << daemon.GetWatchDirectory() << std::endl;
    std::wcout << L"[*] Listening on UDP 1716 (Discovery) & TCP 1716 (Data)..." << std::endl;
    std::wcout << L"[*] Periodic identity broadcast active (every 4s)..." << std::endl;

    if (!daemon.Start(targetBindIp)) {
        std::wcerr << L"[!] Failed to start sync daemon." << std::endl;
        return 1;
    }

    std::wcout << L"[+] Daemon active and syncing. Press Ctrl+C to terminate." << std::endl;

    HANDLE hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WaitForSingleObject(hStopEvent, INFINITE);

    daemon.Stop();
    ToastNotifier::Instance().Shutdown();
    CoUninitialize();
    WSACleanup();
    return 0;
}

// Fallback entry point for MinGW-w64 when -municode is omitted
int main(int argc, char* argv[]) {
    std::vector<std::wstring> wargs;
    std::vector<wchar_t*> wargv;
    for (int i = 0; i < argc; ++i) {
        wargs.push_back(Utf8ToWide(argv[i]));
    }
    for (auto& s : wargs) {
        wargv.push_back(&s[0]);
    }
    wargv.push_back(nullptr);
    return wmain(argc, wargv.data());
}
