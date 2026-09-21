#pragma once
#include <filesystem>
#include <string>
#include <vector>

// The console of Code salons: one command = one process, in the salon's code folder,
// inside a job (memory cap, time limit, kill), with secret-looking environment
// variables removed and secrets masked in what is shown and kept.
namespace Console
{
    enum class Profile { Cmd, PowerShell, Gcloud };
    const char* ProfileName(Profile profile);
    Profile ProfileFromName(const std::string& name); // unknown -> PowerShell

    inline constexpr unsigned kTimeoutSeconds = 600;

    // Names that look like credentials (TOKEN, KEY, SECRET, PASSWORD…, GH_TOKEN, AWS_*…).
    bool IsSecretEnvName(const std::wstring& name);
    // "-NAME" removals for Process::Run, from the current environment.
    std::vector<std::wstring> SecretEnvRemovals();

    // Replaces known key shapes (sk-…, ghp_…, AIza…, Bearer …) and the exact given secrets by ••••.
    std::string Mask(const std::string& text, const std::vector<std::string>& secrets);

    // A gcloud command: starts with "gcloud " and chains nothing (no & | ; < > ` $ ( ) % ^ or line break).
    bool ValidGcloud(const std::string& command, std::string& error);

    struct Prepared
    {
        std::vector<std::wstring> args;
        std::filesystem::path script; // temporary file to delete after the run ("" if none)
        std::string error;            // not runnable
    };
    // How to launch one command with a profile (scripts go to scratchDir).
    Prepared Prepare(Profile profile, const std::string& command, const std::filesystem::path& scratchDir);
}
