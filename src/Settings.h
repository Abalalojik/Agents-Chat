#pragma once
#include "ModelCatalog.h"

#include <filesystem>
#include <map>
#include <string>

// App-wide options, saved in <data root>/settings.json.
class Settings
{
public:
    explicit Settings(std::filesystem::path root);

    // A missing file means catalog defaults, not an error.
    bool Load();

    // Model + thinking an AI uses at a level (catalog default until changed).
    ModelChoice Choice(const std::string& aiId, ModelLevel level) const;
    bool SetChoice(const std::string& aiId, ModelLevel level, const ModelChoice& choice);

    // Presence of an AI on a tier ("chat" or "code"). An offline state whose
    // date has passed ends by itself here (back online, saved).
    Presence GetPresence(const std::string& aiId, const std::string& tier);
    bool SetPresence(const std::string& aiId, const std::string& tier, const Presence& presence);
    // Manual overrides take precedence over provider detection.
    bool PutBackOnline(const std::string& aiId, const std::string& tier);
    bool UseDetectedPresence(const std::string& aiId, const std::string& tier);
    bool HasPresenceOverride(const std::string& aiId, const std::string& tier) const;
    // Whether a backend exists for this AI/tier (installed + signed in, or API key).
    // Not saved: detected at start-up and after sign-in.
    void SetAvailable(const std::string& aiId, const std::string& tier, bool available);
    void SetDetectedPresence(const std::string& aiId, const std::string& tier, const Presence& presence);
    bool IsAvailable(const std::string& aiId, const std::string& tier) const;

    // How the AIs address the user ("" = "l'utilisatrice").
    const std::string& UserName() const { return m_userName; }
    bool SetUserName(const std::string& name);
    // Most AI turns per user message.
    int MaxTurns() const { return m_maxTurns; }
    bool SetMaxTurns(int turns);
    // Exact commands code agents may run without asking (Claude/Gemini), one per line.
    const std::string& AllowedCommands() const { return m_allowedCommands; }
    bool SetAllowedCommands(const std::string& commands);
    const std::string& CommitEmail() const { return m_commitEmail; }
    bool SetCommitEmail(const std::string& email);

    // API keys, stored encrypted (DPAPI). ApiKey returns "" when none.
    std::string ApiKey(const std::string& provider) const;
    bool HasApiKey(const std::string& provider) const { return m_apiKeys.count(provider) > 0; }
    bool SetApiKey(const std::string& provider, const std::string& plainKey); // "" removes it

    const std::string& LastError() const { return m_lastError; }

private:
    bool Save();

    std::filesystem::path m_file;
    // aiId -> level key -> choice (only entries the user changed or loaded)
    std::map<std::string, std::map<std::string, ModelChoice>> m_models;
    // "aiId/tier" -> persistent manual Online/Offline override.
    std::map<std::string, Presence> m_offline;
    std::map<std::string, std::string> m_apiKeys; // provider -> DPAPI-protected base64
    std::map<std::string, bool> m_available;      // "aiId/tier" -> backend present
    std::map<std::string, Presence> m_detected;   // live account/quota state; never persisted
    std::string m_userName;
    int m_maxTurns = 8;
    std::string m_allowedCommands;
    // Empty for a fresh installation: personal Git identities are local user data.
    std::string m_commitEmail;
    std::string m_lastError;
};
