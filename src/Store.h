#pragma once
#include "GitHub.h"
#include "Model.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Owns the app's data on disk:
//   <root>/workspace.json                                  sous-serveurs, salons, roles
//   <root>/subservers/<sid>/channels/<cid>/transcript.jsonl append-only, one event per line
//   <root>/inbox.json, memory.json, tasks.json
// The built-in "Amélioration d’Agents Chat" sous-serveur.
inline constexpr const char* kSelfSubserverId = "agentchats-self";

class Store
{
public:
    explicit Store(std::filesystem::path root);

    // Reads everything. A missing file is a fresh start, not an error.
    bool Load();

    const std::vector<Subserver>& Subservers() const { return m_subservers; }
    Subserver* FindSubserver(const std::string& id);
    Channel* FindChannel(Subserver& subserver, const std::string& channelId);

    Subserver* CreateSubserver(const std::string& name, const std::string& vaultPath,
                               const std::string& lorePath, const std::string& codePath);
    // Creates the built-in workspace where the agents can discuss and improve
    // AgentChats itself. Idempotent: existing user configuration is preserved.
    bool EnsureSelfImprovementSubserver(const std::string& codePath);
    bool UpdateSources(Subserver& subserver, const std::string& vaultPath, const std::string& lorePath,
                       const std::string& codePath);
    bool UpdateFolderAccess(Subserver& subserver, const std::string& mainPath,
                            const std::vector<Subserver::FolderAccess>& additionalFolders,
                            const std::vector<Subserver::ExclusionRule>& exclusions,
                            const std::string& githubUrl = {});
    Channel* CreateChannel(Subserver& subserver, const std::string& name, ChannelType type,
                           const std::string& language);
    bool RenameSubserver(Subserver& subserver, const std::string& name);
    bool RenameChannel(Channel& channel, const std::string& name);
    // Removes it from the app; its conversations go to the Windows Recycle Bin.
    bool DeleteSubserver(const std::string& id);
    bool DeleteChannel(Subserver& subserver, const std::string& channelId);
    bool SetChannelLanguage(Channel& channel, const std::string& language);
    bool SetChannelLevel(Channel& channel, ModelLevel level);
    // Overrides one AI's level in this salon; nullptr returns it to the salon's level.
    bool SetAiLevel(Channel& channel, const std::string& ai, const ModelLevel* level);
    // Gives or clears (nullptr) an AI's role. A new lead replaces the previous one.
    bool SetRole(Channel& channel, const std::string& ai, const TeamRole* role);

    // Messages of a salon, loaded from its transcript on first access.
    const std::vector<Message>& Messages(const Subserver& subserver, const Channel& channel);

    // Writes the message to the transcript first; only a successfully written
    // message enters the in-memory history. Returns false on I/O failure.
    bool AppendMessage(const Subserver& subserver, const Channel& channel, const std::string& sender,
                       const std::string& content, const std::string& kind = "", const std::string& ref = "");

    // Boîte aux lettres
    const std::vector<InboxItem>& Inbox() const { return m_inbox; }
    int PendingInboxCount() const;
    int PendingInboxCount(const std::string& subserverId) const;
    const InboxItem* AddInboxItem(InboxItem item);
    bool DecideInboxItem(const std::string& id, const std::string& status, const std::string& answer);
    const InboxItem* FindInboxItem(const std::string& id) const;

    // Mémoire commune
    const std::vector<MemoryNote>& Memory() const { return m_memory; }
    // Adds a fact to the shared memory. Equivalent visible facts are reused
    // across AIs; a duplicate learned at a wider scope promotes the existing
    // note instead of creating a second copy.
    const MemoryNote* AddMemory(const std::string& level, const std::string& scopeId, const std::string& ai,
                                const std::string& text, bool* created = nullptr, bool* promoted = nullptr);
    bool UpdateMemory(const std::string& id, const std::string& text);
    bool Forget(const std::string& id);
    const MemoryNote* FindMemory(const std::string& id) const;
    // Notes an AI sees in a salon: "toi" + this sous-serveur + this salon.
    std::vector<const MemoryNote*> MemoryFor(const std::string& subserverId, const std::string& channelId) const;

    // Tableau des tâches
    const std::vector<TaskItem>& AllTasks() const { return m_tasks; }
    std::vector<TaskItem> Tasks(const std::string& channelId) const;
    const TaskItem* AddTask(const std::string& channelId, const std::string& title, const std::string& assignee,
                            const std::string& createdBy);
    // Finds by id, or else by title (case-insensitive) in the salon.
    bool SetTaskStatus(const std::string& channelId, const std::string& idOrTitle, const std::string& status);
    bool SetTaskAssignee(const std::string& id, const std::string& assignee);
    bool DeleteTask(const std::string& id);
    const TaskItem* FindTask(const std::string& id) const;
    bool SetTaskIssue(const std::string& id, int number, const std::string& url, const std::string& state);
    // Brings a salon's board in line with the repository's issues: open issues
    // become tasks, issues closed on GitHub mark their task done. Never touches GitHub.
    struct IssueImport
    {
        int created = 0, updated = 0, closed = 0;
    };
    IssueImport ImportIssues(const std::string& channelId, const std::vector<GitHub::Issue>& issues);

    const std::filesystem::path& Root() const { return m_root; }
    const std::string& LastError() const { return m_lastError; }
    void SetError(const std::string& e) { m_lastError = e; }
    void ClearError() { m_lastError.clear(); }

    // Folder of a salon's own files (transcript, console history).
    std::filesystem::path ChannelDir(const Subserver& subserver, const Channel& channel) const
    {
        return TranscriptPath(subserver, channel).parent_path();
    }

private:
    bool SaveWorkspace();
    bool SaveInbox();
    bool SaveMemory();
    bool ConsolidateMemoryDuplicates();
    bool MemoryScopesOverlap(const std::string& leftLevel, const std::string& leftScope,
                             const std::string& rightLevel, const std::string& rightScope) const;
    bool SaveTasks();
    bool WriteFileAtomic(const std::filesystem::path& file, const std::string& text);
    std::filesystem::path TranscriptPath(const Subserver& subserver, const Channel& channel) const;
    static std::string CacheKey(const Subserver& subserver, const Channel& channel);

    std::filesystem::path m_root;
    std::vector<Subserver> m_subservers;
    std::map<std::string, std::vector<Message>> m_messages;
    std::map<std::string, long long> m_nextSeq;
    std::vector<InboxItem> m_inbox;
    std::vector<MemoryNote> m_memory;
    std::vector<TaskItem> m_tasks;
    std::string m_lastError;
};
