#pragma once
#include "common.h"
#include "kdeconnect_protocol.h"
#include "toast_notifier.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <set>

namespace KDEConnect {

    class SmsDaemon {
    public:
        using ConversationsUpdatedCallback = std::function<void()>;

        static SmsDaemon& Instance();

        bool Start(const std::wstring& bindIp = L"");
        void Stop();
        bool IsRunning() const { return m_running; }
        bool IsClientMode() const { return m_isClientMode; }

        // Local storage access
        std::vector<Conversation> LoadAllConversations();
        Conversation LoadConversation(int64_t threadId);
        void SaveMessageToDisk(const Message& msg);
        bool DeleteConversation(int64_t threadId);

        // Sync triggers (work seamlessly in both Standalone and IPC Client modes)
        bool RequestConversationsSync();
        bool RequestThreadSync(int64_t threadId);

        // Send SMS to paired phone via protocol or daemon IPC
        bool SendOutgoingSms(const std::wstring& phoneNumber, const std::wstring& text, int64_t preferredThreadId = -1);

        // UI notification hook
        void SetOnConversationsUpdated(ConversationsUpdatedCallback cb);

        // Status info for UI
        std::wstring GetConnectionStatusText() const;

        // Protocol access
        ProtocolHandler& GetProtocol() { return m_protocol; }

        // Directory watcher status
        std::wstring GetWatchDirectory() const { return m_watchDir; }
        void ReloadStorageDirectory();

    private:
        SmsDaemon();
        ~SmsDaemon();

        void DirectoryWatcherThread();
        void NamedPipeIpcThread();
        void ProcessFolderChangeNotification();
        void HandleIncomingKdePacket(const DeviceInfo& device, const KDEPacket& packet);
        void SaveMessagesBatchToDisk(const std::vector<Message>& messages);
        void LoadAllConversationsFromDiskLocked();
        void LoadDeletedThreadsLocked();
        void SaveDeletedThreadsLocked();
        bool SendIpcCommand(const std::string& command, std::string* outResponse = nullptr);
        void NotifyConversationsUpdated();

        std::atomic<bool> m_running{false};
        std::atomic<bool> m_isClientMode{false};
        std::atomic<int64_t> m_ignoreWatcherUntilMs{0};
        std::wstring m_watchDir;
        HANDLE m_dirHandle = INVALID_HANDLE_VALUE;

        std::thread m_watcherThread;
        std::thread m_ipcThread;

        ProtocolHandler m_protocol;
        mutable std::mutex m_storageMutex;
        std::map<int64_t, Conversation> m_conversations;
        std::map<int64_t, int64_t> m_deletedThreads;
        std::set<int64_t> m_requestedThreads;

        std::mutex m_callbackMutex;
        ConversationsUpdatedCallback m_onConversationsUpdated;
    };

} // namespace KDEConnect
