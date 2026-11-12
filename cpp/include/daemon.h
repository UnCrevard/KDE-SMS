#pragma once
#include "common.h"
#include "kdeconnect_protocol.h"
#include "toast_notifier.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <map>

namespace KDEConnect {

    class SmsDaemon {
    public:
        static SmsDaemon& Instance();

        bool Start(const std::wstring& bindIp = L"");
        void Stop();
        bool IsRunning() const { return m_running; }

        // Local storage access
        std::vector<Conversation> LoadAllConversations();
        Conversation LoadConversation(int64_t threadId);
        void SaveMessageToDisk(const Message& msg);

        // Send SMS to paired phone via protocol
        bool SendOutgoingSms(const std::wstring& phoneNumber, const std::wstring& text);

        // Protocol access
        ProtocolHandler& GetProtocol() { return m_protocol; }

        // Directory watcher status
        std::wstring GetWatchDirectory() const { return m_watchDir; }

    private:
        SmsDaemon();
        ~SmsDaemon();

        void DirectoryWatcherThread();
        void NamedPipeIpcThread();
        void ProcessFolderChangeNotification();
        void HandleIncomingKdePacket(const DeviceInfo& device, const KDEPacket& packet);

        std::atomic<bool> m_running{false};
        std::wstring m_watchDir;
        HANDLE m_dirHandle = INVALID_HANDLE_VALUE;

        std::thread m_watcherThread;
        std::thread m_ipcThread;

        ProtocolHandler m_protocol;
        std::mutex m_storageMutex;
        std::map<int64_t, Conversation> m_conversations;
    };

} // namespace KDEConnect
