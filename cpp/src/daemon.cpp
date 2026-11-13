#include "daemon.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include <cstring>
#include <cstdlib>

namespace KDEConnect {

    namespace {

        std::string EscapeJson(const std::string& input) {
            std::ostringstream ss;
            for (unsigned char c : input) {
                switch (c) {
                    case '"': ss << "\\\""; break;
                    case '\\': ss << "\\\\"; break;
                    case '\b': ss << "\\b"; break;
                    case '\f': ss << "\\f"; break;
                    case '\n': ss << "\\n"; break;
                    case '\r': ss << "\\r"; break;
                    case '\t': ss << "\\t"; break;
                    default:
                        if (c < 0x20) {
                            ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                        } else {
                            ss << static_cast<char>(c);
                        }
                        break;
                }
            }
            return ss.str();
        }

        void AppendUtf8Codepoint(uint32_t cp, std::string& out) {
            if (cp <= 0x7F) {
                out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp <= 0x10FFFF) {
                out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        bool ParseHex4(const std::string& s, size_t pos, uint32_t& outVal) {
            if (pos + 4 > s.size()) return false;
            uint32_t val = 0;
            for (size_t i = 0; i < 4; ++i) {
                char ch = s[pos + i];
                val <<= 4;
                if (ch >= '0' && ch <= '9') val |= static_cast<uint32_t>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') val |= static_cast<uint32_t>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') val |= static_cast<uint32_t>(ch - 'A' + 10);
                else return false;
            }
            outVal = val;
            return true;
        }

        std::string UnescapeJsonString(const std::string& raw) {
            std::string out;
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    char next = raw[++i];
                    switch (next) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            uint32_t cp1 = 0;
                            if (ParseHex4(raw, i + 1, cp1)) {
                                i += 4;
                                if (cp1 >= 0xD800 && cp1 <= 0xDBFF &&
                                    i + 6 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                                    uint32_t cp2 = 0;
                                    if (ParseHex4(raw, i + 3, cp2) && cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                                        uint32_t fullCp = 0x10000 + (((cp1 - 0xD800) << 10) | (cp2 - 0xDC00));
                                        AppendUtf8Codepoint(fullCp, out);
                                        i += 6;
                                        break;
                                    }
                                }
                                AppendUtf8Codepoint(cp1, out);
                            } else {
                                out.push_back('u');
                            }
                            break;
                        }
                        default:
                            out.push_back(next);
                            break;
                    }
                } else {
                    out.push_back(raw[i]);
                }
            }
            return out;
        }

        size_t SkipWs(const std::string& s, size_t pos) {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
                ++pos;
            }
            return pos;
        }

        size_t FindStringEnd(const std::string& s, size_t openQuote) {
            size_t i = openQuote + 1;
            while (i < s.size()) {
                if (s[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (s[i] == '"') return i;
                ++i;
            }
            return std::string::npos;
        }

        size_t FindCompoundEnd(const std::string& s, size_t startPos) {
            char openCh = s[startPos];
            char closeCh = (openCh == '{') ? '}' : ']';
            int depth = 0;
            for (size_t i = startPos; i < s.size(); ++i) {
                if (s[i] == '"') {
                    size_t qEnd = FindStringEnd(s, i);
                    if (qEnd == std::string::npos) return std::string::npos;
                    i = qEnd;
                    continue;
                }
                if (s[i] == openCh) ++depth;
                else if (s[i] == closeCh) {
                    --depth;
                    if (depth == 0) return i;
                }
            }
            return std::string::npos;
        }

        // Iterates top-level fields of a JSON object
        template<typename Fn>
        void ForEachJsonObjectField(const std::string& obj, Fn&& callback) {
            size_t pos = SkipWs(obj, 0);
            if (pos >= obj.size() || obj[pos] != '{') return;
            ++pos;

            while (pos < obj.size()) {
                pos = SkipWs(obj, pos);
                if (pos >= obj.size() || obj[pos] == '}') break;
                if (obj[pos] == ',') { ++pos; continue; }
                if (obj[pos] != '"') break;

                size_t keyEnd = FindStringEnd(obj, pos);
                if (keyEnd == std::string::npos) break;
                std::string key = obj.substr(pos + 1, keyEnd - pos - 1);

                pos = SkipWs(obj, keyEnd + 1);
                if (pos >= obj.size() || obj[pos] != ':') break;
                pos = SkipWs(obj, pos + 1);
                if (pos >= obj.size()) break;

                size_t valStart = pos;
                size_t nextPos = pos;

                if (obj[pos] == '"') {
                    size_t valEnd = FindStringEnd(obj, pos);
                    if (valEnd == std::string::npos) break;
                    nextPos = valEnd + 1;
                    std::string rawVal = obj.substr(pos + 1, valEnd - pos - 1);
                    callback(key, rawVal, true);
                } else if (obj[pos] == '{' || obj[pos] == '[') {
                    size_t valEnd = FindCompoundEnd(obj, pos);
                    if (valEnd == std::string::npos) break;
                    nextPos = valEnd + 1;
                    callback(key, obj.substr(valStart, nextPos - valStart), false);
                } else {
                    size_t valEnd = obj.find_first_of(",}\r\n", pos);
                    if (valEnd == std::string::npos) valEnd = obj.size();
                    nextPos = valEnd;
                    std::string rawVal = obj.substr(valStart, valEnd - valStart);
                    while (!rawVal.empty() && (rawVal.back() == ' ' || rawVal.back() == '\t')) rawVal.pop_back();
                    callback(key, rawVal, false);
                }
                pos = nextPos;
            }
        }

        // Iterates elements of a JSON array of objects
        template<typename Fn>
        void ForEachJsonArrayObject(const std::string& arr, Fn&& callback) {
            size_t pos = SkipWs(arr, 0);
            if (pos >= arr.size() || arr[pos] != '[') return;
            ++pos;

            while (pos < arr.size()) {
                pos = SkipWs(arr, pos);
                if (pos >= arr.size() || arr[pos] == ']') break;
                if (arr[pos] == ',') { ++pos; continue; }

                if (arr[pos] == '{') {
                    size_t objEnd = FindCompoundEnd(arr, pos);
                    if (objEnd == std::string::npos) break;
                    callback(arr.substr(pos, objEnd - pos + 1));
                    pos = objEnd + 1;
                } else if (arr[pos] == '[') {
                    size_t end = FindCompoundEnd(arr, pos);
                    if (end == std::string::npos) break;
                    pos = end + 1;
                } else if (arr[pos] == '"') {
                    size_t end = FindStringEnd(arr, pos);
                    if (end == std::string::npos) break;
                    pos = end + 1;
                } else {
                    size_t end = arr.find_first_of(",]\r\n", pos);
                    if (end == std::string::npos) break;
                    pos = end;
                }
            }
        }

        std::string HexEncode(const std::string& bytes) {
            static const char* hex = "0123456789abcdef";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (unsigned char c : bytes) {
                out.push_back(hex[(c >> 4) & 0x0F]);
                out.push_back(hex[c & 0x0F]);
            }
            return out;
        }

        std::string HexDecode(const std::string& hexStr) {
            std::string out;
            out.reserve(hexStr.size() / 2);
            auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
                int h = nibble(hexStr[i]);
                int l = nibble(hexStr[i + 1]);
                if (h < 0 || l < 0) break;
                out.push_back(static_cast<char>((h << 4) | l));
            }
            return out;
        }

        std::wstring NormalizePhoneSuffix(const std::wstring& phone) {
            std::wstring digits;
            for (wchar_t ch : phone) {
                if (ch >= L'0' && ch <= L'9') digits.push_back(ch);
            }
            if (digits.size() > 9) {
                return digits.substr(digits.size() - 9);
            }
            return digits;
        }

    } // namespace

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

    void SmsDaemon::SetOnConversationsUpdated(ConversationsUpdatedCallback cb) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_onConversationsUpdated = std::move(cb);
    }

    void SmsDaemon::NotifyConversationsUpdated() {
        ConversationsUpdatedCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            cb = m_onConversationsUpdated;
        }
        if (cb) cb();
    }

    bool SmsDaemon::Start(const std::wstring& bindIp) {
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
            std::wcerr << L"[Daemon] Avertissement: impossible d'ouvrir le dossier " << m_watchDir << std::endl;
        }

        // Preload any existing synced threads from disk immediately
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            LoadAllConversationsFromDiskLocked();
        }

        // Hook protocol packet and ready callbacks
        m_protocol.SetOnPacketReceived([this](const DeviceInfo& dev, const KDEPacket& pkt) {
            HandleIncomingKdePacket(dev, pkt);
        });

        m_protocol.SetOnDeviceReady([this](const DeviceInfo& dev) {
            std::wcout << L"[SMS Sync] Appareil pret (" << (dev.deviceName.empty() ? dev.ipAddress : dev.deviceName)
                       << L") -> Demande des conversations SMS..." << std::endl;
            m_protocol.RequestAllConversations(dev);
        });

        // Start protocol handler on UDP 1716 and TCP 1716.
        // If KDEConnectDaemon.exe already owns port 1716, seamlessly fall back to IPC Client Mode!
        bool protocolStarted = m_protocol.Start(bindIp);
        m_isClientMode = !protocolStarted;

        if (!m_isClientMode) {
            m_ipcThread = std::thread(&SmsDaemon::NamedPipeIpcThread, this);
        } else {
            std::wcout << L"[IPC Client] Port 1716 occupe par KDEConnectDaemon. Mode client IPC active." << std::endl;
        }

        m_watcherThread = std::thread(&SmsDaemon::DirectoryWatcherThread, this);

        // If in client mode, kick off a SYNC command to the background daemon
        if (m_isClientMode) {
            std::thread([this]() {
                if (m_running) {
                    SendIpcCommand("SYNC\n");
                }
            }).detach();
        }

        return true;
    }

    void SmsDaemon::Stop() {
        if (!m_running) return;

        m_running = false;

        if (m_dirHandle != INVALID_HANDLE_VALUE) {
            CancelIoEx(m_dirHandle, nullptr);
            CloseHandle(m_dirHandle);
            m_dirHandle = INVALID_HANDLE_VALUE;
        }

        if (!m_isClientMode) {
            m_protocol.Stop();

            // Wake up blocking ConnectNamedPipe if waiting
            HANDLE wakePipe = CreateFileW(
                IPC_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (wakePipe != INVALID_HANDLE_VALUE) {
                CloseHandle(wakePipe);
            }
        }

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
                FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                &bytesReturned,
                nullptr,
                nullptr
            );

            if (!success || bytesReturned == 0 || !m_running) {
                break;
            }

            bool hasJsonChange = false;
            FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            while (fni) {
                std::wstring fileName(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
                if (fileName.ends_with(L".json")) {
                    hasJsonChange = true;
                }

                if (fni->NextEntryOffset == 0) break;
                fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(fni) + fni->NextEntryOffset);
            }

            if (hasJsonChange && m_running) {
                // Brief debounce so atomic moves/writes settle
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                ProcessFolderChangeNotification();
            }
        }
    }

    void SmsDaemon::ProcessFolderChangeNotification() {
        {
            std::lock_guard<std::mutex> lock(m_storageMutex);
            LoadAllConversationsFromDiskLocked();
        }
        NotifyConversationsUpdated();
    }

    bool SmsDaemon::SendIpcCommand(const std::string& command, std::string* outResponse) {
        if (!WaitNamedPipeW(IPC_PIPE_NAME, 1200)) {
            return false;
        }
        HANDLE pipe = CreateFileW(
            IPC_PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        DWORD bytesWritten = 0;
        BOOL wrote = WriteFile(pipe, command.c_str(), static_cast<DWORD>(command.size()), &bytesWritten, nullptr);

        if (wrote && outResponse) {
            char buf[2048];
            DWORD bytesRead = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
                buf[bytesRead] = '\0';
                *outResponse = buf;
            }
        }

        CloseHandle(pipe);
        return wrote == TRUE;
    }

    void SmsDaemon::NamedPipeIpcThread() {
        while (m_running) {
            HANDLE pipe = CreateNamedPipeW(
                IPC_PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                65536,
                65536,
                0,
                nullptr
            );

            if (pipe == INVALID_HANDLE_VALUE) break;

            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected && m_running) {
                std::vector<char> readBuffer(65536, 0);
                DWORD bytesRead = 0;
                if (ReadFile(pipe, readBuffer.data(), static_cast<DWORD>(readBuffer.size() - 1), &bytesRead, nullptr)) {
                    readBuffer[bytesRead] = '\0';
                    std::string cmd(readBuffer.data(), bytesRead);
                    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();

                    std::string response = "{\"status\":\"ok\"}\n";

                    if (cmd == "SYNC") {
                        bool ok = RequestConversationsSync();
                        response = ok ? "{\"status\":\"ok\"}\n" : "{\"status\":\"no_device\"}\n";
                    } else if (cmd.rfind("SYNC_THREAD:", 0) == 0) {
                        int64_t tid = std::strtoll(cmd.substr(12).c_str(), nullptr, 10);
                        bool ok = RequestThreadSync(tid);
                        response = ok ? "{\"status\":\"ok\"}\n" : "{\"status\":\"no_device\"}\n";
                    } else if (cmd.rfind("SEND_SMS|", 0) == 0) {
                        size_t sep1 = cmd.find('|', 9);
                        if (sep1 != std::string::npos) {
                            std::string phoneHex = cmd.substr(9, sep1 - 9);
                            std::string bodyHex = cmd.substr(sep1 + 1);
                            std::wstring phone = Utf8ToWide(HexDecode(phoneHex));
                            std::wstring body = Utf8ToWide(HexDecode(bodyHex));
                            if (!phone.empty() && !body.empty()) {
                                m_protocol.SendSms(DeviceInfo{}, phone, body);
                            }
                        }
                    } else if (cmd == "STATUS") {
                        auto connectedDevs = m_protocol.GetConnectedDevices();
                        std::ostringstream ss;
                        ss << "{\"status\":\"ok\",\"connected\":" << connectedDevs.size() << "}\n";
                        response = ss.str();
                    }

                    DWORD bytesWritten = 0;
                    WriteFile(pipe, response.c_str(), static_cast<DWORD>(response.length()), &bytesWritten, nullptr);
                }
                DisconnectNamedPipe(pipe);
            }
            CloseHandle(pipe);
        }
    }

    bool SmsDaemon::RequestConversationsSync() {
        if (m_isClientMode) {
            return SendIpcCommand("SYNC\n");
        }
        auto devs = m_protocol.GetConnectedDevices();
        if (devs.empty()) return false;
        return m_protocol.RequestAllConversations(devs.front());
    }

    bool SmsDaemon::RequestThreadSync(int64_t threadId) {
        if (threadId <= 0) return false;
        if (m_isClientMode) {
            return SendIpcCommand("SYNC_THREAD:" + std::to_string(threadId) + "\n");
        }
        auto devs = m_protocol.GetConnectedDevices();
        if (devs.empty()) return false;
        return m_protocol.RequestConversationMessages(devs.front(), threadId, -1);
    }

    std::wstring SmsDaemon::GetConnectionStatusText() const {
        if (m_isClientMode) {
            return L"Connecte au service Daemon (IPC actif)";
        }
        auto devs = m_protocol.GetConnectedDevices();
        if (devs.empty()) {
            return L"En attente de connexion du telephone sur le port 1716...";
        }
        const auto& d = devs.front();
        std::wstring name = d.deviceName.empty() ? d.ipAddress : d.deviceName;
        return L"Connecte a " + name + (d.isPaired ? L" (Appaire)" : L" (En attente d'appairage)");
    }

    void SmsDaemon::HandleIncomingKdePacket(const DeviceInfo& device, const KDEPacket& packet) {
        if (packet.type == "kdeconnect.sms.messages") {
            std::string messagesArrayJson;
            ForEachJsonObjectField(packet.bodyJson, [&](const std::string& key, const std::string& val, bool) {
                if (key == "messages") {
                    messagesArrayJson = val;
                }
            });

            if (messagesArrayJson.empty()) {
                std::wcout << L"[SMS Sync] Paquet kdeconnect.sms.messages recu mais liste 'messages' vide." << std::endl;
                return;
            }

            std::vector<Message> parsedMessages;
            std::set<int64_t> threadsInPacket;

            ForEachJsonArrayObject(messagesArrayJson, [&](const std::string& msgObj) {
                Message msg;
                int smsType = 1;
                bool hasAttachments = false;

                ForEachJsonObjectField(msgObj, [&](const std::string& key, const std::string& val, bool) {
                    if (key == "_id") {
                        msg.id = std::strtoll(val.c_str(), nullptr, 10);
                    } else if (key == "thread_id") {
                        msg.threadId = std::strtoll(val.c_str(), nullptr, 10);
                    } else if (key == "body") {
                        msg.body = Utf8ToWide(UnescapeJsonString(val));
                    } else if (key == "date") {
                        msg.timestamp = std::strtoll(val.c_str(), nullptr, 10);
                    } else if (key == "type") {
                        smsType = std::atoi(val.c_str());
                    } else if (key == "read") {
                        msg.isRead = (val == "1" || val == "true");
                    } else if (key == "address" && msg.address.empty()) {
                        msg.address = Utf8ToWide(UnescapeJsonString(val));
                    } else if (key == "addresses") {
                        std::wstring joinedAddresses;
                        ForEachJsonArrayObject(val, [&](const std::string& addrObj) {
                            ForEachJsonObjectField(addrObj, [&](const std::string& aKey, const std::string& aVal, bool) {
                                if (aKey == "address" && !aVal.empty()) {
                                    std::wstring wAddr = Utf8ToWide(UnescapeJsonString(aVal));
                                    if (!joinedAddresses.empty()) joinedAddresses += L", ";
                                    joinedAddresses += wAddr;
                                }
                            });
                        });
                        if (!joinedAddresses.empty()) {
                            msg.address = joinedAddresses;
                        }
                    } else if (key == "attachments") {
                        if (val.size() > 2 && val.find('{') != std::string::npos) {
                            hasAttachments = true;
                        }
                    }
                });

                msg.isOutgoing = (smsType == 2 || smsType == 4 || smsType == 5 || smsType == 6);
                if (msg.body.empty() && hasAttachments) {
                    msg.body = L"[Piece jointe MMS]";
                }
                if (msg.threadId <= 0 && !msg.address.empty()) {
                    msg.threadId = static_cast<int64_t>(std::hash<std::wstring>{}(msg.address) % 1000000) + 1;
                }
                if (msg.id == 0) {
                    msg.id = msg.timestamp ^ (msg.threadId << 16);
                }

                if (msg.threadId > 0 && (!msg.body.empty() || !msg.address.empty())) {
                    threadsInPacket.insert(msg.threadId);
                    parsedMessages.push_back(std::move(msg));
                }
            });

            std::wcout << L"[SMS Sync] " << parsedMessages.size() << L" message(s) recu(s) dans "
                       << threadsInPacket.size() << L" conversation(s)." << std::endl;

            if (!parsedMessages.empty()) {
                SaveMessagesBatchToDisk(parsedMessages);
                NotifyConversationsUpdated();

                // When request_conversations returns multiple threads (1 latest message each),
                // automatically fetch full message history for the most recent threads!
                std::vector<int64_t> threadsToFetch;
                {
                    std::lock_guard<std::mutex> lock(m_storageMutex);
                    std::vector<std::pair<int64_t, int64_t>> candidates; // (lastTimestamp, threadId)
                    for (int64_t tid : threadsInPacket) {
                        if (m_requestedThreads.find(tid) == m_requestedThreads.end()) {
                            auto it = m_conversations.find(tid);
                            if (it != m_conversations.end() && it->second.messages.size() <= 1) {
                                candidates.emplace_back(it->second.lastTimestamp, tid);
                            }
                        }
                    }
                    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                        return a.first > b.first;
                    });
                    size_t limit = std::min<size_t>(candidates.size(), 15);
                    for (size_t i = 0; i < limit; ++i) {
                        m_requestedThreads.insert(candidates[i].second);
                        threadsToFetch.push_back(candidates[i].second);
                    }
                }

                if (!threadsToFetch.empty()) {
                    std::thread([this, device, threadsToFetch = std::move(threadsToFetch)]() {
                        for (int64_t tid : threadsToFetch) {
                            if (!m_running) break;
                            m_protocol.RequestConversationMessages(device, tid, -1);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    }).detach();
                }
            }
        } else if (packet.type == "kdeconnect.telephony") {
            std::string eventType;
            std::string phone;
            std::string body;
            std::string contactName;

            ForEachJsonObjectField(packet.bodyJson, [&](const std::string& key, const std::string& val, bool) {
                if (key == "event") eventType = UnescapeJsonString(val);
                else if (key == "phoneNumber") phone = UnescapeJsonString(val);
                else if (key == "messageBody") body = UnescapeJsonString(val);
                else if (key == "contactName") contactName = UnescapeJsonString(val);
            });

            if (!phone.empty() && !body.empty()) {
                std::wstring wPhone = Utf8ToWide(phone);
                std::wstring wName = Utf8ToWide(contactName);
                std::wstring wSuffix = NormalizePhoneSuffix(wPhone);

                int64_t matchedThreadId = -1;
                {
                    std::lock_guard<std::mutex> lock(m_storageMutex);
                    for (auto& kv : m_conversations) {
                        if (!wSuffix.empty() && NormalizePhoneSuffix(kv.second.address) == wSuffix) {
                            matchedThreadId = kv.first;
                            if (!wName.empty()) kv.second.contactName = wName;
                            break;
                        }
                    }
                }
                if (matchedThreadId < 0) {
                    matchedThreadId = static_cast<int64_t>(std::hash<std::string>{}(phone) % 100000) + 1;
                }

                Message msg;
                msg.id = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                msg.threadId = matchedThreadId;
                msg.address = wPhone;
                msg.body = Utf8ToWide(body);
                msg.timestamp = msg.id;
                msg.isOutgoing = false;
                msg.isRead = false;

                SaveMessageToDisk(msg);
                NotifyConversationsUpdated();

                // Fire native Windows Toast Notification
                ToastNotifier::Instance().ShowSmsNotification(
                    wName.empty() ? msg.address : wName,
                    msg.body,
                    msg.threadId
                );

                // Refresh synced conversations from phone as well
                m_protocol.RequestAllConversations(device);
            }
        }
    }

    void SmsDaemon::SaveMessageToDisk(const Message& msg) {
        SaveMessagesBatchToDisk(std::vector<Message>{ msg });
    }

    void SmsDaemon::SaveMessagesBatchToDisk(const std::vector<Message>& messages) {
        if (messages.empty()) return;

        std::lock_guard<std::mutex> lock(m_storageMutex);
        std::set<int64_t> dirtyThreads;

        for (const auto& msg : messages) {
            auto& conv = m_conversations[msg.threadId];
            conv.threadId = msg.threadId;
            if (!msg.address.empty() && conv.address.empty()) {
                conv.address = msg.address;
            } else if (!msg.address.empty() && msg.address.size() > conv.address.size() && conv.contactName == conv.address) {
                conv.address = msg.address;
            }
            if (conv.contactName.empty()) {
                conv.contactName = conv.address.empty() ? (L"Fil #" + std::to_wstring(msg.threadId)) : conv.address;
            }

            // Deduplicate by real Android _id or matching optimistic outgoing message
            bool updated = false;
            for (auto& existing : conv.messages) {
                if (existing.id == msg.id && msg.id != 0) {
                    existing.body = msg.body;
                    existing.timestamp = msg.timestamp;
                    existing.isOutgoing = msg.isOutgoing;
                    existing.isRead = msg.isRead;
                    if (!msg.address.empty()) existing.address = msg.address;
                    updated = true;
                    break;
                }
                if (existing.isOutgoing == msg.isOutgoing &&
                    existing.body == msg.body &&
                    std::llabs(existing.timestamp - msg.timestamp) < 60000) {
                    existing.id = msg.id;
                    existing.timestamp = msg.timestamp;
                    existing.isRead = msg.isRead;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                conv.messages.push_back(msg);
            }
            dirtyThreads.insert(msg.threadId);
        }

        for (int64_t tid : dirtyThreads) {
            auto& conv = m_conversations[tid];
            std::sort(conv.messages.begin(), conv.messages.end(), [](const Message& a, const Message& b) {
                if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                return a.id < b.id;
            });

            conv.unreadCount = 0;
            for (const auto& m : conv.messages) {
                if (!m.isOutgoing && !m.isRead) {
                    conv.unreadCount++;
                }
            }
            if (!conv.messages.empty()) {
                conv.lastTimestamp = conv.messages.back().timestamp;
                conv.snippet = conv.messages.back().body;
            }

            std::wstring filePath = m_watchDir + L"\\thread_" + std::to_wstring(tid) + L".json";
            std::wstring tmpPath = filePath + L".tmp";

            {
                std::ofstream file(std::filesystem::path(tmpPath), std::ios::out | std::ios::trunc);
                if (file.is_open()) {
                    file << "{\n"
                         << "  \"threadId\": " << conv.threadId << ",\n"
                         << "  \"address\": \"" << EscapeJson(WideToUtf8(conv.address)) << "\",\n"
                         << "  \"contactName\": \"" << EscapeJson(WideToUtf8(conv.contactName)) << "\",\n"
                         << "  \"messages\": [\n";

                    for (size_t i = 0; i < conv.messages.size(); ++i) {
                        const auto& m = conv.messages[i];
                        file << "    {\n"
                             << "      \"id\": " << m.id << ",\n"
                             << "      \"address\": \"" << EscapeJson(WideToUtf8(m.address)) << "\",\n"
                             << "      \"body\": \"" << EscapeJson(WideToUtf8(m.body)) << "\",\n"
                             << "      \"timestamp\": " << m.timestamp << ",\n"
                             << "      \"isOutgoing\": " << (m.isOutgoing ? "true" : "false") << ",\n"
                             << "      \"isRead\": " << (m.isRead ? "true" : "false") << "\n"
                             << "    }" << (i + 1 < conv.messages.size() ? "," : "") << "\n";
                    }
                    file << "  ]\n}\n";
                    file.close();
                }
            }

            MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }

    void SmsDaemon::LoadAllConversationsFromDiskLocked() {
        std::map<int64_t, Conversation> loadedFromDisk;

        try {
            std::filesystem::path dirPath(m_watchDir);
            if (!std::filesystem::exists(dirPath)) return;

            for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
                if (!entry.is_regular_file()) continue;
                std::wstring filename = entry.path().filename().wstring();
                if (filename.rfind(L"thread_", 0) != 0 || !filename.ends_with(L".json")) continue;

                std::ifstream file(entry.path(), std::ios::in | std::ios::binary);
                if (!file.is_open()) continue;

                std::string jsonContent((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (jsonContent.empty()) continue;

                Conversation conv;
                std::string messagesArr;

                ForEachJsonObjectField(jsonContent, [&](const std::string& key, const std::string& val, bool) {
                    if (key == "threadId") conv.threadId = std::strtoll(val.c_str(), nullptr, 10);
                    else if (key == "address") conv.address = Utf8ToWide(UnescapeJsonString(val));
                    else if (key == "contactName") conv.contactName = Utf8ToWide(UnescapeJsonString(val));
                    else if (key == "messages") messagesArr = val;
                });

                if (conv.threadId <= 0) continue;
                if (conv.contactName.empty()) {
                    conv.contactName = conv.address.empty() ? (L"Fil #" + std::to_wstring(conv.threadId)) : conv.address;
                }

                if (!messagesArr.empty()) {
                    ForEachJsonArrayObject(messagesArr, [&](const std::string& msgObj) {
                        Message m;
                        m.threadId = conv.threadId;
                        m.address = conv.address;
                        ForEachJsonObjectField(msgObj, [&](const std::string& key, const std::string& val, bool) {
                            if (key == "id") m.id = std::strtoll(val.c_str(), nullptr, 10);
                            else if (key == "address") m.address = Utf8ToWide(UnescapeJsonString(val));
                            else if (key == "body") m.body = Utf8ToWide(UnescapeJsonString(val));
                            else if (key == "timestamp") m.timestamp = std::strtoll(val.c_str(), nullptr, 10);
                            else if (key == "isOutgoing") m.isOutgoing = (val == "true" || val == "1");
                            else if (key == "isRead") m.isRead = (val == "true" || val == "1");
                        });
                        if (m.timestamp > 0 || !m.body.empty()) {
                            conv.messages.push_back(std::move(m));
                        }
                    });
                }

                std::sort(conv.messages.begin(), conv.messages.end(), [](const Message& a, const Message& b) {
                    if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
                    return a.id < b.id;
                });

                if (!conv.messages.empty()) {
                    conv.lastTimestamp = conv.messages.back().timestamp;
                    conv.snippet = conv.messages.back().body;
                }
                for (const auto& m : conv.messages) {
                    if (!m.isOutgoing && !m.isRead) conv.unreadCount++;
                }

                loadedFromDisk[conv.threadId] = std::move(conv);
            }
        } catch (...) {
            // Keep existing in-memory cache if filesystem scan races
            return;
        }

        if (!loadedFromDisk.empty()) {
            m_conversations = std::move(loadedFromDisk);
        }
    }

    std::vector<Conversation> SmsDaemon::LoadAllConversations() {
        std::lock_guard<std::mutex> lock(m_storageMutex);

        if (m_conversations.empty()) {
            LoadAllConversationsFromDiskLocked();
        }

        std::vector<Conversation> result;
        result.reserve(m_conversations.size());
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

    bool SmsDaemon::SendOutgoingSms(const std::wstring& phoneNumber, const std::wstring& text, int64_t preferredThreadId) {
        if (phoneNumber.empty() || text.empty()) return false;

        int64_t threadId = preferredThreadId;
        if (threadId <= 0) {
            std::wstring suffix = NormalizePhoneSuffix(phoneNumber);
            std::lock_guard<std::mutex> lock(m_storageMutex);
            for (const auto& kv : m_conversations) {
                if ((!suffix.empty() && NormalizePhoneSuffix(kv.second.address) == suffix) ||
                    kv.second.address == phoneNumber) {
                    threadId = kv.first;
                    break;
                }
            }
        }
        if (threadId <= 0) {
            threadId = static_cast<int64_t>(std::hash<std::string>{}(WideToUtf8(phoneNumber)) % 100000) + 1;
        }

        Message msg;
        msg.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        msg.threadId = threadId;
        msg.address = phoneNumber;
        msg.body = text;
        msg.timestamp = msg.id;
        msg.isOutgoing = true;
        msg.isRead = true;

        SaveMessageToDisk(msg);
        NotifyConversationsUpdated();

        if (m_isClientMode) {
            std::string cmd = "SEND_SMS|" + HexEncode(WideToUtf8(phoneNumber)) + "|" + HexEncode(WideToUtf8(text)) + "\n";
            return SendIpcCommand(cmd);
        }

        return m_protocol.SendSms(DeviceInfo{}, phoneNumber, text);
    }

} // namespace KDEConnect
