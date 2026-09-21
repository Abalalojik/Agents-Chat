#include "Settings.h"
#include "Platform.h"
#include "Secrets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

Settings::Settings(fs::path root) : m_file(std::move(root) / "settings.json") {}

bool Settings::Load()
{
    if (!fs::exists(m_file))
        return true;
    try
    {
        std::ifstream in(m_file, std::ios::binary);
        const json doc = json::parse(in);
        const json models = doc.value("models", json::object());
        for (const auto& [aiId, levels] : models.items())
        {
            if (!levels.is_object())
                continue;
            for (const auto& [levelKey, choice] : levels.items())
            {
                ModelLevel level = ModelLevel::Normal;
                if (!ModelLevelFromKey(levelKey, level) || !choice.is_object())
                    continue;
                m_models[aiId][ModelLevelKey(level)] = {choice.value("model", ""), choice.value("thinking", "")};
            }
        }
        m_userName = doc.value("userName", "");
        m_maxTurns = std::clamp(doc.value("maxTurns", 8), 1, 40);
        m_allowedCommands = doc.value("allowedCommands", "");

        const json keys = doc.value("apiKeys", json::object());
        for (const auto& [provider, protectedKey] : keys.items())
            if (protectedKey.is_string() && !protectedKey.get<std::string>().empty())
                m_apiKeys[provider] = protectedKey.get<std::string>();

        const json offline = doc.value("offline", json::object());
        for (const auto& [key, p] : offline.items())
        {
            if (!p.is_object())
                continue;
            m_offline[key] = {PresenceState::Offline, p.value("reason", ""), p.value("until", "")};
        }
        const json overrides = doc.value("presenceOverrides", json::object());
        for (const auto& [key, p] : overrides.items())
        {
            if (!p.is_object())
                continue;
            const std::string state = p.value("state", "");
            if (state == "online" || state == "offline")
                m_offline[key] = {state == "online" ? PresenceState::Online : PresenceState::Offline,
                                  p.value("reason", ""), p.value("until", "")};
        }
        return true;
    }
    catch (const std::exception& e)
    {
        m_lastError = std::string("Lecture de settings.json impossible : ") + e.what();
        return false;
    }
}

ModelChoice Settings::Choice(const std::string& aiId, ModelLevel level) const
{
    const auto ai = m_models.find(aiId);
    if (ai != m_models.end())
    {
        const auto it = ai->second.find(ModelLevelKey(level));
        if (it != ai->second.end() && !it->second.model.empty())
            return it->second;
    }
    if (const AiCatalogEntry* entry = FindCatalogEntry(aiId))
        return entry->defaults[static_cast<size_t>(level)];
    return {};
}

bool Settings::SetChoice(const std::string& aiId, ModelLevel level, const ModelChoice& choice)
{
    const auto previous = m_models;
    m_models[aiId][ModelLevelKey(level)] = choice;
    if (!Save())
    {
        m_models = previous;
        return false;
    }
    return true;
}

Presence Settings::GetPresence(const std::string& aiId, const std::string& tier)
{
    const std::string key = aiId + "/" + tier;
    const auto it = m_offline.find(key);
    if (it != m_offline.end())
    {
        // ISO 8601 UTC strings of the same shape compare in time order.
        if (it->second.state == PresenceState::Online || it->second.untilIso.empty() ||
            Platform::NowIsoUtc() < it->second.untilIso)
            return it->second;
        m_offline.erase(it); // the announced date has passed
        Save();
    }
    const auto detected = m_detected.find(key);
    if (detected != m_detected.end())
        return detected->second;
    return {IsAvailable(aiId, tier) ? PresenceState::Unknown : PresenceState::NotConnected, "", ""};
}

bool Settings::SetPresence(const std::string& aiId, const std::string& tier, const Presence& presence)
{
    const auto previous = m_offline;
    const std::string key = aiId + "/" + tier;
    if (presence.state == PresenceState::Offline || presence.state == PresenceState::Online)
        m_offline[key] = presence;
    else
        m_offline.erase(key);
    if (!Save())
    {
        m_offline = previous;
        return false;
    }
    return true;
}

bool Settings::PutBackOnline(const std::string& aiId, const std::string& tier)
{
    return SetPresence(aiId, tier, {PresenceState::Online, "forcé manuellement", ""});
}

bool Settings::UseDetectedPresence(const std::string& aiId, const std::string& tier)
{
    const auto previous = m_offline;
    m_offline.erase(aiId + "/" + tier);
    if (!Save())
    {
        m_offline = previous;
        return false;
    }
    return true;
}

bool Settings::HasPresenceOverride(const std::string& aiId, const std::string& tier) const
{
    return m_offline.count(aiId + "/" + tier) > 0;
}

std::string Settings::ApiKey(const std::string& provider) const
{
    const auto it = m_apiKeys.find(provider);
    return it == m_apiKeys.end() ? std::string() : Secrets::Unprotect(it->second);
}

bool Settings::SetApiKey(const std::string& provider, const std::string& plainKey)
{
    const auto previous = m_apiKeys;
    if (plainKey.empty())
    {
        m_apiKeys.erase(provider);
    }
    else
    {
        const std::string protectedKey = Secrets::Protect(plainKey);
        if (protectedKey.empty())
        {
            m_lastError = "Chiffrement de la clé impossible.";
            return false;
        }
        m_apiKeys[provider] = protectedKey;
    }
    if (!Save())
    {
        m_apiKeys = previous;
        return false;
    }
    return true;
}

void Settings::SetAvailable(const std::string& aiId, const std::string& tier, bool available)
{
    m_available[aiId + "/" + tier] = available;
}

void Settings::SetDetectedPresence(const std::string& aiId, const std::string& tier, const Presence& presence)
{
    m_detected[aiId + "/" + tier] = presence;
    m_available[aiId + "/" + tier] = PresenceCanWork(presence.state);
}

bool Settings::IsAvailable(const std::string& aiId, const std::string& tier) const
{
    const auto it = m_available.find(aiId + "/" + tier);
    return it != m_available.end() && it->second;
}

bool Settings::SetUserName(const std::string& name)
{
    const std::string previous = m_userName;
    m_userName = name;
    if (!Save())
    {
        m_userName = previous;
        return false;
    }
    return true;
}

bool Settings::SetMaxTurns(int turns)
{
    const int previous = m_maxTurns;
    m_maxTurns = std::clamp(turns, 1, 40);
    if (!Save())
    {
        m_maxTurns = previous;
        return false;
    }
    return true;
}

bool Settings::SetAllowedCommands(const std::string& commands)
{
    const std::string previous = m_allowedCommands;
    m_allowedCommands = commands;
    if (!Save())
    {
        m_allowedCommands = previous;
        return false;
    }
    return true;
}

bool Settings::Save()
{
    json models = json::object();
    for (const auto& [aiId, levels] : m_models)
        for (const auto& [levelKey, choice] : levels)
            models[aiId][levelKey] = {{"model", choice.model}, {"thinking", choice.thinking}};
    json offline = json::object(); // kept for backward compatibility with older builds
    json presenceOverrides = json::object();
    for (const auto& [key, p] : m_offline)
    {
        if (p.state == PresenceState::Offline)
            offline[key] = {{"reason", p.reason}, {"until", p.untilIso}};
        presenceOverrides[key] = {{"state", p.state == PresenceState::Online ? "online" : "offline"},
                                  {"reason", p.reason}, {"until", p.untilIso}};
    }
    json keys = json::object();
    for (const auto& [provider, protectedKey] : m_apiKeys)
        keys[provider] = protectedKey;
    const json doc = {{"version", 1}, {"models", models}, {"offline", offline},
                      {"presenceOverrides", presenceOverrides}, {"apiKeys", keys},
                      {"userName", m_userName}, {"maxTurns", m_maxTurns}, {"allowedCommands", m_allowedCommands}};

    std::string error;
    if (!Platform::WriteFileAtomic(m_file, doc.dump(2), error))
    {
        m_lastError = "Écriture de settings.json impossible : " + error;
        return false;
    }
    return true;
}
