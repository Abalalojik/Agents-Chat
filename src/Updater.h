#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

class Updater
{
public:
    explicit Updater(std::filesystem::path dataRoot);
    ~Updater();
    void Check();
    void Download();
    void PrepareSource();
    void BuildLocal();
    void PrepareContribution();
    bool Busy() const { return m_busy; }
    std::string Status() const;
    std::string AvailableVersion() const;
    bool Ready() const;
    bool Apply(void* hwnd);
    bool SourceReady() const;
    const std::filesystem::path& SourcePath() const { return m_source; }
    static bool ApplyPendingUpdate(const std::filesystem::path& destination, unsigned long parentPid);

private:
    void RunCheck(bool download);
    std::filesystem::path m_root, m_pending, m_source;
    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_busy{false};
    std::string m_status, m_version, m_downloadUrl, m_hashUrl;
    bool m_ready = false;
};

inline constexpr const char* kAgentChatsVersion = AGENTCHATS_VERSION;
