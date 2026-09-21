#pragma once
#include "Backends.h"
#include "Model.h"
#include "ModelCatalog.h"
#include "Tools.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Settings;

// Everything a conversation job needs, copied on the UI thread so the worker
// never touches the Store.
struct JobInput
{
    std::string subserverId, channelId;
    std::string subserverName, channelName;
    ChannelType type = ChannelType::Detente;
    std::string language;
    std::string userName;                 // how the AIs address the user
    std::vector<Message> history;         // including the user's new message
    std::vector<std::string> members;     // AIs that can be addressed in this salon
    std::vector<std::string> defaultSpeakers; // who answers a message that names nobody (no lead)
    std::vector<std::string> forcedSpeakers;  // imposed first speakers (answer to a question, review of code work)
    std::map<std::string, BackendKind> backend;
    std::map<std::string, std::string> provider; // for API backends
    std::map<std::string, std::string> apiKey;   // per AI, resolved on the UI thread
    std::map<std::string, ModelChoice> model;
    std::string lead;                     // team lead id, "" if none
    std::map<std::string, std::string> roleName, roleInstructions;
    std::map<std::string, TeamRole> roles;
    std::string toolGuide;                // tools protocol for this salon type
    Tools::Sources sources;               // what reading tools may open
    std::string context;                  // extra context (memory, sources) for the prompt
    int maxTurns = 8;
    std::wstring workDir;                 // neutral folder for CLI chat mode
};

struct ConductorEvent
{
    enum class Kind
    {
        TurnStart,  // ai starts writing
        Chunk,      // streamed text
        TurnDone,   // final text (to persist)
        TurnPassed, // ai had nothing to add
        TurnFailed, // result carries the error / offline info
        Notice,     // system line for the salon
        Action,     // a tool call that needs the app (text = JSON arguments)
        JobDone,
    };
    Kind kind;
    std::string subserverId, channelId, ai, text;
    TurnResult result;
};

class Conductor
{
public:
    Conductor() = default;
    ~Conductor();

    bool Busy() const { return m_busy; }
    // Starts a job; returns false when one is already running.
    bool Start(JobInput input);
    void Stop();
    std::vector<ConductorEvent> Drain();

    // A reply's own mentions of other AIs ("@claude", "@all"/"@tous").
    static std::vector<std::string> Mentions(const std::string& text, const std::vector<std::string>& candidates);
    // The thread as sent to one AI: short context prepared locally (public for the tests).
    static std::string BuildConversation(const JobInput& in, const std::vector<Message>& history, const std::string& ai);

private:
    void Run(JobInput input);
    void Post(ConductorEvent ev);
    std::string BuildSystemPrompt(const JobInput& in, const std::string& ai) const;

    std::thread m_thread;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_cancel{false};
    std::mutex m_mutex;
    std::vector<ConductorEvent> m_events;
};

// Display name of an AI id ("claude" -> "Claude").
const char* AiDisplayName(const std::string& id);
