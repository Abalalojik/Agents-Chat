#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

// Local, model-free synchronization of a personal Outlook.com/Hotmail account.
// OAuth uses Microsoft's consumer-only device-code endpoint. Refresh tokens are
// encrypted with Windows DPAPI; mail/calendar data is cached locally for tools.
class CloudSync
{
public:
    explicit CloudSync(std::filesystem::path dataRoot);
    ~CloudSync();

    bool SaveMicrosoftClientId(const std::string& clientId);
    std::string MicrosoftClientId() const;
    bool HasMicrosoftAccount() const;

    void StartMicrosoftLogin();
    bool SaveGoogleCredentials(const std::string& clientId, const std::string& clientSecret);
    std::string GoogleClientId() const;
    bool HasGoogleCredentials() const;
    bool HasGoogleAccount() const;
    void StartGoogleLogin();
    void StartSynciConnect(const std::string& setupToken);
    bool HasSynciAccount() const;
    void DisconnectSynci();
    void SyncNow();
    bool SyncBlocking(); // headless local scheduler entry point
    void Tick(); // call from the UI thread; schedules a sync every ten minutes
    void Disconnect();

    bool Busy() const { return m_busy; }
    std::string Status() const;
    std::string UserCode() const;
    std::string VerificationUrl() const;

private:
    void LoginWorker();
    void GoogleLoginWorker();
    void SynciConnectWorker();
    void SyncWorker();
    bool RefreshAccessToken(std::string& token, std::string& error);
    bool RefreshGoogleAccessToken(std::string& token, std::string& error);
    bool SaveConfig();
    void SetStatus(const std::string& value);

    std::filesystem::path m_root, m_configFile, m_cacheFile, m_financeCacheFile;
    mutable std::mutex m_mutex;
    std::string m_clientId, m_protectedRefreshToken;
    std::string m_googleClientId, m_protectedGoogleSecret, m_protectedGoogleRefreshToken;
    std::string m_protectedSynciAccessUrl, m_pendingSynciSetupToken;
    std::string m_status = "non configuré", m_userCode, m_verificationUrl;
    std::thread m_worker;
    std::atomic<bool> m_busy{false}, m_stop{false};
    std::atomic<long long> m_nextSyncEpoch{0};
};
