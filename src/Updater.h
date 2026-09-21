#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
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
    // Builds AgentChats + AgentChatsTests from a source tree (default: the local clone) and
    // runs the tests; only a build whose tests pass is staged for installation.
    void BuildLocal(const std::filesystem::path& source = {});
    struct BuildReport
    {
        bool ok = false;          // built, tests passed, staged
        bool built = false;
        bool testsPassed = false;
        std::string summary;
        std::string log;          // tail of the build and test output
    };
    // The result of the last BuildLocal, once (then empty).
    std::optional<BuildReport> TakeReport();
    bool LocalBuildReady() const;
    void PrepareContribution();
    bool Busy() const { return m_busy; }
    std::string Status() const;
    std::string AvailableVersion() const;
    bool Ready() const;
    bool Apply(void* hwnd);
    bool SourceReady() const;
    const std::filesystem::path& SourcePath() const { return m_source; }
    static bool ApplyPendingUpdate(const std::filesystem::path& destination, unsigned long parentPid);
    // The version replaced by the last update, kept next to the executable.
    bool HasPrevious() const;
    bool Rollback(void* hwnd);
    static std::filesystem::path PreviousPath();

private:
    void RunCheck(bool download);
    std::filesystem::path m_root, m_pending, m_source;
    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_busy{false};
    std::string m_status, m_version, m_downloadUrl, m_hashUrl;
    bool m_ready = false;
    std::optional<BuildReport> m_report;
};

inline constexpr const char* kAgentChatsVersion = AGENTCHATS_VERSION;
