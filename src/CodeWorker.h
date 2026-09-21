#pragma once
#include "Backends.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Runs approved code work (a chat AI's instructions to its code twin) in the
// background, several at once, each stoppable.
struct CodeJob
{
    std::string subserverId, channelId;
    std::string ai; // chat AI whose twin works
    std::string model, thinking;
    std::string instructions;
    std::wstring projectDir;
    std::string allowedCommands;
};

struct CodeEvent
{
    enum class Kind
    {
        Progress,
        Done,
        Failed,
    };
    Kind kind;
    std::string jobId, subserverId, channelId, ai, text;
    TurnResult result;
};

class CodeWorker
{
public:
    ~CodeWorker();
    std::string Start(CodeJob job);
    void StopAll();
    std::vector<CodeEvent> Drain();
    // One line per running job, for the AIs' context and the members column.
    std::vector<std::string> RunningSummaries() const;
    bool IsRunning(const std::string& ai) const;

private:
    struct Running
    {
        CodeJob job;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void Post(CodeEvent ev);

    mutable std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<Running>> m_jobs;
    std::vector<CodeEvent> m_events;
};

// Display name of an AI's code twin ("claude" -> "Claude Code"), nullptr if none.
const char* CodeTwinOf(const std::string& aiId);
// Sender id of the twin in transcripts ("claude" -> "claude-code").
const char* CodeTwinSender(const std::string& aiId);
