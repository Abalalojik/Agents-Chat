#pragma once
#include <filesystem>
#include <string>

// Small platform helpers (Windows and POSIX). All strings crossing this API are UTF-8.
namespace Platform
{
    std::wstring Widen(const std::string& utf8);
    std::string Narrow(const std::wstring& wide);

    // Writes a complete file through a sibling temporary file, then atomically
    // replaces the destination (including when it already exists).
    bool WriteFileAtomic(const std::filesystem::path& file, const std::string& text, std::string& error);

    // %LOCALAPPDATA%\AgentChats on Windows, $XDG_DATA_HOME/AgentChats (~/.local/share) on Linux,
    // or $AGENTCHATS_DATA when set; created if missing.
    std::filesystem::path DataRoot();

    // Locates the AgentChats source root from the running executable/build tree.
    // Empty when the executable was distributed without its source checkout.
    std::filesystem::path ProjectRoot();

    // Random 16-hex-digit id, safe to use as a folder name.
    std::string NewId();

    // Current time as ISO 8601 UTC, e.g. "2026-09-21T08:15:02Z".
    std::string NowIsoUtc();

    // "HH:MM" in local time for an ISO 8601 UTC timestamp ("" if unparsable).
    std::string LocalTimeOfDay(const std::string& isoUtc);

    // "HH:MM" if the timestamp falls today (local time), else "DD/MM HH:MM".
    std::string LocalWhen(const std::string& isoUtc);

    // Opens a URL or a file with the desktop's default application (browser for http links).
    void OpenUrl(const std::string& url);

    // Sends a file or folder to the desktop trash (recoverable). True when it is gone.
    bool MoveToTrash(const std::filesystem::path& path);

    // Native folder picker. Returns "" when cancelled.
    std::string PickFolder(void* ownerHwnd, const wchar_t* title);
}
