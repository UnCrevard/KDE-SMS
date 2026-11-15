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
        using ConnectionChangedCallback = std::function<void()>;

        static SmsDaemon& Instance();

        bool Start(const std::wstring& bindIp = L"");
        void Stop();
        bool IsRunning() const { return m_running; }
        bool IsClientMode() const { return m_isClientMode; }

        // Local storage access
        std::vector<Conversation> LoadAllConversations();
        Conversation LoadConversation(int64_t threadId);
        int64_t ResolveMergedThreadId(int64_t threadId) const;
        std::vector<ContactSummary> GetSyncedContacts() const;
        void SaveMessageToDisk(const Message& msg);
        bool DeleteConversation(int64_t threadId);

        // Sync triggers (work seamlessly in both Standalone and IPC Client modes)
        bool RequestConversationsSync();
        bool RequestThreadSync(int64_t threadId);

        // Send SMS or MMS (with optional image attachment) to paired phone via protocol or daemon IPC
        bool SendOutgoingSms(
            const std::wstring& phoneNumber,
            const std::wstring& text,
            int64_t preferredThreadId = -1,
            const std::wstring& attachmentPath = L""
        );

        // UI notification hooks
        void SetOnConversationsUpdated(ConversationsUpdatedCallback cb);
        void SetOnConnectionChanged(ConnectionChangedCallback cb);

        // Status info for UI
        bool IsPhoneConnected() const;
        std::wstring GetConnectedDeviceName() const;
        void RefreshClientStatus();
        std::wstring GetConnectionStatusText() const;
        std::wstring GetBoundEndpointSummary() const;
        std::wstring GetLastBindError() const;

        // Protocol access
        ProtocolHandler& GetProtocol() { return m_protocol; }

        // Directory watcher & live settings reload
        std::wstring GetWatchDirectory() const { return m_watchDir; }
        void ReloadStorageDirectory();
        bool ApplySettingsReload(const std::wstring& newBindIp, const std::wstring& newDeviceName);

    private:
        struct CachedContact {
            std::string uid;
            int64_t timestamp = 0;
            std::wstring name;
            std::vector<std::wstring> phones;
            std::wstring avatarPath;
        };

        struct PendingAttachmentRequest {
            int64_t threadId = 0;
            int64_t messageId = 0;
            size_t attachmentIndex = 0;
            int64_t partId = -1;
            std::string uniqueId;
            std::wstring mimeType;
            int64_t requestedAtMs = 0;
        };

        SmsDaemon();
        ~SmsDaemon();

        void DirectoryWatcherThread();
        void NamedPipeIpcThread();
        void ProcessFolderChangeNotification();
        void HandleIncomingKdePacket(const DeviceInfo& device, const KDEPacket& packet);
        void SaveMessagesBatchToDisk(const std::vector<Message>& messages, std::vector<Message>* outNewlyInserted = nullptr);
        void MaybeNotifyIncomingMessage(const Message& msg, const std::wstring& resolvedName);
        std::wstring BuildConversationFilePathLocked(const Conversation& conv) const;
        void RemoveConversationFileLocked(int64_t threadId);
        void WriteConversationJsonLocked(const Conversation& conv);
        void LoadAllConversationsFromDiskLocked();
        void LoadDeletedThreadsLocked();
        void SaveDeletedThreadsLocked();
        void LoadContactsCacheLocked();
        void SaveContactsCacheLocked();
        void IndexContactLocked(const CachedContact& contact);
        std::wstring ResolveSingleContactLocked(const std::wstring& rawAddress) const;
        std::wstring ResolveContactNameForAddressLocked(const std::wstring& rawAddress) const;
        std::wstring ResolveSingleAvatarLocked(const std::wstring& rawAddress) const;
        std::wstring ResolveAvatarForAddressLocked(const std::wstring& rawAddress) const;
        int64_t FindConversationByPhoneLocked(const std::wstring& rawAddress) const;
        bool ApplyContactsToConversationsLocked();
        bool IsDaemonIpcAvailable() const;
        void RefreshClientStatusFromDaemon();
        bool SendIpcCommand(const std::string& command, std::string* outResponse = nullptr);
        void NotifyConversationsUpdated();
        void NotifyConnectionChanged();

        std::atomic<bool> m_running{false};
        std::atomic<bool> m_isClientMode{false};
        std::atomic<bool> m_clientPhoneConnected{false};
        std::atomic<int64_t> m_ignoreWatcherUntilMs{0};
        int64_t m_startupEpochMs = 0;
        std::wstring m_watchDir;
        HANDLE m_dirHandle = INVALID_HANDLE_VALUE;

        std::thread m_watcherThread;
        std::thread m_ipcThread;

        ProtocolHandler m_protocol;
        mutable std::mutex m_netStatusMutex;
        std::wstring m_lastBindError;
        std::wstring m_clientBoundEndpoint;
        std::wstring m_clientConnectedDeviceName;
        mutable std::mutex m_storageMutex;
        std::map<int64_t, Conversation> m_conversations;
        std::map<int64_t, std::wstring> m_threadFilePaths;
        std::map<int64_t, int64_t> m_deletedThreads;
        std::map<int64_t, int64_t> m_threadRedirects;
        std::set<int64_t> m_requestedThreads;
        std::map<std::string, PendingAttachmentRequest> m_pendingAttachments;
        std::set<std::string> m_completedAttachmentKeys;
        std::map<std::string, int64_t> m_contactUidTimestamps;
        std::map<std::wstring, std::wstring> m_phoneToContactName;
        std::map<std::wstring, std::wstring> m_phoneToAvatarPath;
        std::map<std::string, CachedContact> m_contactsCache;

        std::mutex m_notifyMutex;
        std::map<std::wstring, int64_t> m_recentNotifiedSignatures;
        std::set<int64_t> m_notifiedMsgIds;

        std::mutex m_callbackMutex;
        ConversationsUpdatedCallback m_onConversationsUpdated;
        ConnectionChangedCallback m_onConnectionChanged;
    };

} // namespace KDEConnect
