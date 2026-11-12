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
            outPacket.id = std::strtoll(idStr.c_str(), nullptr, 10);
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
        if (!GetComputerNameW(computerName, &size)) {
            wcscpy_s(computerName, L"WindowsPC");
        }

        // Generate a standard 32-character hexadecimal deviceId derived from computer and user name
        wchar_t userName[256];
        DWORD userSize = 256;
        if (!GetUserNameW(userName, &userSize)) {
            wcscpy_s(userName, L"User");
        }

        std::string seed = WideToUtf8(computerName) + "_" + WideToUtf8(userName);
        uint64_t h1 = 0xcbf29ce484222325ULL;
        uint64_t h2 = 0x100000001b3ULL;
        for (char c : seed) {
            h1 = (h1 ^ static_cast<uint8_t>(c)) * 0x100000001b3ULL;
            h2 = (h2 + static_cast<uint8_t>(c)) * 0xcbf29ce484222325ULL;
        }
        std::ostringstream hexId;
        hexId << std::hex << std::setfill('0') << std::setw(16) << h1 << std::setw(16) << h2;

        m_localIdentity.deviceId = Utf8ToWide(hexId.str());
        m_localIdentity.deviceName = computerName;
        m_localIdentity.deviceType = L"desktop";
        m_localIdentity.isPaired = true;
        m_localIdentity.isReachable = true;
    }

    ProtocolHandler::~ProtocolHandler() {
        Stop();
        WSACleanup();
    }

    std::vector<std::wstring> ProtocolHandler::GetLocalIpAddresses() {
        std::vector<std::wstring> ips;
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(hostname, nullptr, &hints, &res) == 0) {
                for (addrinfo* ptr = res; ptr != nullptr; ptr = ptr->ai_next) {
                    sockaddr_in* s = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
                    char ipStr[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, &s->sin_addr, ipStr, sizeof(ipStr))) {
                        std::string str(ipStr);
                        if (str != "127.0.0.1") {
                            ips.push_back(Utf8ToWide(str));
                        }
                    }
                }
                freeaddrinfo(res);
            }
        }
        return ips;
    }

    bool ProtocolHandler::Start(const std::wstring& bindIp, uint16_t port) {
        m_bindIp = bindIp;
        m_port = port;
        m_running = true;

        // Create UDP Discovery Socket
        m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_udpSocket == INVALID_SOCKET) {
            std::wcerr << L"[ERREUR UDP] Impossible de creer le socket UDP (code " << WSAGetLastError() << L")" << std::endl;
            return false;
        }

        BOOL opt = TRUE;
        setsockopt(m_udpSocket, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));
        setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        // CRITICAL WINSOCK RULE:
        // On Windows, the receiving socket MUST be bound to INADDR_ANY (0.0.0.0).
        // If bound to a specific IP, Windows rejects all incoming packets sent to 255.255.255.255!
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(m_port);
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_udpSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
            int err = WSAGetLastError();
            std::wcerr << L"[ERREUR UDP] Echec bind sur 0.0.0.0:" << m_port << L" (code " << err << L")" << std::endl;
            if (err == 10048) {
                std::wcerr << L"             -> Le port 1716 est deja utilise ! Fermez l'application KDE Connect officielle." << std::endl;
            }
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
            return false;
        } else {
            std::wcout << L"[UDP Serveur] Ecoute active sur 0.0.0.0:" << m_port << L" (Reception diffusions OK)" << std::endl;
        }

        // Create TCP Listening Socket
        m_tcpListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_tcpListenSocket != INVALID_SOCKET) {
            setsockopt(m_tcpListenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
            sockaddr_in tcpBindAddr{};
            tcpBindAddr.sin_family = AF_INET;
            tcpBindAddr.sin_port = htons(m_port);
            tcpBindAddr.sin_addr.s_addr = INADDR_ANY;

            if (bind(m_tcpListenSocket, (sockaddr*)&tcpBindAddr, sizeof(tcpBindAddr)) == SOCKET_ERROR) {
                std::wcerr << L"[ERREUR TCP] Echec bind sur port " << m_port << L" (code " << WSAGetLastError() << L")" << std::endl;
            } else {
                listen(m_tcpListenSocket, SOMAXCONN);
                std::wcout << L"[TCP Serveur] Ecoute active sur 0.0.0.0:" << m_port << L" (Pret pour appairage et donnees)" << std::endl;
            }
        }

        // Start worker threads
        m_udpThread = std::thread(&ProtocolHandler::UdpDiscoveryThread, this);
        m_tcpThread = std::thread(&ProtocolHandler::TcpServerThread, this);
        m_broadcastThread = std::thread(&ProtocolHandler::PeriodicBroadcastThread, this);

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

        if (m_broadcastThread.joinable()) m_broadcastThread.join();
        if (m_udpThread.joinable()) m_udpThread.join();
        if (m_tcpThread.joinable()) m_tcpThread.join();
    }

    void ProtocolHandler::PeriodicBroadcastThread() {
        while (m_running) {
            BroadcastIdentity();
            for (int i = 0; i < 8 && m_running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
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
           << "\"protocolVersion\":8,"
           << "\"tcpPort\":" << m_port << ","
           << "\"incomingCapabilities\":[\"kdeconnect.telephony\",\"kdeconnect.sms\",\"kdeconnect.battery\"],"
           << "\"outgoingCapabilities\":[\"kdeconnect.sms.request\",\"kdeconnect.sms.request_conversations\"]"
           << "}";
        pkt.bodyJson = ss.str();

        std::string raw = pkt.Serialize();

        // Target interfaces
        std::vector<std::string> targetIps;
        if (!m_bindIp.empty()) {
            targetIps.push_back(WideToUtf8(m_bindIp));
        } else {
            auto ips = GetLocalIpAddresses();
            for (const auto& w : ips) targetIps.push_back(WideToUtf8(w));
        }

        // 1. Send via dedicated ephemeral sender sockets bound to specific interface(s)
        for (const auto& ipUtf8 : targetIps) {
            in_addr localInAddr{};
            if (inet_pton(AF_INET, ipUtf8.c_str(), &localInAddr) == 1) {
                uint32_t hostOrderIp = ntohl(localInAddr.s_addr);
                uint32_t subnetBcast = (hostOrderIp & 0xFFFFFF00) | 0x000000FF;

                in_addr bcastAddrIn{};
                bcastAddrIn.s_addr = htonl(subnetBcast);
                char bcastIpStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &bcastAddrIn, bcastIpStr, sizeof(bcastIpStr));

                SOCKET senderSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (senderSock != INVALID_SOCKET) {
                    BOOL opt = TRUE;
                    setsockopt(senderSock, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));

                    sockaddr_in ifAddr{};
                    ifAddr.sin_family = AF_INET;
                    ifAddr.sin_port = 0; // ephemeral port
                    ifAddr.sin_addr = localInAddr;

                    if (bind(senderSock, (sockaddr*)&ifAddr, sizeof(ifAddr)) != SOCKET_ERROR) {
                        sockaddr_in destSubnet{};
                        destSubnet.sin_family = AF_INET;
                        destSubnet.sin_port = htons(m_port);
                        destSubnet.sin_addr.s_addr = htonl(subnetBcast);

                        int res = sendto(senderSock, raw.c_str(), static_cast<int>(raw.length()), 0,
                                         (sockaddr*)&destSubnet, sizeof(destSubnet));
                        if (res != SOCKET_ERROR) {
                            std::wcout << L"[UDP Annonce] Envoyee sur " << Utf8ToWide(bcastIpStr) 
                                       << L":" << m_port << L" (via " << Utf8ToWide(ipUtf8) << L") [OK]" << std::endl;
                        } else {
                            std::wcout << L"[UDP Annonce] Echec sur " << Utf8ToWide(bcastIpStr) 
                                       << L" (code " << WSAGetLastError() << L")" << std::endl;
                        }

                        // Also send global broadcast from this interface
                        sockaddr_in destGlobal{};
                        destGlobal.sin_family = AF_INET;
                        destGlobal.sin_port = htons(m_port);
                        destGlobal.sin_addr.s_addr = INADDR_BROADCAST;
                        sendto(senderSock, raw.c_str(), static_cast<int>(raw.length()), 0,
                               (sockaddr*)&destGlobal, sizeof(destGlobal));
                    }
                    closesocket(senderSock);
                }
            }
        }

        // 2. Global broadcast fallback from listener socket
        sockaddr_in globalAddr{};
        globalAddr.sin_family = AF_INET;
        globalAddr.sin_port = htons(m_port);
        globalAddr.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(m_udpSocket, raw.c_str(), static_cast<int>(raw.length()), 0,
               (sockaddr*)&globalAddr, sizeof(globalAddr));
    }

    void ProtocolHandler::UdpDiscoveryThread() {
        char buffer[4096];
        sockaddr_in senderAddr{};
        int senderLen = sizeof(senderAddr);

        while (m_running) {
            int received = recvfrom(m_udpSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&senderAddr, &senderLen);
            if (received <= 0) {
                if (!m_running) break;
                continue;
            }

            buffer[received] = '\0';
            KDEPacket pkt;
            if (KDEPacket::Deserialize(buffer, pkt)) {
                char senderIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &senderAddr.sin_addr, senderIp, sizeof(senderIp));
                std::string ipStr(senderIp);

                if (pkt.type == "kdeconnect.identity") {
                    DeviceInfo dev;
                    dev.ipAddress = Utf8ToWide(ipStr);
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

                    // Extract custom tcpPort advertised by phone
                    size_t pPort = pkt.bodyJson.find("\"tcpPort\":");
                    if (pPort != std::string::npos) {
                        size_t portEnd = pkt.bodyJson.find_first_of(",}\r\n", pPort + 10);
                        std::string portStr = pkt.bodyJson.substr(pPort + 10, portEnd - (pPort + 10));
                        int parsedPort = std::atoi(portStr.c_str());
                        if (parsedPort > 0) dev.port = static_cast<uint16_t>(parsedPort);
                    }

                    // Ignore self broadcasts
                    if (dev.deviceId != m_localIdentity.deviceId) {
                        std::wcout << L"\n[UDP Recu] Paquet decouvert de " << dev.ipAddress 
                                   << L":" << ntohs(senderAddr.sin_port) 
                                   << L" | Appareil: " << (dev.deviceName.empty() ? L"Telephone" : dev.deviceName) 
                                   << L" (ID: " << dev.deviceId << L")" << std::endl;

                        // Immediately respond directly to phone's IP so phone instantly shows our PC
                        KDEPacket myPkt;
                        myPkt.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        myPkt.type = "kdeconnect.identity";
                        std::ostringstream respSs;
                        respSs << "{"
                               << "\"deviceId\":\"" << WideToUtf8(m_localIdentity.deviceId) << "\","
                               << "\"deviceName\":\"" << WideToUtf8(m_localIdentity.deviceName) << "\","
                               << "\"deviceType\":\"desktop\","
                               << "\"protocolVersion\":8,"
                               << "\"tcpPort\":" << m_port << ","
                               << "\"incomingCapabilities\":[\"kdeconnect.telephony\",\"kdeconnect.sms\",\"kdeconnect.battery\"],"
                               << "\"outgoingCapabilities\":[\"kdeconnect.sms.request\",\"kdeconnect.sms.request_conversations\"]"
                               << "}";
                        myPkt.bodyJson = respSs.str();
                        std::string rawResp = myPkt.Serialize();

                        int r1 = sendto(m_udpSocket, rawResp.c_str(), static_cast<int>(rawResp.length()), 0, (sockaddr*)&senderAddr, sizeof(senderAddr));
                        sockaddr_in target1716 = senderAddr;
                        target1716.sin_port = htons(m_port);
                        int r2 = sendto(m_udpSocket, rawResp.c_str(), static_cast<int>(rawResp.length()), 0, (sockaddr*)&target1716, sizeof(target1716));

                        std::wcout << L"[UDP Reponse] Identite du PC envoyee directement vers " << dev.ipAddress 
                                   << L":" << ntohs(senderAddr.sin_port) << L" et :" << m_port 
                                   << L" (Statut: " << (r1 != SOCKET_ERROR || r2 != SOCKET_ERROR ? L"OK" : L"Echec") << L")" << std::endl;

                        std::lock_guard<std::mutex> lock(m_devicesMutex);
                        auto it = std::find_if(m_knownDevices.begin(), m_knownDevices.end(), [&](const DeviceInfo& d) {
                            return d.deviceId == dev.deviceId;
                        });
                        if (it == m_knownDevices.end()) {
                            m_knownDevices.push_back(dev);
                            std::wcout << L"[+] Nouvel appareil KDE Connect enregistre: " << dev.deviceName 
                                       << L" (" << dev.ipAddress << L")" << std::endl;
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
            if (clientSocket == INVALID_SOCKET) {
                if (!m_running) break;
                continue;
            }

            // Handle client packet reading in detached worker
            std::thread(&ProtocolHandler::HandleClientConnection, this, clientSocket).detach();
        }
    }

    void ProtocolHandler::HandleClientConnection(SOCKET clientSocket) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        getpeername(clientSocket, (sockaddr*)&clientAddr, &addrLen);
        char peerIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, peerIp, sizeof(peerIp));
        std::wstring peerIpW = Utf8ToWide(peerIp);

        std::wcout << L"[TCP Client] Connexion entrante etablie depuis " << peerIpW 
                   << L":" << ntohs(clientAddr.sin_port) << std::endl;

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
                    std::wcout << L"[TCP Paquet] Type: " << Utf8ToWide(pkt.type) 
                               << L" recu de " << peerIpW << std::endl;

                    if (pkt.type == "kdeconnect.pair") {
                        std::wcout << L"[Appairage] Demande d'appairage reçue du telephone ! Validation automatique..." << std::endl;
                        // Confirm pairing handshake back to phone
                        KDEPacket pairReply;
                        pairReply.id = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        pairReply.type = "kdeconnect.pair";
                        pairReply.bodyJson = "{\"pair\":true}";
                        std::string rawReply = pairReply.Serialize();
                        send(clientSocket, rawReply.c_str(), static_cast<int>(rawReply.length()), 0);
                        std::wcout << L"[Appairage] Confirmation envoyee au telephone avec succes !" << std::endl;
                    }

                    DeviceInfo dev;
                    dev.ipAddress = peerIpW;
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
