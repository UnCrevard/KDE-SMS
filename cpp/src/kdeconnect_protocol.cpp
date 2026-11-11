#include "kdeconnect_protocol.h"
#include <iostream>

namespace KDEConnect {

    // Simple JSON helpers for KDE Connect packet framing
    static std::string EscapeJsonString(const std::string& input) {
        std::ostringstream ss;
        for (char c : input) {
            switch (c) {
                case '"': ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if ('\x00' <= c && c <= '\x1f') {
                        ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                    } else {
                        ss << c;
                    }
            }
        }
        return ss.str();
    }

    std::string KDEPacket::Serialize() const {
        std::ostringstream oss;
        oss << "{\"id\":" << id
            << ",\"type\":\"" << EscapeJsonString(type) << "\""
            << ",\"body\":" << (bodyJson.empty() ? "{}" : bodyJson)
            << "}\n";
        return oss.str();
    }

    bool KDEPacket::Deserialize(const std::string& raw, KDEPacket& outPacket) {
        if (raw.empty()) return false;

        // Basic robust JSON scanning for id, type and body
        size_t idPos = raw.find("\"id\":");
        if (idPos != std::string::npos) {
            size_t idEnd = raw.find_first_of(",}\r\n", idPos + 5);
            std::string idStr = raw.substr(idPos + 5, idEnd - (idPos + 5));
            outPacket.id = _strtoi64(idStr.c_str(), nullptr, 10);
        }

        size_t typePos = raw.find("\"type\":");
        if (typePos != std::string::npos) {
            size_t quoteStart = raw.find('"', typePos + 7);
            if (quoteStart != std::string::npos) {
                size_t quoteEnd = raw.find('"', quoteStart + 1);
                if (quoteEnd != std::string::npos) {
                    outPacket.type = raw.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                }
            }
        }

        size_t bodyPos = raw.find("\"body\":");
        if (bodyPos != std::string::npos) {
            size_t startBrace = raw.find('{', bodyPos);
            if (startBrace != std::string::npos) {
                int braceCount = 0;
                size_t endBrace = startBrace;
                for (size_t i = startBrace; i < raw.size(); ++i) {
                    if (raw[i] == '{') braceCount++;
                    else if (raw[i] == '}') {
                        braceCount--;
                        if (braceCount == 0) {
                            endBrace = i;
                            break;
                        }
                    }
                }
                outPacket.bodyJson = raw.substr(startBrace, endBrace - startBrace + 1);
            }
        }

        return !outPacket.type.empty();
    }

    ProtocolHandler::ProtocolHandler() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        // Generate local identity
        wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(computerName, &size);

        m_localIdentity.deviceId = L"win32-kdeconnect-sms-client";
        m_localIdentity.deviceName = computerName;
        m_localIdentity.deviceType = L"desktop";
        m_localIdentity.isPaired = true;
        m_localIdentity.isReachable = true;
    }

    ProtocolHandler::~ProtocolHandler() {
        Stop();
        WSACleanup();
    }

    bool ProtocolHandler::Start(uint16_t port) {
        m_port = port;
        m_running = true;

        // Create UDP Discovery Socket
        m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_udpSocket == INVALID_SOCKET) return false;

        BOOL opt = TRUE;
        setsockopt(m_udpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));
        setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(m_port);
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_udpSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
            return false;
        }

        // Create TCP Listening Socket
        m_tcpListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_tcpListenSocket != INVALID_SOCKET) {
            setsockopt(m_tcpListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
            if (bind(m_tcpListenSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) != SOCKET_ERROR) {
                listen(m_tcpListenSocket, SOMAXCONN);
            }
        }

        // Start worker threads
        m_udpThread = std::thread(&ProtocolHandler::UdpDiscoveryThread, this);
        m_tcpThread = std::thread(&ProtocolHandler::TcpServerThread, this);

        // Broadcast initial presence
        BroadcastIdentity();
        return true;
    }

    void ProtocolHandler::Stop() {
        m_running = false;

        if (m_udpSocket != INVALID_SOCKET) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
        }

        if (m_tcpListenSocket != INVALID_SOCKET) {
            closesocket(m_tcpListenSocket);
            m_tcpListenSocket = INVALID_SOCKET;
        }

        if (m_udpThread.joinable()) m_udpThread.join();
        if (m_tcpThread.joinable()) m_tcpThread.join();
    }

    void ProtocolHandler::BroadcastIdentity() {
        if (m_udpSocket == INVALID_SOCKET) return;

        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.identity";

        std::ostringstream ss;
        ss << "{"
           << "\"deviceId\":\"" << WideToUtf8(m_localIdentity.deviceId) << "\","
           << "\"deviceName\":\"" << WideToUtf8(m_localIdentity.deviceName) << "\","
           << "\"deviceType\":\"desktop\","
           << "\"protocolVersion\":7,"
           << "\"incomingCapabilities\":[\"kdeconnect.telephony\",\"kdeconnect.sms\",\"kdeconnect.battery\"],"
           << "\"outgoingCapabilities\":[\"kdeconnect.sms.request\",\"kdeconnect.sms.request_conversations\"]"
           << "}";
        pkt.bodyJson = ss.str();

        std::string raw = pkt.Serialize();

        sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(m_port);
        bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

        sendto(m_udpSocket, raw.c_str(), static_cast<int>(raw.length()), 0, (sockaddr*)&bcastAddr, sizeof(bcastAddr));
    }

    void ProtocolHandler::UdpDiscoveryThread() {
        char buffer[4096];
        sockaddr_in senderAddr{};
        int senderLen = sizeof(senderAddr);

        while (m_running) {
            int received = recvfrom(m_udpSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&senderAddr, &senderLen);
            if (received <= 0) break;

            buffer[received] = '\0';
            KDEPacket pkt;
            if (KDEPacket::Deserialize(buffer, pkt)) {
                if (pkt.type == "kdeconnect.identity") {
                    char senderIp[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &senderAddr.sin_addr, senderIp, sizeof(senderIp));

                    DeviceInfo dev;
                    dev.ipAddress = Utf8ToWide(senderIp);
                    dev.port = ntohs(senderAddr.sin_port);

                    // Extract deviceId and deviceName from bodyJson
                    size_t dId = pkt.bodyJson.find("\"deviceId\":");
                    if (dId != std::string::npos) {
                        size_t q1 = pkt.bodyJson.find('"', dId + 11);
                        size_t q2 = pkt.bodyJson.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos) {
                            dev.deviceId = Utf8ToWide(pkt.bodyJson.substr(q1 + 1, q2 - q1 - 1));
                        }
                    }

                    size_t dName = pkt.bodyJson.find("\"deviceName\":");
                    if (dName != std::string::npos) {
                        size_t q1 = pkt.bodyJson.find('"', dName + 13);
                        size_t q2 = pkt.bodyJson.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos) {
                            dev.deviceName = Utf8ToWide(pkt.bodyJson.substr(q1 + 1, q2 - q1 - 1));
                        }
                    }

                    // Ignore self broadcasts
                    if (dev.deviceId != m_localIdentity.deviceId) {
                        std::lock_guard<std::mutex> lock(m_devicesMutex);
                        auto it = std::find_if(m_knownDevices.begin(), m_knownDevices.end(), [&](const DeviceInfo& d) {
                            return d.deviceId == dev.deviceId;
                        });
                        if (it == m_knownDevices.end()) {
                            m_knownDevices.push_back(dev);
                            if (m_onDeviceDiscovered) m_onDeviceDiscovered(dev);
                        }
                    }
                }
            }
        }
    }

    void ProtocolHandler::TcpServerThread() {
        while (m_running && m_tcpListenSocket != INVALID_SOCKET) {
            sockaddr_in clientAddr{};
            int clientLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(m_tcpListenSocket, (sockaddr*)&clientAddr, &clientLen);
            if (clientSocket == INVALID_SOCKET) break;

            // Handle client packet reading in detached worker
            std::thread(&ProtocolHandler::HandleClientConnection, this, clientSocket).detach();
        }
    }

    void ProtocolHandler::HandleClientConnection(SOCKET clientSocket) {
        std::string bufferAccumulator;
        char buffer[4096];

        while (m_running) {
            int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) break;

            buffer[bytesRead] = '\0';
            bufferAccumulator += buffer;

            // Packets are delimited by newline \n
            size_t newlinePos;
            while ((newlinePos = bufferAccumulator.find('\n')) != std::string::npos) {
                std::string packetLine = bufferAccumulator.substr(0, newlinePos);
                bufferAccumulator.erase(0, newlinePos + 1);

                KDEPacket pkt;
                if (KDEPacket::Deserialize(packetLine, pkt)) {
                    DeviceInfo dev;
                    if (m_onPacketReceived) {
                        m_onPacketReceived(dev, pkt);
                    }
                }
            }
        }
        closesocket(clientSocket);
    }

    bool ProtocolHandler::SendPacket(const DeviceInfo& device, const KDEPacket& packet) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return false;

        sockaddr_in destAddr{};
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(device.port);
        std::string ipUtf8 = WideToUtf8(device.ipAddress);
        inet_pton(AF_INET, ipUtf8.c_str(), &destAddr.sin_addr);

        if (connect(sock, (sockaddr*)&destAddr, sizeof(destAddr)) == SOCKET_ERROR) {
            closesocket(sock);
            return false;
        }

        std::string raw = packet.Serialize();
        send(sock, raw.c_str(), static_cast<int>(raw.length()), 0);
        closesocket(sock);
        return true;
    }

    bool ProtocolHandler::RequestAllConversations(const DeviceInfo& device) {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.sms.request_conversations";
        pkt.bodyJson = "{}";
        return SendPacket(device, pkt);
    }

    bool ProtocolHandler::RequestConversationMessages(const DeviceInfo& device, int64_t threadId, int64_t rangeStart) {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.sms.request_conversation";
        std::ostringstream ss;
        ss << "{\"threadID\":" << threadId << ",\"rangeStartTimestamp\":" << rangeStart << "}";
        pkt.bodyJson = ss.str();
        return SendPacket(device, pkt);
    }

    bool ProtocolHandler::SendSms(const DeviceInfo& device, const std::wstring& phoneNumber, const std::wstring& text) {
        KDEPacket pkt;
        pkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        pkt.type = "kdeconnect.sms.request";

        std::ostringstream ss;
        ss << "{"
           << "\"sendSms\":true,"
           << "\"phoneNumber\":\"" << EscapeJsonString(WideToUtf8(phoneNumber)) << "\","
           << "\"messageBody\":\"" << EscapeJsonString(WideToUtf8(text)) << "\""
           << "}";
        pkt.bodyJson = ss.str();
        return SendPacket(device, pkt);
    }

} // namespace KDEConnect
