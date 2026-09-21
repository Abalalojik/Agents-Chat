// Checks the rules the app's safety relies on: path confinement, nested
// vault/lore zones, @mentions, and tool-call extraction.
// Build target: AgentChatsTests. Exit code 0 = all passed.

#include "../src/Conductor.h"
#include "../src/Platform.h"
#include "../src/Store.h"
#include "../src/Tools.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            std::printf("ECHEC ligne %d : %s\n", __LINE__, #cond);                  \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--connections")
    {
        for (const std::string ai : {"claude", "chatgpt", "gemini"})
        {
            const LoginStatus status = CheckLogin(ai);
            std::printf("%s: known=%s loggedIn=%s detail=%s\n", ai.c_str(), status.known ? "true" : "false",
                        status.loggedIn ? "true" : "false", status.detail.c_str());
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--turns")
    {
        const fs::path work = fs::temp_directory_path() / "agentchats-connection-test";
        std::error_code diagnosticEc;
        fs::create_directories(work, diagnosticEc);
        std::atomic<bool> cancel{false};
        struct Probe { const char* name; BackendKind kind; const char* model; const char* thinking; };
        const Probe probes[] = {{"claude", BackendKind::ClaudeCli, "claude-haiku-4-5", "auto"},
                                {"chatgpt", BackendKind::CodexCli, "gpt-5.6-luna", "low"}};
        int failures = 0;
        for (const Probe& probe : probes)
        {
            TurnRequest request;
            request.kind = probe.kind;
            request.model = probe.model;
            request.thinking = probe.thinking;
            request.systemPrompt = "Test de connexion. Réponds exactement : OK";
            request.prompt = "Réponds maintenant.";
            request.workDir = work.wstring();
            const TurnResult result = RunTurn(request, [](const std::string&) {}, cancel);
            std::printf("%s: ok=%s text=%s error=%s\n", probe.name, result.ok ? "true" : "false",
                        result.text.c_str(), result.error.c_str());
            failures += result.ok ? 0 : 1;
        }
        fs::remove_all(work, diagnosticEc);
        return failures == 0 ? 0 : 1;
    }
    CHECK(!Platform::ProjectRoot().empty());
    const fs::path base = fs::temp_directory_path() / "agentchats-tests";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path vault = base / "Jalyra";
    const fs::path lore = vault / "lore";
    const fs::path sibling = base / "Jalyra2";
    fs::create_directories(lore / "persos");
    fs::create_directories(vault / ".obsidian");
    fs::create_directories(sibling);
    std::ofstream(vault / "Chapitre 3.md") << "Lilith gele le jardin.";
    std::ofstream(lore / "persos" / "Lilith.md") << "Lilith : magie de glace.";
    std::ofstream(sibling / "secret.md") << "hors perimetre";

    // --- Atomic replacement really replaces on Windows ---------------------------------
    const fs::path atomic = base / "atomic.json";
    std::string atomicError;
    CHECK(Platform::WriteFileAtomic(atomic, "premier", atomicError));
    CHECK(Platform::WriteFileAtomic(atomic, "second", atomicError));
    {
        std::ifstream in(atomic, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(content == "second");
    }

    // --- Confine: stays inside the root --------------------------------------------------
    CHECK(!Tools::Confine(vault, "Chapitre 3.md").empty());
    CHECK(!Tools::Confine(vault, "lore/persos/Lilith.md").empty());
    CHECK(Tools::Confine(vault, "../Jalyra2/secret.md").empty());       // escape with ..
    CHECK(Tools::Confine(vault, "..\\Jalyra2\\secret.md").empty());
    CHECK(Tools::Confine(vault, "C:\\Windows\\win.ini").empty());       // absolute
    CHECK(Tools::Confine(vault, "\\Windows\\win.ini").empty());          // rooted
    CHECK(Tools::Confine(vault, ".obsidian/app.json").empty());          // hidden config
    CHECK(Tools::Confine(vault, "persos/../../Jalyra2/secret.md").empty());
    CHECK(Tools::Confine(vault, "").empty() || Tools::Confine(vault, "") == fs::weakly_canonical(vault, ec)); // root itself at most

    // --- Nested zones: the most specific folder wins ----------------------------------------
    CHECK(Tools::ZoneOf(vault / "Chapitre 3.md", vault, lore) == "vault");
    CHECK(Tools::ZoneOf(lore / "persos" / "Lilith.md", vault, lore) == "lore");
    CHECK(Tools::ZoneOf(sibling / "secret.md", vault, lore) == "");       // Jalyra2 is not inside Jalyra
    CHECK(Tools::ZoneOf(lore / "persos" / "Lilith.md", vault, {}) == "vault");
    // Reached through the vault root, a lore file is still lore.
    CHECK(Tools::ZoneOf(Tools::Confine(vault, "lore/persos/Lilith.md"), vault, lore) == "lore");

    // --- Mentions ------------------------------------------------------------------------------
    const std::vector<std::string> all = {"chatgpt", "claude", "gemini", "mistral", "deepseek", "grok"};
    CHECK((Conductor::Mentions("@claude tu en penses quoi ?", all) == std::vector<std::string>{"claude"}));
    CHECK((Conductor::Mentions("@gemini puis @claude", all) == std::vector<std::string>{"gemini", "claude"}));
    CHECK(Conductor::Mentions("@claudette n'est pas Claude", all).empty());
    CHECK(Conductor::Mentions("@tous", all).size() == all.size());
    CHECK(Conductor::Mentions("pas de mention", all).empty());

    // --- Tool extraction -------------------------------------------------------------------------
    std::string visible;
    auto calls = Tools::Extract("Je regarde.\n```outil\n{\"nom\": \"lire_note\", \"chemin\": \"a.md\"}\n```\nVoilà.", visible);
    CHECK(calls.size() == 1 && calls[0].name == "lire_note");
    CHECK(visible.find("```") == std::string::npos);
    calls = Tools::Extract("```outil\n[{\"nom\":\"retenir\",\"texte\":\"x\"},{\"nom\":\"tache\",\"titre\":\"y\"}]\n```", visible);
    CHECK(calls.size() == 2);
    calls = Tools::Extract("```outil\n{pas du json}\n```", visible);
    CHECK(calls.size() == 1 && calls[0].name == "__invalide");

    // --- Reading tools stay in their zone ----------------------------------------------------------
    Tools::Sources src;
    src.vault = vault;
    src.lore = lore;
    const std::string read = Tools::RunRead({"lire_note", {{"source", "vault"}, {"chemin", "../Jalyra2/secret.md"}}}, src, ChannelType::Detente);
    CHECK(read.find("hors perimetre") == std::string::npos);
    const std::string ok = Tools::RunRead({"lire_note", {{"source", "lore"}, {"chemin", "persos/Lilith.md"}}}, src, ChannelType::Detente);
    CHECK(ok.find("magie de glace") != std::string::npos);
    const std::string code = Tools::RunRead({"lire_note", {{"source", "vault"}, {"chemin", "Chapitre 3.md"}}}, src, ChannelType::Code);
    CHECK(code.find("Lilith") == std::string::npos); // no vault access from a Code salon

    // --- Mail/calendar tools read the provider-neutral local cache -----------------------
    src.dataRoot = base / "cloud-data";
    fs::create_directories(src.dataRoot / "cloud");
    std::ofstream(src.dataRoot / "cloud" / "cache.json") << R"({
      "syncedAt":"2026-09-21T12:00:00Z",
      "messages":[
        {"account":"Microsoft","subject":"Facture Hotmail","bodyPreview":"Montant 12 EUR","isRead":false,"receivedDateTime":"2026-09-21","from":{"emailAddress":{"name":"Boutique"}}},
        {"account":"Google","subject":"Projet Gmail","bodyPreview":"Réunion demain","isRead":true,"receivedDateTime":"2026-09-20","from":{"emailAddress":{"name":"Équipe"}}}
      ],
      "events":[
        {"account":"Microsoft","subject":"Dentiste","start":{"dateTime":"2026-09-22T10:00:00"},"end":{"dateTime":"2026-09-22T11:00:00"},"location":{"displayName":"Paris"}},
        {"account":"Google","subject":"Réunion projet","start":{"dateTime":"2026-09-23T09:00:00"},"end":{"dateTime":"2026-09-23T10:00:00"},"location":{"displayName":"Meet"}}
      ]})";
    const std::string mails = Tools::RunRead({"chercher_mails", {{"requete", ""}, {"non_lus", false}}}, src, ChannelType::Detente);
    CHECK(mails.find("Facture Hotmail") != std::string::npos && mails.find("Projet Gmail") != std::string::npos);
    const std::string unread = Tools::RunRead({"chercher_mails", {{"requete", ""}, {"non_lus", true}}}, src, ChannelType::Detente);
    CHECK(unread.find("Facture Hotmail") != std::string::npos && unread.find("Projet Gmail") == std::string::npos);
    const std::string agenda = Tools::RunRead({"lire_agenda", {{"requete", ""}}}, src, ChannelType::Detente);
    CHECK(agenda.find("Dentiste") != std::string::npos && agenda.find("Réunion projet") != std::string::npos);

    // --- Banking tools expose only the read-only local SimpleFIN cache ------------------
    std::ofstream(src.dataRoot / "cloud" / "finance.json") << R"({
      "syncedAt":"2026-09-21T12:05:00Z",
      "accounts":[{"id":"secret-id","name":"Compte courant","currency":"EUR","balance":"1234.56","available-balance":"1200.00",
        "transactions":[
          {"id":"tx1","posted":1789948800,"amount":"-42.50","description":"SUPERMARCHE","payee":"Marché local","pending":false},
          {"id":"tx2","posted":1789862400,"amount":"1800.00","description":"VIREMENT SALAIRE","pending":false}
        ]}]})";
    const std::string balances = Tools::RunRead({"solde_comptes", nlohmann::json::object()}, src, ChannelType::Detente);
    CHECK(balances.find("1234.56 EUR") != std::string::npos && balances.find("secret-id") == std::string::npos);
    const std::string expenses = Tools::RunRead({"chercher_transactions", {{"requete", "marché"}, {"depenses_seulement", true}}}, src, ChannelType::Detente);
    CHECK(expenses.find("-42.50 EUR") != std::string::npos && expenses.find("1800.00") == std::string::npos && expenses.find("tx1") == std::string::npos);

    // --- Shared memory deduplicates facts across AIs and overlapping scopes -------------
    const fs::path data = base / "data";
    fs::create_directories(data);
    Store store(data);
    Subserver* sub = store.CreateSubserver("Test", "", "", Platform::Narrow(base.wstring()));
    CHECK(sub != nullptr);
    Channel* channel = sub ? store.CreateChannel(*sub, "mémoire", ChannelType::Detente, "Français") : nullptr;
    CHECK(channel != nullptr);
    const std::string testSubId = sub ? sub->id : std::string();
    const std::string testChannelId = channel ? channel->id : std::string();
    bool created = false, promoted = false;
    const MemoryNote* first = channel ? store.AddMemory("salon", channel->id, "chatgpt",
        "Préfère être appelée Djenny", &created, &promoted) : nullptr;
    CHECK(first != nullptr && created && !promoted);
    const MemoryNote* duplicate = sub ? store.AddMemory("subserver", sub->id, "claude",
        "L'utilisatrice préfère qu'on l'appelle Djenny.", &created, &promoted) : nullptr;
    CHECK(duplicate != nullptr && !created && promoted);
    CHECK(store.Memory().size() == 1);
    CHECK(store.Memory()[0].level == "subserver");

    // The built-in self-improvement workspace is idempotent.
    CHECK(store.EnsureSelfImprovementSubserver(Platform::Narrow(base.wstring())));
    const size_t withSelfWorkspace = store.Subservers().size();
    CHECK(store.EnsureSelfImprovementSubserver(Platform::Narrow(base.wstring())));
    CHECK(store.Subservers().size() == withSelfWorkspace);
    Subserver* self = store.FindSubserver("agentchats-self");
    Channel* feedback = self ? store.FindChannel(*self, "agentchats-feedback") : nullptr;
    CHECK(feedback != nullptr && feedback->type == ChannelType::Bugs);

    // Folder access and Discord-like role permissions survive a reload.
    sub = store.FindSubserver(testSubId);
    channel = sub ? store.FindChannel(*sub, testChannelId) : nullptr;
    CHECK(sub != nullptr && store.UpdateFolderAccess(*sub, Platform::Narrow(base.wstring()),
        {{Platform::Narrow((base / "extra").wstring()), true}},
        {{Platform::Narrow((base / "secret").wstring()), "cant_access"}, {"private.txt", "cant_read"}}));
    TeamRole trusted{"Développeur local", "", true};
    trusted.globalRead = true;
    trusted.canWriteFiles = true;
    trusted.manageGithub = true;
    CHECK(channel != nullptr && store.SetRole(*channel, "chatgpt", &trusted));
    Store reloaded(data);
    CHECK(reloaded.Load());
    Subserver* loadedSub = reloaded.FindSubserver(testSubId);
    Channel* loadedChannel = loadedSub ? reloaded.FindChannel(*loadedSub, testChannelId) : nullptr;
    CHECK(loadedSub && loadedSub->additionalFolders.size() == 1 && loadedSub->additionalFolders[0].canWrite);
    CHECK(loadedSub && loadedSub->exclusions.size() == 2 && loadedSub->exclusions[1].mode == "cant_read");
    CHECK(loadedChannel && loadedChannel->roles["chatgpt"].globalRead && loadedChannel->roles["chatgpt"].manageGithub);

    // Tasks are shared across channels and can be reassigned from the global todo.
    const TaskItem* task = channel ? store.AddTask(channel->id, "Vérifier le bug", "user", "user") : nullptr;
    CHECK(task != nullptr && store.AllTasks().size() == 1);
    const std::string taskId = task ? task->id : std::string();
    CHECK(store.SetTaskAssignee(taskId, "chatgpt"));
    CHECK(store.AllTasks()[0].assignee == "chatgpt");
    CHECK(ChannelTypeFromKey("bugs", channel->type) && channel->type == ChannelType::Bugs);

    fs::remove_all(base, ec);
    std::printf(g_failures == 0 ? "Tous les tests passent.\n" : "%d test(s) en échec.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
