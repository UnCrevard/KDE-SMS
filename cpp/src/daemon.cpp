#include "daemon.h"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace KDEConnect {

    SmsDaemon& SmsDaemon::Instance() {
        static SmsDaemon instance;
        return instance;
    }

    SmsDaemon::SmsDaemon() {
        m_watchDir = GetMessagesDirectory();
    }

    SmsDaemon::~SmsDaemon() {
        Stop();
    }

    bool SmsDaemon::Start() {
        if (m_running) return true;

        m_running = true;

        // Open directory handle for ReadDirectoryChangesW
        m_dirHandle = CreateFileW(
            m_watchDir.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr
        );

        if (m_dirHandle == INVALID_HANDLE_VALUE) {
            std::wcerr << L"[Daemon] Warning: Could not open directory handle for " << m_watchDir << std::endl;
        }

        // Start protocol handler on UDP 1716 and TCP 1716
        m_protocol.SetOnPacketReceived([this](const DeviceInfo& dev, const KDEPacket& pkt) {
            HandleIncomingKdePacket(dev, pkt);
        });

        m_protocol.Start();

        // Start worker threads
        m_watcherThread = std::thread(&SmsDaemon::DirectoryWatcherThread, this);
        m_ipcThread = std::thread(&SmsDaemon::NamedPipeIpcThread, this);

        // Preload any existing messages from disk
        LoadAllConversations();

        return true;
    }

    void SmsDaemon::Stop() {
        if (!m_running) return;

        m_running = false;

        if (m_dirHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_dirHandle);
            m_dirHandle = INVALID_HANDLE_VALUE;
        }

        m_protocol.Stop();

        if (m_watcherThread.joinable()) m_watcherThread.join();
        if (m_ipcThread.joinable()) m_ipcThread.join();
    }

    void SmsDaemon::DirectoryWatcherThread() {
        alignas(DWORD) char buffer[4096];
        DWORD bytesReturned = 0;

        while (m_running && m_dirHandle != INVALID_HANDLE_VALUE) {
            BOOL success = ReadDirectoryChangesW(
                m_dirHandle,
                buffer,
                sizeof(buffer),
                FALSE, // Watch current directory only
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                &bytesReturned,
                nullptr,
                nullptr
            );

            if (!success || bytesReturned == 0) {
                // If handle closed or error occurred
                break;
            }

            FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            while (fni) {
                std::wstring fileName(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
                if (fileName.ends_with(L".json")) {
                    ProcessFolderChangeNotification();
                }

                if (fni->NextEntryOffset == 0) break;
                fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(fni) + fni->NextEntryOffset);
            }
        }
    }

    void SmsDaemon::ProcessFolderChangeNotification() {
        // Trigger a reload and notify local listeners
        LoadAllConversations();
    }

    void SmsDaemon::NamedPipeIpcThread() {
        while (m_running) {
            HANDLE pipe = CreateNamedPipeW(
                IPC_PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                8192,
                8192,
                0,
                nullptr
            );

            if (pipe == INVALID_HANDLE_VALUE) break;

            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected) {
                char readBuffer[2048];
                DWORD bytesRead = 0;
                if (ReadFile(pipe, readBuffer, sizeof(readBuffer) - 1, &bytesRead, nullptr)) {
                    readBuffer[bytesRead] = '\0';
                    std::string cmd(readBuffer);

                    // Respond with status or process commands
                    std::string response = "{\"status\":\"ok\"}\n";
                    DWORD bytesWritten = 0;
                    WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
                }
                DisconnectNamedPipe(pipe);
            }
            CloseHandle(pipe);
        }
    }

    void SmsDaemon::HandleIncomingKdePacket(const DeviceInfo& /*device*/, const KDEPacket& packet) {
        if (packet.type == "kdeconnect.telephony") {
            // Incoming SMS event notification
            // Extract phone number and message body
            std::string phone;
            std::string body;

            size_t pPos = packet.bodyJson.find("\"phoneNumber\":");
            if (pPos != std::string::npos) {
                size_t q1 = packet.bodyJson.find('"', pPos + 14);
                size_t q2 = packet.bodyJson.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    phone = packet.bodyJson.substr(q1 + 1, q2 - q1 - 1);
                }
            }

            size_t bPos = packet.bodyJson.find("\"messageBody\":");
            if (bPos != std::string::npos) {
                size_t q1 = packet.bodyJson.find('"', bPos + 14);
                size_t q2 = packet.bodyJson.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    body = packet.bodyJson.substr(q1 + 1, q2 - q1 - 1);
                }
            }

            if (!phone.empty() && !body.empty()) {
                Message msg;
                msg.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                msg.threadId = std::hash<std::string>{}(phone) % 100000;
                msg.address = Utf8ToWide(phone);
                msg.body = Utf8ToWide(body);
                msg.timestamp = msg.id;
                msg.isOutgoing = false;
                msg.isRead = false;

                SaveMessageToDisk(msg);

                // Fire native Windows Toast Notification
                ToastNotifier::Instance().ShowSmsNotification(msg.address, msg.body, msg.threadId);
            }
        }
    }

    void SmsDaemon::SaveMessageToDisk(const Message& msg) {
        std::lock_guard<std::mutex> lock(m_storageMutex);

        std::wstring filePath = m_watchDir + L"\\thread_" + std::to_wstring(msg.threadId) + L".json";

        // Read or append to conversation file
        auto& conv = m_conversations[msg.threadId];
        conv.threadId = msg.threadId;
        conv.address = msg.address;
        if (conv.contactName.empty()) conv.contactName = msg.address;
        conv.snippet = msg.body;
        conv.lastTimestamp = msg.timestamp;
        conv.messages.push_back(msg);

        // Serialize to disk (triggers ReadDirectoryChangesW for any other watching process)
        std::ofstream file(std::filesystem::path(filePath), std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "{\n"
                 << "  \"threadId\": " << conv.threadId << ",\n"
                 << "  \"address\": \"" << WideToUtf8(conv.address) << "\",\n"
                 << "  \"contactName\": \"" << WideToUtf8(conv.contactName) << "\",\n"
                 << "  \"messages\": [\n";

            for (size_t i = 0; i < conv.messages.size(); ++i) {
                const auto& m = conv.messages[i];
                file << "    {\n"
                     << "      \"id\": " << m.id << ",\n"
                     << "      \"body\": \"" << WideToUtf8(m.body) << "\",\n"
                     << "      \"timestamp\": " << m.timestamp << ",\n"
                     << "      \"isOutgoing\": " << (m.isOutgoing ? "true" : "false") << "\n"
                     << "    }" << (i + 1 < conv.messages.size() ? "," : "") << "\n";
            }
            file << "  ]\n}\n";
            file.close();
        }
    }

    std::vector<Conversation> SmsDaemon::LoadAllConversations() {
        std::lock_guard<std::mutex> lock(m_storageMutex);

        // If storage is empty, populate with initial demo threads if files don't exist yet
        if (m_conversations.empty()) {
            // Seed a few demo conversations for out-of-the-box native preview
            Conversation alice;
            alice.threadId = 101;
            alice.address = L"+1 (555) 019-2834";
            alice.contactName = L"Alice Vance";
            alice.snippet = L"Here are the test results from the lab! 🧪🎉";
            alice.lastTimestamp = 1710150000000;
            alice.unreadCount = 1;

            Message m1{1, 101, alice.address, L"Hey! Did you get a chance to inspect the new Win32 build?", 1710148000000, false, true};
            Message m2{2, 101, alice.address, L"Yes! The DirectWrite color emoji rendering looks gorgeous 😎🚀", 1710149000000, true, true};
            Message m3{3, 101, alice.address, L"Here are the test results from the lab! 🧪🎉", 1710150000000, false, false};
            alice.messages = {m1, m2, m3};

            Conversation bob;
            bob.threadId = 102;
            bob.address = L"+1 (555) 084-9912";
            bob.contactName = L"Bob Miller";
            bob.snippet = L"Meeting moved to 3 PM in the conference room.";
            bob.lastTimestamp = 1710142000000;
            bob.unreadCount = 0;

            Message b1{4, 102, bob.address, L"Are we still on for the sync today?", 1710141000000, false, true};
            Message b2{5, 102, bob.address, L"Meeting moved to 3 PM in the conference room.", 1710142000000, false, true};
            bob.messages = {b1, b2};

            Conversation charlie;
            charlie.threadId = 103;
            charlie.address = L"+1 (555) 041-3200";
            charlie.contactName = L"Charlie Brown";
            charlie.snippet = L"Photo attachment received 📸";
            charlie.lastTimestamp = 1710130000000;
            charlie.unreadCount = 0;

            Message c1{6, 103, charlie.address, L"Check out this screenshot from Windows 11 Fluent 🎨", 1710130000000, false, true};
            charlie.messages = {c1};

            m_conversations[alice.threadId] = alice;
            m_conversations[bob.threadId] = bob;
            m_conversations[charlie.threadId] = charlie;
        }

        std::vector<Conversation> result;
        for (const auto& pair : m_conversations) {
            result.push_back(pair.second);
        }

        // Sort descending by last timestamp
        std::sort(result.begin(), result.end(), [](const Conversation& a, const Conversation& b) {
            return a.lastTimestamp > b.lastTimestamp;
        });

        return result;
    }

    Conversation SmsDaemon::LoadConversation(int64_t threadId) {
        std::lock_guard<std::mutex> lock(m_storageMutex);
        auto it = m_conversations.find(threadId);
        if (it != m_conversations.end()) {
            return it->second;
        }
        return {};
    }

    bool SmsDaemon::SendOutgoingSms(const std::wstring& phoneNumber, const std::wstring& text) {
        int64_t threadId = std::hash<std::string>{}(WideToUtf8(phoneNumber)) % 100000;

        Message msg;
        msg.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        msg.threadId = threadId;
        msg.address = phoneNumber;
        msg.body = text;
        msg.timestamp = msg.id;
        msg.isOutgoing = true;
        msg.isRead = true;

        SaveMessageToDisk(msg);

        // Also forward over KDE Connect protocol to paired phone
        DeviceInfo targetPhone;
        targetPhone.ipAddress = L"192.168.1.50"; // default or paired
        targetPhone.port = 1716;
        m_protocol.SendSms(targetPhone, phoneNumber, text);

        return true;
    }

} // namespace KDEConnect
