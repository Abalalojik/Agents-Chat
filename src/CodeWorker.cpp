#include "CodeWorker.h"
#include "Platform.h"

const char* CodeTwinOf(const std::string& aiId)
{
    if (aiId == "chatgpt") return "Codex";
    if (aiId == "claude") return "Claude Code";
    if (aiId == "gemini") return "Gemini CLI";
    return nullptr;
}

const char* CodeTwinSender(const std::string& aiId)
{
    if (aiId == "chatgpt") return "codex";
    if (aiId == "claude") return "claude-code";
    if (aiId == "gemini") return "gemini-cli";
    return "system";
}

CodeWorker::~CodeWorker()
{
    StopAll();
    // Join outside the lock: a finishing job posts its last event under the same mutex.
    std::map<std::string, std::unique_ptr<Running>> jobs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jobs.swap(m_jobs);
    }
    for (auto& [id, job] : jobs)
        if (job->thread.joinable())
            job->thread.join();
}

std::string CodeWorker::Start(CodeJob job)
{
    const std::string id = Platform::NewId();
    auto running = std::make_unique<Running>();
    running->job = job;
    running->cancel = std::make_shared<std::atomic<bool>>(false);
    Running* raw = running.get();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs[id] = std::move(running);
    }
    raw->thread = std::thread([this, id, raw, job = std::move(job)] {
        TurnResult r = RunCodeWork(job.ai, job.model, job.thinking, job.instructions, job.projectDir, job.allowedCommands,
                                   [&](const std::string& line) {
                                       Post({CodeEvent::Kind::Progress, id, job.subserverId, job.channelId, job.ai, line, {}});
                                   },
                                   *raw->cancel);
        Post({r.ok ? CodeEvent::Kind::Done : CodeEvent::Kind::Failed, id, job.subserverId, job.channelId, job.ai,
              r.ok ? r.text : r.error, r});
        raw->finished = true;
    });
    return id;
}

void CodeWorker::StopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, job] : m_jobs)
        job->cancel->store(true);
}

void CodeWorker::Post(CodeEvent ev)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(std::move(ev));
}

std::vector<CodeEvent> CodeWorker::Drain()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Reap finished jobs.
    for (auto it = m_jobs.begin(); it != m_jobs.end();)
    {
        if (it->second->finished)
        {
            if (it->second->thread.joinable())
                it->second->thread.join();
            it = m_jobs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    std::vector<CodeEvent> out;
    out.swap(m_events);
    return out;
}

std::vector<std::string> CodeWorker::RunningSummaries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& [id, job] : m_jobs)
        if (!job->finished)
        {
            std::string instr = job->job.instructions.substr(0, 160);
            out.push_back(std::string(CodeTwinOf(job->job.ai) ? CodeTwinOf(job->job.ai) : "?") + " travaille : " + instr);
        }
    return out;
}

bool CodeWorker::IsRunning(const std::string& ai) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [id, job] : m_jobs)
        if (!job->finished && job->job.ai == ai)
            return true;
    return false;
}
