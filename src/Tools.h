#pragma once
#include "Model.h"

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The AIs' tools. In chat mode the CLIs' own tools are disabled; an AI asks the
// app for an action with a fenced block:
//
//   ```outil
//   {"nom": "lire_note", "source": "vault", "chemin": "Chapitre 3.md"}
//   ```
//
// Reading tools run immediately and their results are handed back to the same
// AI. Everything that changes something (corrections, memory, questions, code
// work, skills, tasks) becomes an event the app handles, with the user's
// approval where required.
namespace Tools
{
    struct Call
    {
        std::string name;
        nlohmann::json args;
    };

    // Extracts tool calls and returns the text with the blocks removed.
    std::vector<Call> Extract(const std::string& reply, std::string& textWithoutCalls);

    bool IsReadTool(const std::string& name);

    // What the AIs are told they can do in a salon of this type.
    std::string Guide(ChannelType type, bool hasVault, bool hasLore, bool hasCode);

    // Folders an AI may read in this salon.
    struct Sources
    {
        struct Folder { std::filesystem::path path; bool canWrite = false; };
        struct Exclusion { std::filesystem::path path; std::string mode; };
        std::filesystem::path vault, lore;
        std::filesystem::path main;
        std::vector<Folder> additional;
        std::vector<Exclusion> exclusions;
        bool globalRead = false;
        std::filesystem::path dataRoot;   // for transcripts
        std::string subserverId;
        std::vector<std::string> channelIds; // salons of the sous-serveur
        std::string channelId;
    };

    // Runs a reading tool, returns its result as text for the AI.
    std::string RunRead(const Call& call, const Sources& sources, ChannelType type);

    // Resolves a writable project file. Only the primary folder and additional
    // folders explicitly marked Can Write are eligible; exclusions always win.
    // Returns an empty path and a user-facing error when access is refused.
    std::filesystem::path ResolveWrite(const Sources& sources, const std::string& source,
                                       const std::string& relative, std::string& error);

    // Resolves a path inside a root; empty on escape, `.obsidian`, or bad input.
    std::filesystem::path Confine(const std::filesystem::path& root, const std::string& relative);

    // Which source a file belongs to when folders are nested (e.g. vault = Jalyra/,
    // lore = Jalyra/lore): the most specific configured folder wins, so a file in
    // the lore folder is lore even though it is also inside the vault.
    // Returns "lore", "vault" or "" (outside both).
    std::string ZoneOf(const std::filesystem::path& file, const std::filesystem::path& vaultRoot,
                       const std::filesystem::path& loreRoot);
}
