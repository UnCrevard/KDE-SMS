#pragma once
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <wincrypt.h>
#include <security.h>
#include <schannel.h>
#include <thread>
#include <atomic>
#include <map>
#include <set>

namespace KDEConnect {

    struct KDEPacket {
        int64_t id = 0;
        std::string type;
        std::string bodyJson;

        std::string Serialize() const;
        static bool Deserialize(const std::string& raw, KDEPacket& outPacket);
    };

    class ProtocolHandler {
    public:
        using PacketCallback = std::function<void(const DeviceInfo& device, const KDEPacket& packet)>;
        using DeviceDiscoveredCallback = std::function<void(const DeviceInfo& device)>;
        using DeviceReadyCallback = std::function<void(const DeviceInfo& device)>;

        ProtocolHandler();
        ~ProtocolHandler();

        bool Start(const std::wstring& bindIp = L"", uint16_t port = 1716);
        void Stop();

        // Broadcast presence on UDP
        void BroadcastIdentity();

        // Enumerate local IPv4 addresses
        static std::vector<std::wstring> GetLocalIpAddresses();

        // Send packet to paired device over active TLS session
        bool SendPacket(const DeviceInfo& device, const KDEPacket& packet);
        bool SendPairRequest(const DeviceInfo& device);

        // High-level SMS commands
        bool RequestAllConversations(const DeviceInfo& device = DeviceInfo{});
        bool RequestConversationMessages(const DeviceInfo& device, int64_t threadId, int64_t rangeStart = -1);
        bool SendSms(const DeviceInfo& device, const std::wstring& phoneNumber, const std::wstring& text);

        void SetOnPacketReceived(PacketCallback cb) { m_onPacketReceived = cb; }
        void SetOnDeviceDiscovered(DeviceDiscoveredCallback cb) { m_onDeviceDiscovered = cb; }
        void SetOnDeviceReady(DeviceReadyCallback cb) { m_onDeviceReady = cb; }

        std::vector<DeviceInfo> GetConnectedDevices() const;
        bool IsDevicePaired(const std::wstring& deviceId) const;

        const DeviceInfo& GetLocalIdentity() const { return m_localIdentity; }
        const std::wstring& GetBindIp() const { return m_bindIp; }

    private:
        struct TlsSession {
            SOCKET sock = INVALID_SOCKET;
            CredHandle hCred{};
            CtxtHandle hCtx{};
            bool hasCred = false;
            bool hasCtx = false;
            SecPkgContext_StreamSizes streamSizes{};
            std::vector<char> pendingCipher;
            std::mutex sendMutex;
        };

        void LoadPairedDevices();
        void MarkDevicePaired(const std::wstring& deviceId, bool paired);
        bool EnsureLocalCertificate();
        bool PerformTlsHandshakeClient(SOCKET sock, TlsSession& session);
        bool PerformTlsHandshakeServer(SOCKET sock, TlsSession& session);
        bool TlsSendAll(TlsSession& session, const std::string& plaintext);
        bool TlsRecvChunk(TlsSession& session, std::string& outPlaintext);
        void CleanupTlsSession(TlsSession& session);
        void RunEncryptedPacketLoop(std::shared_ptr<TlsSession> session, DeviceInfo dev);

        void UdpDiscoveryThread();
        void TcpServerThread();
        void HandleClientConnection(SOCKET clientSocket);
        void PeriodicBroadcastThread();
        void ConnectAndSendIdentityTcp(const DeviceInfo& device);
        std::string BuildIdentityPacket() const;

        std::atomic<bool> m_running{false};
        uint16_t m_port = 1716;
        std::wstring m_bindIp;

        SOCKET m_udpSocket = INVALID_SOCKET;
        SOCKET m_tcpListenSocket = INVALID_SOCKET;
        PCCERT_CONTEXT m_localCert = nullptr;

        std::thread m_udpThread;
        std::thread m_tcpThread;
        std::thread m_broadcastThread;

        DeviceInfo m_localIdentity;
        PacketCallback m_onPacketReceived;
        DeviceDiscoveredCallback m_onDeviceDiscovered;
        DeviceReadyCallback m_onDeviceReady;

        mutable std::mutex m_devicesMutex;
        std::vector<DeviceInfo> m_knownDevices;
        std::set<std::wstring> m_pairedDeviceIds;

        mutable std::mutex m_connMutex;
        std::map<std::wstring, std::shared_ptr<TlsSession>> m_activeSessions;
        std::map<std::wstring, std::chrono::steady_clock::time_point> m_lastTcpAttempt;
    };

} // namespace KDEConnect
