#pragma once
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>

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

        ProtocolHandler();
        ~ProtocolHandler();

        bool Start(uint16_t port = 1716);
        void Stop();

        // Broadcast presence on UDP
        void BroadcastIdentity();

        // Send packet to paired device over TCP
        bool SendPacket(const DeviceInfo& device, const KDEPacket& packet);

        // High-level SMS commands
        bool RequestAllConversations(const DeviceInfo& device);
        bool RequestConversationMessages(const DeviceInfo& device, int64_t threadId, int64_t rangeStart = 0);
        bool SendSms(const DeviceInfo& device, const std::wstring& phoneNumber, const std::wstring& text);

        void SetOnPacketReceived(PacketCallback cb) { m_onPacketReceived = cb; }
        void SetOnDeviceDiscovered(DeviceDiscoveredCallback cb) { m_onDeviceDiscovered = cb; }

        const DeviceInfo& GetLocalIdentity() const { return m_localIdentity; }

    private:
        void UdpDiscoveryThread();
        void TcpServerThread();
        void HandleClientConnection(SOCKET clientSocket);

        std::atomic<bool> m_running{false};
        uint16_t m_port = 1716;

        SOCKET m_udpSocket = INVALID_SOCKET;
        SOCKET m_tcpListenSocket = INVALID_SOCKET;

        std::thread m_udpThread;
        std::thread m_tcpThread;

        DeviceInfo m_localIdentity;
        PacketCallback m_onPacketReceived;
        DeviceDiscoveredCallback m_onDeviceDiscovered;

        std::mutex m_devicesMutex;
        std::vector<DeviceInfo> m_knownDevices;
    };

} // namespace KDEConnect
