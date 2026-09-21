#pragma once
#include "CodeWorker.h"
#include "CloudSync.h"
#include "Conductor.h"
#include "Settings.h"
#include "Store.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The chat-app user interface. One call to Frame() draws the whole window.
class App
{
public:
    App(Store& store, Settings& settings, void* hwnd);
    ~App();
    void Frame();

private:
    // Layout
    void DrawSubserverColumn(float height);
    void DrawChannelColumn(float height);
    void DrawCenter(float width, float height);
    void DrawMembersColumn(float height);
    void DrawChannelView(Subserver& subserver, Channel& channel, float width, float height);
    void DrawMessage(Subserver& subserver, Channel& channel, const Message& msg);
    void DrawInboxView();
    void DrawTodoView();
    void DrawInboxItem(const InboxItem& item);
    void DrawWelcome();
    void DrawErrorBar();
    void DrawTaskBoard(Channel& channel);
    void DrawMemberRow(Channel* channel, const std::string& ai, bool troupe);
    void OpenTodo(const std::string& owner);
    void OpenPrivateMessage(const std::string& ai);

    // Popups and windows
    void DrawCreateSubserverPopup();
    void DrawEditSourcesPopup();
    void DrawCreateChannelPopup();
    void DrawOptionsWindow();
    void DrawChatOptionsWindow();
    void DrawGeneralTab();
    void DrawConnectionsTab();
    void DrawCloudTab();
    void DrawModelsTab();
    void DrawMemoryTab();
    void DrawPresenceMenu(const std::string& aiId, const char* tier, const char* label, const Presence& presence);
    void DrawRoleMenu(Channel& channel, const std::string& ai);

    bool FolderField(const char* label, std::string& path, const wchar_t* pickerTitle);
    static bool LanguagePicker(const char* id, std::string& language, float width);

    // Conversation plumbing
    void SendUserMessage(Subserver& subserver, Channel& channel, const std::string& text);
    bool StartJob(Subserver& subserver, Channel& channel, const std::vector<std::string>& speakers);
    void ProcessEvents();
    void HandleAction(const ConductorEvent& ev);
    void ApplyCorrection(const InboxItem& item);
    void LaunchCodeWork(const InboxItem& item);
    void RequestSkill(const InboxItem& item);
    void PostSystem(const std::string& subserverId, const std::string& channelId, const std::string& text,
                    const std::string& sender = "system", const std::string& kind = "", const std::string& ref = "");
    void RefreshAvailability();
    void ApplyAvailability();
    std::vector<std::string> SalonMembers(const Channel& channel, bool forDefaultSpeakers) const;

    Store& m_store;
    Settings& m_settings;
    void* m_hwnd;
    Conductor m_conductor;
    CodeWorker m_codeWorker;
    CloudSync m_cloud;

    std::string m_selectedSubserver;
    std::string m_selectedChannel;
    bool m_showInbox = false;
    bool m_showTodo = false;
    bool m_showInboxHistory = false;
    std::string m_composer;
    std::map<std::string, size_t> m_shownCount;   // salon -> messages drawn
    std::string m_queuedText, m_queuedChannel;     // sent when the running job ends
    bool m_startQueued = false;
    bool m_scrollToBottom = false;

    // Live view of the running job
    std::string m_jobChannel; // channel id the conductor works in
    struct Pending
    {
        std::string ai;
        std::string text;
    };
    std::vector<Pending> m_pending;
    std::vector<std::string> m_notices;
    std::map<std::string, std::string> m_codeProgress; // job id -> last progress lines
    std::map<std::string, std::string> m_codeJobChannel;
    std::map<std::string, std::string> m_codeJobAi;
    // Availability detection (background)
    struct Availability
    {
        bool claude = false, codex = false, gemini = false;
        std::string claudeDetail, codexDetail, geminiDetail;
        std::string claudePath, codexPath, geminiPath;
    };
    std::thread m_availThread;
    std::atomic<bool> m_availReady{false};
    std::atomic<bool> m_availRunning{false};
    std::mutex m_availMutex;
    Availability m_avail;

    // Popups
    bool m_showOptions = false;
    bool m_focusOptions = false;
    bool m_showChatOptions = false;
    bool m_focusChatOptions = false;
    bool m_showMembers = true;
    std::string m_chatOptionsAi = "salon";
    // Rename / delete (sous-serveurs and salons)
    std::string m_renameSubserver, m_renameChannel, m_renameText;
    std::string m_deleteSubserver, m_deleteChannel;
    bool m_openRename = false, m_openDelete = false;
    void DrawRenameDeletePopups();
    bool m_openCreateSubserver = false;
    std::string m_newSubName, m_newSubVault, m_newSubLore, m_newSubCode;
    bool m_openEditSources = false;
    std::string m_editVault, m_editLore, m_editCode;
    std::string m_editMain;
    std::vector<Subserver::FolderAccess> m_editAdditionalFolders;
    std::vector<Subserver::ExclusionRule> m_editExclusions;
    bool m_openCreateChannel = false;
    std::string m_newChannelName;
    int m_newChannelType = 0;
    std::string m_newChannelLanguage = "Français";

    // Options state
    std::map<std::string, std::string> m_keyInput;
    std::map<std::string, std::string> m_answerInput; // inbox question id -> answer being typed
    std::string m_userNameInput, m_allowedInput;
    std::string m_microsoftClientIdInput;
    std::string m_googleClientIdInput, m_googleClientSecretInput;
    std::string m_synciSetupTokenInput;
    std::string m_localSchedulerStatus;
    bool m_cloudLoaded = false;
    bool m_generalLoaded = false;
    std::string m_memoryEditId, m_memoryEditText;
    std::string m_newTaskTitle;
    std::string m_todoOwner = "user";
    std::string m_todoNewTitle;
    std::string m_todoChannel;
    bool m_todoShowDone = false;
};
