#pragma once
#include <map>
#include <string>
#include <vector>

// What an AI may do in a channel is decided by its
// type (which tools it gets), never by prompt wording alone.
enum class ChannelType
{
    Analyse,           // critique of the writing; may correct vault notes (diff card)
    Detente,           // casual chat; vault + lore read-only, no write tool
    ConsolidationLore, // may modify lore files (diff card)
    Code,              // only type allowed to send instructions to code agents
    Bugs,              // issue triage and fixes, backed by the project's GitHub repository
};

// How strong (and costly) a model to use. A settings table maps each level to a
// concrete model per AI, so salons pick a level, never a model name.
enum class ModelLevel
{
    Leger,
    Normal,
    Fort,
};

const char* ModelLevelLabel(ModelLevel level);
const char* ModelLevelKey(ModelLevel level);
bool ModelLevelFromKey(const std::string& key, ModelLevel& out);
ModelLevel DefaultLevelFor(ChannelType type);

const char* ChannelTypeLabel(ChannelType type);
const char* ChannelTypeKey(ChannelType type); // stable id used in saved files
bool ChannelTypeFromKey(const std::string& key, ChannelType& out);

// A team role given to one AI in one salon. At most one lead per salon: it
// receives messages that name nobody and delegates by @mentioning teammates.
struct TeamRole
{
    std::string name;
    std::string instructions;
    bool isLead = false;
    bool globalRead = false;
    bool canWriteFiles = false;
    bool manageTasks = true;
    bool manageGithub = false;
};

struct RolePreset
{
    const char* name;
    const char* instructions;
    bool isLead;
};

inline constexpr RolePreset kRolePresets[] = {
    {"Chef d'équipe",
     "Tu coordonnes l'équipe : découpe chaque demande en tâches concrètes, confie-les aux coéquipiers dont le rôle "
     "correspond en les mentionnant, vérifie et intègre leurs résultats.",
     true},
    {"Développeur", "Tu implémentes les tâches qu'on te confie, complètement, et tu signales ce qui reste incertain.", false},
    {"Relecteur", "Tu relis le travail de l'équipe : bugs, incohérences, cas limites. Remarques précises avec le correctif proposé.", false},
    {"Testeur", "Tu vérifies que le travail fonctionne : tests, cas limites, et tu signales ce qui échoue.", false},
    {"Architecte", "Tu conçois la structure et les choix d'ensemble, et tu laisses les détails aux autres.", false},
    {"Chercheur", "Tu prends en charge les recherches documentaires et web. Vérifie les faits, date les informations instables, cite les sources et distingue clairement preuve, inférence et incertitude.", false},
    {"Éditeur", "Tu relis les textes pour la langue, le rythme et la clarté, sans changer la voix de l'autrice.", false},
    {"Gardien du lore", "Tu veilles à la cohérence avec le canon : personnages, chronologie, règles du monde.", false},
};

struct Channel
{
    std::string id;
    std::string name;
    ChannelType type = ChannelType::Detente;
    std::string language = "Français"; // the AIs answer in this language, in this salon
    ModelLevel level = ModelLevel::Normal;     // level of every AI in this salon...
    std::map<std::string, ModelLevel> aiLevels;  // ...unless overridden for one AI (key: Sender id)
    std::map<std::string, TeamRole> roles;       // ai id -> role in this salon

    ModelLevel LevelFor(const std::string& ai) const
    {
        const auto it = aiLevels.find(ai);
        return it != aiLevels.end() ? it->second : level;
    }

    std::string Lead() const
    {
        for (const auto& [ai, role] : roles)
            if (role.isLead)
                return ai;
        return {};
    }
};

// Languages offered in the UI (any other name can be typed in).
inline constexpr const char* kLanguages[] = {
    "Français", "English", "Español", "Deutsch", "Italiano", "Português", "Nederlands"};

// A Subserver ("sous-serveur" in the UI) is one universe or project, like a
// server in a chat app. Its sources are shared by all its salons.
struct Subserver
{
    struct FolderAccess
    {
        std::string path;
        bool canWrite = false;
    };
    struct ExclusionRule
    {
        std::string path;
        std::string mode = "cant_access"; // cant_access or cant_read
    };

    std::string id;
    std::string name;
    std::string mainPath; // primary folder shared with chat and code agents
    std::vector<FolderAccess> additionalFolders;
    std::vector<ExclusionRule> exclusions;
    std::string vaultPath; // Obsidian vault (analysis / relaxation)
    std::string lorePath;  // canon reference, read-only except lore consolidation
    std::string codePath;  // project folder for code channels
    std::string githubUrl; // optional explicit repository for Bugs/GitHub tools
    std::vector<Channel> channels;
};

// Participant ids as written in transcripts.
namespace Sender
{
    inline constexpr const char* User = "user";
    inline constexpr const char* ChatGpt = "chatgpt";
    inline constexpr const char* Claude = "claude";
    inline constexpr const char* Gemini = "gemini";
    inline constexpr const char* System = "system";
    // Code twins, when they report in a salon
    inline constexpr const char* Codex = "codex";
    inline constexpr const char* ClaudeCode = "claude-code";
    inline constexpr const char* GeminiCli = "gemini-cli"; // stable transcript id; displayed as Antigravity CLI
}

struct Message
{
    std::string id;
    std::string sender;    // one of Sender::* or an AI id
    std::string content;   // UTF-8 markdown
    std::string timestamp; // ISO 8601 UTC
    std::string kind;      // "" = normal; "memory" = an AI remembered something (ref = note id)
    std::string ref;
};

// Discord-like presence of one AI on one tier ("chat" = the AI in the salons,
// "code" = its code twin). Offline comes from quota-type errors (or a manual
// pause); it ends at `untilIso`, or when the user puts the AI back online.
enum class PresenceState
{
    NotConnected, // not installed / not signed in / no API key
    Unknown,      // authenticated plan, but the provider exposes no headless quota counter
    Online,
    Limited,      // no plan, but paid/prepaid API credits are available
    Exhausted,    // plan exists, but its current quota window is exhausted
    Offline,      // authenticated, but neither a plan nor credits are available (or manual pause)
};

inline bool PresenceCanWork(PresenceState state)
{
    return state == PresenceState::Online || state == PresenceState::Unknown || state == PresenceState::Limited;
}

struct Presence
{
    PresenceState state = PresenceState::NotConnected;
    std::string reason;   // e.g. "crédits épuisés", "limite de débit", "pause manuelle"
    std::string untilIso; // empty = until the user puts it back online
};

// A request addressed to the user, gathered in the "Boîte aux lettres".
struct InboxItem
{
    std::string id;
    std::string kind;        // "correction", "file_write", "question", "skill", "code"
    std::string subserverId;
    std::string channelId;
    std::string ai;          // who asked
    std::string payload;     // JSON arguments of the request
    std::string status;      // "attente", "accepte", "refuse"
    std::string answer;      // reply to a question / outcome note
    std::string createdAt;
    std::string decidedAt;
    bool blocking = false;   // an agent is waiting on it
};

// One remembered fact. level: "toi" (everywhere), "subserver", "salon".
struct MemoryNote
{
    std::string id;
    std::string level;
    std::string scopeId; // subserver id or channel id ("" for "toi")
    std::string ai;
    std::string text;
    std::string createdAt;
};

// A task on a salon's board.
struct TaskItem
{
    std::string id;
    std::string channelId;
    std::string title;
    std::string assignee; // ai id or "" / "user"
    std::string status;   // "a_faire", "en_cours", "fait", "bloque"
    std::string createdBy;
    std::string updatedAt;
    // Linked GitHub issue (Bugs salons); 0 = none.
    int issueNumber = 0;
    std::string issueUrl;
    std::string issueState; // "OPEN" / "CLOSED" as last seen on GitHub
};
