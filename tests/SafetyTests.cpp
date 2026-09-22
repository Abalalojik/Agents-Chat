// Checks the rules the app's safety relies on: path confinement, nested
// vault/lore zones, @mentions, and tool-call extraction.
// Build target: AgentChatsTests. Exit code 0 = all passed.

#include "../src/Conductor.h"
#include "../src/Console.h"
#include "../src/Process.h"
#include "../src/GitHub.h"
#include "../src/ReleaseSignature.h"
#include "../src/ShortContext.h"
#include "../src/Platform.h"
#include "../src/Settings.h"
#include "../src/Store.h"
#include "../src/Tools.h"
#include "../src/Updater.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
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
    if (argc > 2 && std::string(argv[1]) == "--github-read")
    {
        // Read-only: lists issues through the authenticated gh CLI.
        std::vector<GitHub::Issue> issues;
        const GitHub::Result r = GitHub::ListIssues(argv[2], issues);
        std::printf("github-read %s: ok=%s issues=%zu error=%s\n", argv[2], r.ok ? "true" : "false", issues.size(), r.error.c_str());
        for (size_t i = 0; i < issues.size() && i < 5; ++i)
            std::printf("  #%d [%s] %s\n", issues[i].number, issues[i].state.c_str(), issues[i].title.c_str());
        return r.ok ? 0 : 1;
    }
    if (argc > 3 && std::string(argv[1]) == "--propose-pr")
    {
        // PUBLISHES when <repo> has a GitHub remote: only on a throwaway repository or with approval.
        std::string url;
        const GitHub::Result r = GitHub::ProposePullRequest(argv[2], argv[3], "Diagnostic.", url);
        std::printf("propose-pr: ok=%s url=%s error=%s\n", r.ok ? "true" : "false", url.c_str(), r.error.c_str());
        return r.ok ? 0 : 1;
    }
    if (argc > 3 && std::string(argv[1]) == "--self-build")
    {
        // The self-improvement pipeline on a given source tree: configure, build, run its tests.
        Updater updater{fs::path(argv[3])};
        updater.BuildLocal(fs::path(argv[2]));
        std::optional<Updater::BuildReport> report;
        while (!(report = updater.TakeReport()))
            std::this_thread::sleep_for(std::chrono::seconds(2));
        std::printf("self-build: ok=%s built=%s tests=%s staged=%s\n%s\n--- log tail ---\n%s\n",
                    report->ok ? "true" : "false", report->built ? "true" : "false",
                    report->testsPassed ? "true" : "false", updater.LocalBuildReady() ? "true" : "false",
                    report->summary.c_str(), report->log.substr(report->log.size() > 1500 ? report->log.size() - 1500 : 0).c_str());
        return report->ok ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--connections")
    {
        for (const std::string ai : {"claude", "chatgpt", "gemini"})
        {
            const LoginStatus status = CheckLogin(ai);
            std::printf("%s: known=%s loggedIn=%s planActive=%s quotaKnown=%s quotaAvailable=%s plan=%s detail=%s\n",
                        ai.c_str(), status.known ? "true" : "false", status.loggedIn ? "true" : "false",
                        status.planActive ? "true" : "false", status.quotaKnown ? "true" : "false",
                        status.quotaAvailable ? "true" : "false", status.plan.c_str(), status.detail.c_str());
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
                                {"chatgpt", BackendKind::CodexCli, "gpt-5.6-luna", "low"},
                                {"gemini", BackendKind::GeminiCli, "gemini-3.8-flash-low", "low"}};
        int failures = 0;
        for (const Probe& probe : probes)
        {
            if (argc > 2 && std::string(argv[2]) != probe.name)
                continue;
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
    if (argc > 2 && std::string(argv[1]) == "--code-smoke")
    {
        const std::string ai = argv[2];
        const fs::path project = argc > 3 ? fs::path(Platform::Widen(argv[3])) : Platform::ProjectRoot();
        const fs::path marker = project / ("agentchats-smoke-" + ai + ".txt");
        std::error_code smokeEc;
        fs::remove(marker, smokeEc);
        const std::string expected = "AGENTS_CHAT_CODE_BRIDGE_OK_" + ai;
        std::string model, thinking = "low";
        if (ai == "claude") { model = "claude-haiku-4-5"; thinking = "auto"; }
        else if (ai == "chatgpt") model = "gpt-5.6-luna";
        else if (ai == "gemini") model = "gemini-3.8-flash-low";
        else return 2;
        std::atomic<bool> cancel{false};
        const std::string instruction = "Crée uniquement le fichier agentchats-smoke-" + ai +
            ".txt à la racine du projet, contenant exactement cette ligne : " + expected +
            "\nN'exécute aucune commande et ne modifie rien d'autre.";
        const TurnResult result = RunCodeWork(ai, model, thinking, instruction, project.wstring(), "",
            [](const std::string& line) { std::printf("progress: %s\n", line.c_str()); }, cancel);
        std::string actual;
        if (fs::exists(marker, smokeEc))
        {
            std::ifstream in(marker, std::ios::binary);
            actual.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const bool valid = result.ok && actual.find(expected) != std::string::npos;
        std::printf("%s code: ok=%s marker=%s summary=%s error=%s\n", ai.c_str(), result.ok ? "true" : "false",
                    valid ? "valid" : "invalid", result.text.c_str(), result.error.c_str());
        fs::remove(marker, smokeEc);
        return valid ? 0 : 1;
    }
    CHECK(!Platform::ProjectRoot().empty());
    // A visible but exhausted/disconnected agent must never receive work.
    CHECK(PresenceCanWork(PresenceState::Online));
    CHECK(PresenceCanWork(PresenceState::Unknown));
    CHECK(PresenceCanWork(PresenceState::Limited));
    CHECK(!PresenceCanWork(PresenceState::Exhausted));
    CHECK(!PresenceCanWork(PresenceState::Offline));
    CHECK(!PresenceCanWork(PresenceState::NotConnected));

    const fs::path presenceRoot = fs::temp_directory_path() / "agentchats-presence-tests";
    std::error_code presenceEc;
    fs::remove_all(presenceRoot, presenceEc);
    fs::create_directories(presenceRoot, presenceEc);
    Settings presenceSettings(presenceRoot);
    CHECK(presenceSettings.CommitEmail().empty()); // a release must never inherit the maintainer's identity
    presenceSettings.SetDetectedPresence("chatgpt", "chat", {PresenceState::Exhausted, "quota épuisé", ""});
    CHECK(!PresenceCanWork(presenceSettings.GetPresence("chatgpt", "chat").state));
    CHECK(presenceSettings.PutBackOnline("chatgpt", "chat"));
    CHECK(PresenceCanWork(presenceSettings.GetPresence("chatgpt", "chat").state));
    Settings reloadedPresence(presenceRoot);
    CHECK(reloadedPresence.Load());
    CHECK(reloadedPresence.GetPresence("chatgpt", "chat").state == PresenceState::Online);
    CHECK(reloadedPresence.SetPresence("chatgpt", "chat", {PresenceState::Offline, "économie", ""}));
    CHECK(!PresenceCanWork(reloadedPresence.GetPresence("chatgpt", "chat").state));
    CHECK(reloadedPresence.UseDetectedPresence("chatgpt", "chat"));
    CHECK(reloadedPresence.GetPresence("chatgpt", "chat").state == PresenceState::NotConnected);
    fs::remove_all(presenceRoot, presenceEc);
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
#ifdef _WIN32
    CHECK(Tools::Confine(vault, "C:\\Windows\\win.ini").empty());       // absolute
    CHECK(Tools::Confine(vault, "\\Windows\\win.ini").empty());          // rooted
#else
    CHECK(Tools::Confine(vault, "/etc/passwd").empty());                // absolute
    CHECK(Tools::Confine(vault, "//etc/passwd").empty());
#endif
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
    src.main = vault;
    const std::string read = Tools::RunRead({"lire_note", {{"source", "vault"}, {"chemin", "../Jalyra2/secret.md"}}}, src, ChannelType::Detente);
    CHECK(read.find("hors perimetre") == std::string::npos);
    const std::string ok = Tools::RunRead({"lire_note", {{"source", "lore"}, {"chemin", "persos/Lilith.md"}}}, src, ChannelType::Detente);
    CHECK(ok.find("magie de glace") != std::string::npos);
    const std::string code = Tools::RunRead({"lire_note", {{"source", "vault"}, {"chemin", "Chapitre 3.md"}}}, src, ChannelType::Code);
    CHECK(code.find("Lilith") == std::string::npos); // no vault access from a Code salon
    const std::string projectFile = Tools::RunRead({"lire_note", {{"source", "principal"}, {"chemin", "Chapitre 3.md"}}}, src, ChannelType::Code);
    CHECK(projectFile.find("Lilith") != std::string::npos);
    CHECK(Tools::Guide(ChannelType::Code, false, false, true).find("source\": \"principal") != std::string::npos);
    CHECK(Tools::Guide(ChannelType::Code, false, false, true).find("ecrire_fichier") != std::string::npos);
    CHECK(Tools::Guide(ChannelType::Analyse, false, false, true).find("OUTILS NATIFS DU PROJET") != std::string::npos);
    CHECK(Tools::Guide(ChannelType::Analyse, false, false, true).find("ecrire_fichier") == std::string::npos);
    CHECK(Tools::Guide(ChannelType::Code, false, false, true).find("demander_autorisation") != std::string::npos);
    const std::string numbered = Tools::RunRead({"lire", {{"source", "principal"}, {"chemin", "Chapitre 3.md"}, {"debut", 1}, {"fin", 1}}}, src, ChannelType::Analyse);
    CHECK(numbered.find("1: Lilith") != std::string::npos);
    const std::string recursive = Tools::RunRead({"lister", {{"source", "principal"}, {"chemin", ""}, {"recursif", true}}}, src, ChannelType::Analyse);
    CHECK(recursive.find("lore/persos/Lilith.md") != std::string::npos && recursive.find("octets") != std::string::npos);
    const std::string searched = Tools::RunRead({"chercher", {{"source", "principal"}, {"motif", "magie.*glace"}, {"extensions", {".md"}}, {"regex", true}}}, src, ChannelType::Analyse);
    CHECK(searched.find("lore/persos/Lilith.md:1:") != std::string::npos);

    // --- Project writes honor aliases, Can Write and exclusions ----------------------------
    std::string writeError;
    CHECK(!Tools::ResolveWrite(src, "principal", "nouveau.cpp", writeError).empty());
    CHECK(Tools::ResolveWrite(src, "principal", "../dehors.cpp", writeError).empty());
    CHECK(Tools::ResolveWrite(src, "principal", ".git/config", writeError).empty());
    src.additional.push_back({lore, false});
    CHECK(Tools::ResolveWrite(src, "dossier_0", "persos/Lilith.md", writeError).empty());
    src.additional[0].canWrite = true;
    CHECK(!Tools::ResolveWrite(src, "dossier_0", "persos/Lilith.md", writeError).empty());
    src.exclusions.push_back({fs::path("persos"), "cant_read"});
    CHECK(Tools::ResolveWrite(src, "dossier_0", "persos/Lilith.md", writeError).empty());
    CHECK(Tools::ResolveWrite(src, "global", "C:/Windows/win.ini", writeError).empty());

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

    // --- GitHub: repository slugs and issue import ------------------------------------------------
    CHECK(GitHub::RepoSlug("https://github.com/Abalalojik/Agents-Chat") == "Abalalojik/Agents-Chat");
    CHECK(GitHub::RepoSlug("https://github.com/Abalalojik/Agents-Chat.git") == "Abalalojik/Agents-Chat");
    CHECK(GitHub::RepoSlug("git@github.com:Abalalojik/Agents-Chat.git") == "Abalalojik/Agents-Chat");
    CHECK(GitHub::ContributionBranch("main", "20260921-101112") == "amelioration/20260921-101112");

    // Console: secret-looking variables removed, keys masked, gcloud cannot chain, real runs.
    {
        CHECK(Console::IsSecretEnvName(L"GH_TOKEN"));
        CHECK(Console::IsSecretEnvName(L"openai_api_key"));
        CHECK(Console::IsSecretEnvName(L"AWS_SECRET_ACCESS_KEY"));
        CHECK(Console::IsSecretEnvName(L"MY_PASSWORD"));
        CHECK(!Console::IsSecretEnvName(L"PATH"));
        CHECK(!Console::IsSecretEnvName(L"USERPROFILE"));
        CHECK(!Console::IsSecretEnvName(L"SystemRoot"));
        const std::string masked = Console::Mask("key sk-ant-abcdefghijklmnopqrstuv and ghp_abcdefghijklmnopqrstuvwxyz1234 "
                                                 "AIzaSyA1234567890abcdefghijklmnopqrstu Authorization: Bearer abc.def "
                                                 "mine=Sup3rS3cretValue ok",
                                                 {"Sup3rS3cretValue"});
        CHECK(masked.find("sk-ant-abc") == std::string::npos);
        CHECK(masked.find("ghp_abc") == std::string::npos);
        CHECK(masked.find("AIzaSy") == std::string::npos);
        CHECK(masked.find("abc.def") == std::string::npos);
        CHECK(masked.find("Sup3rS3cretValue") == std::string::npos);
        CHECK(masked.find(" ok") != std::string::npos);
        std::string why;
        CHECK(Console::ValidGcloud("gcloud config list", why));
        CHECK(!Console::ValidGcloud("gcloud config list & del x", why));
        CHECK(!Console::ValidGcloud("gcloud a | b", why));
        CHECK(!Console::ValidGcloud("gcloud $(x)", why));
        CHECK(!Console::ValidGcloud("dir", why));
        CHECK(!Console::Prepare(Console::Profile::Gcloud, "del *", fs::temp_directory_path()).error.empty());

        const fs::path consoleRoot = fs::temp_directory_path() / "agentchats-console-tests";
        std::error_code consoleEc;
        fs::create_directories(consoleRoot / "work", consoleEc);
#ifdef _WIN32
        SetEnvironmentVariableW(L"AGENTCHATS_TEST_TOKEN", L"do-not-leak-0123456789");
#else
        setenv("AGENTCHATS_TEST_TOKEN", "do-not-leak-0123456789", 1);
#endif
        std::vector<std::wstring> env = Console::SecretEnvRemovals();
        auto runConsole = [&](Console::Profile profile, const std::string& command, std::string& out) {
            const Console::Prepared prepared = Console::Prepare(profile, command, consoleRoot / "scripts");
            std::atomic<bool> noCancel{false};
            out.clear();
            const Process::Result r = Process::Run(prepared.args, (consoleRoot / "work").wstring(), "",
                                                   [&](const std::string& line) { out += line + "\n"; }, noCancel, env, 60);
            if (!prepared.script.empty())
                fs::remove(prepared.script, consoleEc);
            return prepared.error.empty() && r.started && r.exitCode == 0;
        };
        std::string out;
        // PowerShell is always there on Windows; on Linux only when pwsh is installed.
        if (!Console::Prepare(Console::Profile::PowerShell, "Write-Output 1", consoleRoot / "scripts").args.empty())
        {
            CHECK(runConsole(Console::Profile::PowerShell, "Write-Output \"[$env:AGENTCHATS_TEST_TOKEN]\"; Write-Output 'héllo « ok »'", out));
            CHECK(out.find("do-not-leak") == std::string::npos);
            CHECK(out.find("[]") != std::string::npos);
            CHECK(out.find("héllo « ok »") != std::string::npos);
        }
#ifdef _WIN32
        CHECK(runConsole(Console::Profile::Cmd, "echo [%AGENTCHATS_TEST_TOKEN%] & cd", out));
#else
        CHECK(runConsole(Console::Profile::Cmd, "echo \"[$AGENTCHATS_TEST_TOKEN]\"; pwd", out));
#endif
        CHECK(out.find("do-not-leak") == std::string::npos);
        CHECK(out.find("agentchats-console-tests") != std::string::npos); // ran in the salon folder
#ifdef _WIN32
        CHECK(!runConsole(Console::Profile::Cmd, "exit /b 3", out));
        SetEnvironmentVariableW(L"AGENTCHATS_TEST_TOKEN", nullptr);
#else
        CHECK(!runConsole(Console::Profile::Cmd, "exit 3", out));
        unsetenv("AGENTCHATS_TEST_TOKEN");
#endif
        fs::remove_all(consoleRoot, consoleEc);
    }

    // Short context: accent folding, stop words, relevance, and the thread budget.
    {
        const std::vector<std::string> terms = ShortContext::Terms("Le Château de Jalyra ÉTAIT détruit, cœur & l'été");
        CHECK(std::find(terms.begin(), terms.end(), "chateau") != terms.end());
        CHECK(std::find(terms.begin(), terms.end(), "jalyra") != terms.end());
        CHECK(std::find(terms.begin(), terms.end(), "detruit") != terms.end());
        CHECK(std::find(terms.begin(), terms.end(), "coeur") != terms.end());
        CHECK(std::find(terms.begin(), terms.end(), "les") == terms.end());
        CHECK(std::find(terms.begin(), terms.end(), "ete") == terms.end());
        const std::vector<std::string> docs = {"On parle de la météo.", "Le château de Jalyra a trois tours.",
                                               "Recette de crêpes.", "Jalyra est au nord."};
        const std::vector<size_t> ranked = ShortContext::Rank(docs, "Combien de tours au château de Jalyra ?", 5);
        CHECK(!ranked.empty() && ranked.front() == 1);
        CHECK(std::find(ranked.begin(), ranked.end(), 3) != ranked.end());
        CHECK(std::find(ranked.begin(), ranked.end(), 0) == ranked.end());
        CHECK(ShortContext::Rank(docs, "le la de", 5).empty());

        JobInput job;
        job.userName = "Djenny";
        std::vector<Message> thread;
        thread.push_back({"m0", "user", "Le château de Jalyra a trois tours de basalte.", "2026-09-21T10:00:00Z", "", ""});
        for (int i = 1; i <= 60; ++i)
            thread.push_back({"m" + std::to_string(i), "claude", std::string(400, 'x') + " remplissage " + std::to_string(i),
                              "2026-09-21T10:00:00Z", "", ""});
        thread.push_back({"last", "user", "Rappelle-moi la matière des tours du château ?", "2026-09-21T11:00:00Z", "", ""});
        const std::string convo = Conductor::BuildConversation(job, thread, "claude");
        CHECK(convo.size() < 24000);
        CHECK(convo.find("basalte") != std::string::npos);            // old but relevant: kept
        CHECK(convo.find("remplissage 1\n") == std::string::npos);   // old and irrelevant: dropped
        CHECK(convo.find("Rappelle-moi la matière") != std::string::npos);
        CHECK(convo.find("chercher_historique") != std::string::npos);
    }
    CHECK(GitHub::ContributionBranch("HEAD", "x") == "amelioration/x");
    CHECK(GitHub::ContributionBranch("amelioration/20260920-090000", "x") == "amelioration/20260920-090000");
    CHECK(GitHub::RepoSlug("https://github.com/Abalalojik/Agents-Chat/issues") == "Abalalojik/Agents-Chat");
    CHECK(GitHub::RepoSlug("https://gitlab.com/a/b").empty());
    CHECK(GitHub::RepoSlug("https://github.com/a b/c").empty());       // no spaces reach gh arguments
    CHECK(GitHub::RepoSlug("https://github.com/owner").empty());
    {
        const fs::path issueRoot = fs::temp_directory_path() / "agentchats-issue-tests";
        std::error_code issueEc;
        fs::remove_all(issueRoot, issueEc);
        fs::create_directories(issueRoot, issueEc);
        Store issueStore(issueRoot);
        std::vector<GitHub::Issue> issues = {{12, "Crash au démarrage", "OPEN", "https://github.com/o/r/issues/12", ""},
                                             {13, "Déjà corrigée", "CLOSED", "https://github.com/o/r/issues/13", ""}};
        Store::IssueImport firstImport = issueStore.ImportIssues("salon", issues);
        CHECK(firstImport.created == 1);                   // closed issues unknown locally are not imported
        CHECK(issueStore.Tasks("salon").size() == 1);
        CHECK(issueStore.Tasks("salon")[0].issueNumber == 12);
        CHECK(issueStore.Tasks("salon")[0].title == "#12 Crash au démarrage");
        Store::IssueImport again = issueStore.ImportIssues("salon", issues);
        CHECK(again.created == 0 && again.updated == 0 && again.closed == 0); // idempotent
        issues[0].state = "CLOSED";
        Store::IssueImport closed = issueStore.ImportIssues("salon", issues);
        CHECK(closed.closed == 1);
        CHECK(issueStore.Tasks("salon")[0].status == "fait");
        Store issueReloaded(issueRoot);
        CHECK(issueReloaded.Load());
        CHECK(issueReloaded.Tasks("salon").size() == 1 && issueReloaded.Tasks("salon")[0].issueState == "CLOSED");
        fs::remove_all(issueRoot, issueEc);
    }

    // --- Signed updates: any change to the executable, version or key is rejected ---------------------
    {
        const fs::path sigRoot = fs::temp_directory_path() / "agentchats-signature-tests";
        std::error_code sigEc;
        fs::remove_all(sigRoot, sigEc);
        fs::create_directories(sigRoot, sigEc);
        const fs::path exe = sigRoot / "AgentChats.exe";
        std::ofstream(exe, std::ios::binary) << "MZ fake executable for the signature test";
        std::vector<unsigned char> priv, otherPriv;
        std::string pub, otherPub;
        CHECK(ReleaseSignature::GenerateKeyPair(priv, pub));
        CHECK(ReleaseSignature::GenerateKeyPair(otherPriv, otherPub));
        const std::string digest = ReleaseSignature::Sha256File(exe);
        CHECK(digest.size() == 64);
        const std::string sig = ReleaseSignature::Sign(priv, "v9.9.9", digest);
        CHECK(!sig.empty());
        CHECK(ReleaseSignature::Verify("v9.9.9", digest, sig, {pub}));                 // genuine
        CHECK(ReleaseSignature::Verify("v9.9.9", digest, sig, {otherPub, pub}));       // key rotation list
        CHECK(!ReleaseSignature::Verify("v9.9.8", digest, sig, {pub}));                // other version
        CHECK(!ReleaseSignature::Verify("v9.9.9", digest, sig, {otherPub}));           // unknown key
        CHECK(!ReleaseSignature::Verify("v9.9.9", digest, sig.substr(0, sig.size() / 2), {pub})); // truncated
        CHECK(!ReleaseSignature::Verify("v9.9.9", digest, "", {pub}));                 // missing
        CHECK(!ReleaseSignature::Verify("v9.9.9", digest, "pas du base64 !", {pub}));  // garbage
        CHECK(!ReleaseSignature::Verify("v9.9.9", digest, sig, {}));                   // no trusted key
        {
            std::ofstream tamper(exe, std::ios::binary | std::ios::app);
            tamper << "X"; // one extra byte
        }
        CHECK(!ReleaseSignature::Verify("v9.9.9", ReleaseSignature::Sha256File(exe), sig, {pub}));
        CHECK(!ReleaseSignature::Verify("v9.9.9", "ZZ", sig, {pub}));                  // malformed digest
        if (!ReleaseSignature::HasTrustedKeys())
            CHECK(!ReleaseSignature::VerifyRelease("v9.9.9", digest, sig));             // nothing embedded yet
        fs::remove_all(sigRoot, sigEc);
    }
    std::printf(g_failures == 0 ? "Tous les tests passent.\n" : "%d test(s) en échec.\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
