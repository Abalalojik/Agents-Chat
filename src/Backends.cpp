#include "Backends.h"
#include "Http.h"
#include "Platform.h"
#include "Process.h"
#include "Settings.h"

#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    std::wstring EnvPath(const wchar_t* name);

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool Contains(const std::string& haystackLower, std::initializer_list<const char*> needles)
    {
        for (const char* n : needles)
            if (haystackLower.find(n) != std::string::npos)
                return true;
        return false;
    }

    std::string IsoFromEpoch(long long epochSeconds)
    {
        const std::time_t t = static_cast<std::time_t>(epochSeconds);
        std::tm utc{};
        gmtime_s(&utc, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buf;
    }

    std::string IsoInSeconds(long long seconds)
    {
        return IsoFromEpoch(static_cast<long long>(std::time(nullptr)) + seconds);
    }

    // Classifies an error text: quota-type errors take the AI offline.
    void ClassifyError(TurnResult& r, const std::string& text)
    {
        const std::string l = Lower(text);
        r.error = text;
        if (Contains(l, {"no longer supported", "migrate to the antigravity"}))
        {
            r.quota = true; // no date: Google refuses this sign-in for Gemini CLI
            r.reason = "refusé par Google (Code Assist individuel fermé)";
            r.error = "Google n'accepte plus Gemini CLI avec cette connexion (offre « Gemini Code Assist for individuals ») "
                      "et renvoie vers Antigravity. Pour Gemini en chat, ajoute une clé AI Studio (Options → Connexions).";
            return;
        }
        if (Contains(l, {"credit balance", "insufficient_quota", "out of credits", "billing", "payment required"}))
        {
            r.quota = true;
            r.reason = "crédits épuisés";
        }
        else if (Contains(l, {"usage limit", "rate limit", "rate_limit", "quota", "resource_exhausted", "429",
                              "too many requests", "limit reached", "hit your limit"}))
        {
            r.quota = true;
            r.reason = "limite atteinte";
        }
        else if (Contains(l, {"not logged in", "please run", "/login", "auth method", "unauthorized", "401",
                              "invalid api key", "invalid x-api-key", "authentication"}))
        {
            r.quota = true; // no date: stays offline until the user fixes it and puts it back
            r.reason = "non connectée";
        }
    }

    std::string Shorten(const std::string& s, size_t max = 600)
    {
        return s.size() > max ? s.substr(s.size() - max) : s;
    }

    // Gemini CLI relaunches itself with a heap limit of half the RAM unless told not to.
    const std::vector<std::wstring> kGeminiEnv = {L"GEMINI_CLI_NO_RELAUNCH=true"};

    std::vector<std::wstring> CodexEnvironment()
    {
        // Recent Codex builds require HOME even on Windows. Explorer-launched
        // GUI applications commonly expose USERPROFILE but no HOME.
#ifdef _WIN32
        wchar_t profile[32768];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile)));
        return (n > 0 && n < std::size(profile))
                   ? std::vector<std::wstring>{L"HOME=" + std::wstring(profile)}
                   : std::vector<std::wstring>{};
#else
        return {}; // HOME is always set on Linux
#endif
    }

    const std::vector<std::wstring> kCodexEnv = CodexEnvironment();

    bool IsAntigravity(const AgentInstall& agent)
    {
        return agent.found && agent.script.empty() &&
               Lower(Platform::Narrow(fs::path(agent.exe).stem().wstring())) == "agy";
    }

    void EnsureAntigravityStatusHook()
    {
        const fs::path settings = fs::path(EnvPath(L"USERPROFILE")) / L".gemini" / L"antigravity-cli" / L"settings.json";
        json value = json::object();
        try
        {
            std::ifstream in(settings, std::ios::binary);
            if (in) in >> value;
        }
        catch (...) { return; }
#ifdef _WIN32
        wchar_t exe[32768];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (!n || n >= std::size(exe))
            return;
        const std::wstring executable(exe, n);
#else
        std::error_code selfEc;
        const std::wstring executable = fs::read_symlink("/proc/self/exe", selfEc).wstring();
        if (executable.empty())
            return;
#endif
        if (Lower(Platform::Narrow(fs::path(executable).stem().wstring())) != "agentchats")
            return; // connection tests must never register themselves as the collector
        if (value.contains("statusLine"))
        {
            const std::string existing = value["statusLine"].value("command", std::string());
            if (existing.find("--capture-antigravity-status") == std::string::npos)
                return; // never replace a user-owned status line
        }
#ifdef _WIN32
        wchar_t shortExe[32768];
        const DWORD shortCount = GetShortPathNameW(executable.c_str(), shortExe, static_cast<DWORD>(std::size(shortExe)));
        const std::wstring shortPath = shortCount && shortCount < std::size(shortExe)
            ? std::wstring(shortExe, shortCount) : std::wstring();
        const bool shellSafeShortPath = !shortPath.empty() && shortPath.find_first_of(L" \t\"") == std::wstring::npos;
        const std::string command = shellSafeShortPath
            ? Platform::Narrow(shortPath) + " --capture-antigravity-status"
            : "powershell.exe -NoProfile -Command \"& '" + Platform::Narrow(executable) + "' --capture-antigravity-status\"";
#else
        std::string quoted;
        for (char c : Platform::Narrow(executable))
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        const std::string command = "'" + quoted + "' --capture-antigravity-status";
#endif
        value["statusLine"] = {
            {"type", "command"},
            {"command", command},
            {"enabled", true},
            {"stack_with_default", true}
        };
        std::error_code ec;
        fs::create_directories(settings.parent_path(), ec);
        std::string error;
        Platform::WriteFileAtomic(settings, value.dump(2), error);
    }

    bool ReadAntigravityQuota(LoginStatus& s)
    {
        try
        {
            std::ifstream in(Platform::DataRoot() / "antigravity-status.json", std::ios::binary);
            if (!in) return false;
            json value;
            in >> value;
            s.plan = value.value("plan_tier", std::string());
            if (s.plan.empty() || !value.contains("quota") || !value["quota"].is_object())
                return false;
            s.planActive = true;
            s.quotaKnown = !value["quota"].empty();
            s.quotaAvailable = false;
            double remaining = 0.0;
            long long longestReset = 0;
            for (const auto& [_, quota] : value["quota"].items())
                if (quota.is_object() && quota.contains("remaining_fraction"))
                {
                    const double percent = std::clamp(quota.value("remaining_fraction", 0.0) * 100.0, 0.0, 100.0);
                    // Model families use independent pools. The agent remains
                    // routable while at least one family has capacity.
                    remaining = std::max(remaining, percent);
                    s.quotaAvailable = s.quotaAvailable || percent > 0.0;
                    if (percent <= 0.0)
                    {
                        longestReset = std::max(longestReset, quota.value("reset_in_seconds", 0LL));
                        if (quota.contains("reset_time") && quota["reset_time"].is_string())
                            s.resetsAt = quota["reset_time"].get<std::string>();
                    }
                }
            if (!s.quotaKnown) return false;
            if (!s.quotaAvailable && s.resetsAt.empty() && longestReset > 0)
                s.resetsAt = IsoInSeconds(longestReset);
            if (!s.quotaAvailable && !s.resetsAt.empty() && s.resetsAt <= Platform::NowIsoUtc())
            {
                s.quotaKnown = false;
                s.resetsAt.clear();
                return false;
            }
            s.remainingPercent = remaining;
            s.detail = s.quotaAvailable ? "forfait Antigravity " + s.plan + " · au moins une famille disponible (" +
                                           std::to_string(static_cast<int>(remaining + 0.5)) + "%)"
                                        : "tous les quotas Antigravity sont épuisés";
            return true;
        }
        catch (...) { return false; }
    }

    std::string AntigravityModel(const std::string& configured)
    {
        // Preserve explicit Antigravity model ids. Migrate the retired Gemini
        // CLI catalogue transparently for existing settings.json files.
        if (configured.rfind("gemini-3.8-", 0) == 0 || configured.rfind("gemini-3.7-", 0) == 0 ||
            configured.rfind("gemini-3.6-", 0) == 0 || configured.rfind("gemini-3.1-pro-", 0) == 0 ||
            configured.rfind("gpt-oss-", 0) == 0 || configured.rfind("claude-", 0) == 0)
            return configured;
        const std::string l = Lower(configured);
        if (l.find("pro") != std::string::npos)
            return "gemini-3.1-pro-high";
        if (l.find("lite") != std::string::npos)
            return "gemini-3.8-flash-low";
        return "gemini-3.8-flash-medium";
    }

    std::vector<std::string> AntigravityFallbackModels(const std::string& configured)
    {
        const std::string preferred = AntigravityModel(configured);
        std::vector<std::string> models = {preferred};
        // Antigravity keeps separate allowance pools for the Gemini, GPT-OSS
        // and Claude families. A 429 on one family must not take the whole
        // agent offline while another pool can still answer.
        for (const char* candidate : {"gpt-oss-120b-medium", "claude-sonnet-4-6"})
            if (preferred != candidate)
                models.emplace_back(candidate);
        return models;
    }

    // ---- Claude Code --------------------------------------------------------------------

    TurnResult RunClaude(const TurnRequest& req, const std::function<void(const std::string&)>& onChunk,
                         const std::atomic<bool>& cancel)
    {
        TurnResult r;
        const AgentInstall claude = FindClaude();
        if (!claude.found)
        {
            r.error = "Claude Code est introuvable.";
            r.quota = true;
            r.reason = "non installé";
            return r;
        }
        std::vector<std::wstring> args = {claude.exe, L"-p", L"--output-format", L"stream-json", L"--verbose",
                                          L"--include-partial-messages", L"--model", Platform::Widen(req.model),
                                          L"--tools", L"", L"--strict-mcp-config", L"--setting-sources", L"",
                                          L"--no-session-persistence", L"--disable-slash-commands",
                                          L"--system-prompt", Platform::Widen(req.systemPrompt)};
        if (!req.thinking.empty() && req.thinking != "auto")
        {
            args.push_back(L"--effort");
            args.push_back(Platform::Widen(req.thinking));
        }

        std::string text, resultError;
        bool rejected = false;
        long long resetsAt = 0;
        const Process::Result p = Process::Run(args, req.workDir, req.prompt, [&](const std::string& line) {
            json ev;
            try
            {
                ev = json::parse(line);
            }
            catch (...)
            {
                return;
            }
            const std::string type = ev.value("type", "");
            if (type == "stream_event")
            {
                const json& e = ev["event"];
                if (e.value("type", "") == "content_block_delta" && e["delta"].value("type", "") == "text_delta")
                {
                    const std::string t = e["delta"].value("text", "");
                    text += t;
                    onChunk(t);
                }
            }
            else if (type == "rate_limit_event")
            {
                const json& info = ev["rate_limit_info"];
                if (info.value("status", "") == "rejected")
                {
                    rejected = true;
                    resetsAt = info.value("resetsAt", 0LL);
                }
            }
            else if (type == "result" && ev.value("is_error", false))
            {
                resultError = ev.value("result", std::string("erreur inconnue"));
            }
        }, cancel, {}, 300);

        if (!p.started)
        {
            r.error = p.error;
            return r;
        }
        if (p.cancelled)
        {
            r.error = "arrêté";
            return r;
        }
        if (rejected)
        {
            r.quota = true;
            r.reason = "limite du forfait";
            if (resetsAt > 0)
                r.untilIso = IsoFromEpoch(resetsAt);
            r.error = resultError.empty() ? "Limite d'usage du forfait Claude atteinte." : resultError;
            return r;
        }
        if (!resultError.empty() || (p.exitCode != 0 && text.empty()))
        {
            ClassifyError(r, resultError.empty() ? Shorten(p.stderrText) : resultError);
            return r;
        }
        r.ok = true;
        r.text = text;
        return r;
    }

    // ---- Codex (ChatGPT) ----------------------------------------------------------------

    TurnResult RunCodex(const TurnRequest& req, const std::function<void(const std::string&)>& onChunk,
                        const std::atomic<bool>& cancel)
    {
        TurnResult r;
        const AgentInstall codex = FindCodex();
        if (!codex.found)
        {
            r.error = "Codex est introuvable.";
            r.quota = true;
            r.reason = "non installé";
            return r;
        }
        // Read-only sandbox in an empty folder, user config ignored (no MCP servers,
        // no skills): Codex has no switch to remove its shell tool entirely.
        std::vector<std::wstring> args = {codex.exe, L"exec", L"--json", L"-m", Platform::Widen(req.model),
                                          L"--sandbox", L"read-only", L"--skip-git-repo-check", L"--ephemeral",
                                          L"--ignore-rules", L"--ignore-user-config"};
        if (!req.thinking.empty() && req.thinking != "auto")
        {
            args.push_back(L"-c");
            args.push_back(Platform::Widen("model_reasoning_effort=" + req.thinking));
        }
        args.push_back(L"-");

        const std::string input = "<instructions>\n" + req.systemPrompt + "\n</instructions>\n\n" + req.prompt;
        std::string text, error;
        const Process::Result p = Process::Run(args, req.workDir, input, [&](const std::string& line) {
            json ev;
            try
            {
                ev = json::parse(line);
            }
            catch (...)
            {
                return;
            }
            const std::string type = ev.value("type", "");
            if (type == "item.completed")
            {
                const json& item = ev["item"];
                const std::string itemType = item.value("type", "");
                if (itemType == "agent_message")
                {
                    const std::string t = item.value("text", "");
                    if (!text.empty())
                        text += "\n\n";
                    text += t;
                    onChunk(t);
                }
                else if (itemType == "error")
                {
                    const std::string m = item.value("message", "");
                    // Config warnings are not failures.
                    if (Lower(m).find("ignoring") == std::string::npos && Lower(m).find("skill descriptions") == std::string::npos)
                        error = m;
                }
            }
            else if (type == "turn.failed" || type == "error")
            {
                error = ev.contains("error") && ev["error"].is_object() ? ev["error"].value("message", line)
                                                                         : ev.value("message", line);
            }
        }, cancel, kCodexEnv, 300);

        if (!p.started)
        {
            r.error = p.error;
            return r;
        }
        if (p.cancelled)
        {
            r.error = "arrêté";
            return r;
        }
        if (!error.empty() || (p.exitCode != 0 && text.empty()))
        {
            ClassifyError(r, error.empty() ? Shorten(p.stderrText) : error);
            return r;
        }
        r.ok = true;
        r.text = text;
        return r;
    }

    // ---- Gemini CLI ---------------------------------------------------------------------

    TurnResult RunGeminiCli(const TurnRequest& req, const std::function<void(const std::string&)>& onChunk,
                            const std::atomic<bool>& cancel)
    {
        TurnResult r;
        const AgentInstall gemini = FindGemini();
        if (!gemini.found)
        {
            r.error = "Gemini CLI est introuvable.";
            r.quota = true;
            r.reason = "non installé";
            return r;
        }
        if (IsAntigravity(gemini))
        {
            const std::string input = "<instructions>\n" + req.systemPrompt +
                                      "\nTu es dans un salon de discussion : ne modifie aucun fichier et "
                                      "n'exécute aucune commande.\n</instructions>\n\n" + req.prompt;
            std::string failures;
            for (const std::string& model : AntigravityFallbackModels(req.model))
            {
                std::vector<std::wstring> args = {gemini.exe, L"-p", Platform::Widen(input), L"--output-format", L"json",
                                                   L"--disable-slash-commands", L"--model", Platform::Widen(model),
                                                   L"--print-timeout", L"60s"};
                std::string output;
                const Process::Result p = Process::Run(args, req.workDir, "", [&](const std::string& line) {
                    output += line;
                    output += '\n';
                }, cancel, {}, 75);
                if (!p.started) { r.error = p.error; return r; }
                if (p.cancelled) { r.error = "arrêté"; return r; }
                if (p.timedOut)
                {
                    r.error = "Antigravity n'a rendu aucune réponse en 75 secondes (processus arrêté).";
                    r.reason = "délai dépassé avec le modèle " + model;
                    return r; // do not silently wait another 75 s on every fallback model
                }
                try
                {
                    const json result = json::parse(output);
                    const std::string response = result.value("response", "");
                    if (result.value("status", "") == "SUCCESS" && !response.empty())
                    {
                        r.ok = true;
                        r.text = response;
                        r.reason = "modèle Antigravity : " + model;
                        onChunk(response);
                        return r;
                    }
                    const std::string error = result.value("error", std::string("aucune réponse"));
                    failures += (failures.empty() ? "" : " | ") + model + " : " + Shorten(error, 180);
                }
                catch (...)
                {
                    failures += (failures.empty() ? "" : " | ") + model + " : " +
                                (p.exitCode == 0 ? "réponse JSON invalide" : Shorten(p.stderrText, 180));
                }
            }
            ClassifyError(r, failures.empty() ? "Tous les modèles Antigravity ont échoué." : failures);
            return r;
        }
        // Deny every tool: in chat mode Gemini only talks.
        const fs::path policy = fs::path(req.workDir) / L"deny-all-tools.toml";
        if (!fs::exists(policy))
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, policy.c_str(), L"wb") == 0 && f)
            {
                const char* toml = "[[rule]]\ntoolName = \"*\"\ndecision = \"deny\"\npriority = 999\n";
                fwrite(toml, 1, strlen(toml), f);
                fclose(f);
            }
        }
        std::vector<std::wstring> args = {gemini.exe, gemini.script, L"-p", L"Réponds au dernier message ci-dessus.",
                                          L"-m", Platform::Widen(req.model), L"-o", L"stream-json",
                                          L"--policy", policy.wstring(), L"-e", L"none", L"--skip-trust"};
        const std::string input = "<instructions>\n" + req.systemPrompt + "\n</instructions>\n\n" + req.prompt;

        std::string text, error;
        const Process::Result p = Process::Run(args, req.workDir, input, [&](const std::string& line) {
            json ev;
            try
            {
                ev = json::parse(line);
            }
            catch (...)
            {
                return;
            }
            const std::string type = ev.value("type", "");
            if (type == "message" && ev.value("role", "") == "assistant")
            {
                const std::string t = ev.value("content", "");
                text += t;
                onChunk(t);
            }
            else if (type == "error")
            {
                error = ev.value("message", line);
            }
            else if (type == "result" && ev.value("status", "") == "error")
            {
                error = ev.contains("error") && ev["error"].is_object() ? ev["error"].value("message", line) : line;
            }
        }, cancel, kGeminiEnv, 300);

        if (!p.started)
        {
            r.error = p.error;
            return r;
        }
        if (p.cancelled)
        {
            r.error = "arrêté";
            return r;
        }
        if (!error.empty() || (p.exitCode != 0 && text.empty()))
        {
            ClassifyError(r, error.empty() ? Shorten(p.stderrText) : error);
            return r;
        }
        r.ok = true;
        r.text = text;
        return r;
    }

    // ---- HTTP APIs -------------------------------------------------------------------------

    std::string EndpointFor(const std::string& provider)
    {
        if (provider == Provider::OpenAI) return "https://api.openai.com/v1/chat/completions";
        if (provider == Provider::Mistral) return "https://api.mistral.ai/v1/chat/completions";
        if (provider == Provider::OpenRouter) return "https://openrouter.ai/api/v1/chat/completions";
        if (provider == Provider::DeepSeek) return "https://api.deepseek.com/chat/completions";
        if (provider == Provider::XAI) return "https://api.x.ai/v1/chat/completions";
        return {};
    }

    void ApplyHttpFailure(TurnResult& r, const Http::Response& resp)
    {
        if (!resp.error.empty() && resp.status == 0)
        {
            r.error = resp.error; // network: transient, never offline
            return;
        }
        std::string message = resp.body;
        try
        {
            const json body = json::parse(resp.body);
            if (body.contains("error"))
                message = body["error"].is_object() ? body["error"].value("message", resp.body) : body["error"].dump();
        }
        catch (...)
        {
        }
        message = "HTTP " + std::to_string(resp.status) + " : " + Shorten(message, 400);
        if (resp.status == 429)
        {
            r.quota = true;
            r.reason = "limite atteinte";
            const auto it = resp.headers.find("retry-after");
            if (it != resp.headers.end())
            {
                try
                {
                    r.untilIso = IsoInSeconds(std::stoll(it->second));
                }
                catch (...)
                {
                }
            }
            if (Lower(message).find("insufficient_quota") != std::string::npos)
                r.reason = "crédits épuisés";
            r.error = message;
        }
        else if (resp.status == 401 || resp.status == 403)
        {
            r.quota = true;
            r.reason = "clé API refusée";
            r.error = message;
        }
        else if (resp.status == 402)
        {
            r.quota = true;
            r.reason = "crédits épuisés";
            r.error = message;
        }
        else
        {
            ClassifyError(r, message);
            if (resp.status >= 500)
                r.quota = false; // server trouble is transient
        }
    }

    TurnResult RunOpenAICompat(const TurnRequest& req, const std::function<void(const std::string&)>& onChunk,
                               const std::atomic<bool>& cancel)
    {
        TurnResult r;
        const std::string& key = req.apiKey;
        if (key.empty())
        {
            r.quota = true;
            r.reason = "pas de clé API";
            r.error = "Aucune clé API pour " + req.provider + " (Options → Connexions).";
            return r;
        }
        json body = {{"model", req.model},
                     {"stream", true},
                     {"messages", json::array({{{"role", "system"}, {"content", req.systemPrompt}},
                                               {{"role", "user"}, {"content", req.prompt}}})}};
        if (!req.thinking.empty() && req.thinking != "auto" &&
            (req.provider == Provider::OpenAI || req.provider == Provider::XAI))
            body["reasoning_effort"] = req.thinking;

        std::string text;
        Http::SseParser sse([&](const std::string& data) {
            if (data == "[DONE]")
                return;
            try
            {
                const json ev = json::parse(data);
                if (ev.contains("choices") && !ev["choices"].empty())
                {
                    const json& delta = ev["choices"][0]["delta"];
                    if (delta.contains("content") && delta["content"].is_string())
                    {
                        const std::string t = delta["content"].get<std::string>();
                        text += t;
                        onChunk(t);
                    }
                }
            }
            catch (...)
            {
            }
        });
        const Http::Response resp = Http::Request("POST", EndpointFor(req.provider),
                                                  {{"Content-Type", "application/json"}, {"Authorization", "Bearer " + key}},
                                                  body.dump(), [&](const std::string& chunk) { sse.Feed(chunk); }, cancel);
        if (sse.Overflowed())
        {
            r.error = "Réponse en flux refusée : événement supérieur à 4 Mio.";
            return r;
        }
        if (cancel)
        {
            r.error = "arrêté";
            return r;
        }
        if (resp.status < 200 || resp.status >= 300)
        {
            ApplyHttpFailure(r, resp);
            return r;
        }
        r.ok = true;
        r.text = text;
        return r;
    }

    TurnResult RunGeminiApi(const TurnRequest& req, const std::function<void(const std::string&)>& onChunk,
                            const std::atomic<bool>& cancel)
    {
        TurnResult r;
        const std::string& key = req.apiKey;
        if (key.empty())
        {
            r.quota = true;
            r.reason = "pas de clé API";
            r.error = "Aucune clé AI Studio (Options → Connexions).";
            return r;
        }
        json body = {{"systemInstruction", {{"parts", json::array({{{"text", req.systemPrompt}}})}}},
                     {"contents", json::array({{{"role", "user"}, {"parts", json::array({{{"text", req.prompt}}})}}})}};
        if (!req.thinking.empty() && req.thinking != "auto")
            body["generationConfig"]["thinkingConfig"]["thinkingLevel"] = req.thinking;

        std::string text;
        Http::SseParser sse([&](const std::string& data) {
            try
            {
                const json ev = json::parse(data);
                for (const json& cand : ev.value("candidates", json::array()))
                    for (const json& part : cand["content"].value("parts", json::array()))
                        if (part.contains("text") && !part.value("thought", false))
                        {
                            const std::string t = part["text"].get<std::string>();
                            text += t;
                            onChunk(t);
                        }
            }
            catch (...)
            {
            }
        });
        const std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" + req.model +
                                ":streamGenerateContent?alt=sse";
        const Http::Response resp = Http::Request("POST", url,
                                                  {{"Content-Type", "application/json"}, {"x-goog-api-key", key}},
                                                  body.dump(), [&](const std::string& chunk) { sse.Feed(chunk); }, cancel);
        if (sse.Overflowed())
        {
            r.error = "Réponse en flux refusée : événement supérieur à 4 Mio.";
            return r;
        }
        if (cancel)
        {
            r.error = "arrêté";
            return r;
        }
        if (resp.status < 200 || resp.status >= 300)
        {
            ApplyHttpFailure(r, resp);
            return r;
        }
        r.ok = true;
        r.text = text;
        return r;
    }
}

namespace
{
    std::vector<std::string> SplitLines(const std::string& text)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos)
                nl = text.size();
            std::string line = text.substr(start, nl - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty())
                out.push_back(line);
            start = nl + 1;
        }
        return out;
    }

    std::string RegexEscape(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos)
                out += '\\';
            out += c;
        }
        return out;
    }

    std::string TomlQuote(const std::string& s)
    {
        std::string out = "'";
        for (char c : s)
            if (c != '\'' && c != '\n')
                out += c;
        return out + "'";
    }
}

TurnResult RunCodeWork(const std::string& aiId, const std::string& model, const std::string& thinking,
                       const std::string& instructions, const std::wstring& projectDir,
                       const std::string& allowedCommands,
                       const std::function<void(const std::string& line)>& onProgress,
                       const std::atomic<bool>& cancel)
{
    TurnResult r;
    std::error_code ec;
    if (projectDir.empty() || !fs::is_directory(projectDir, ec))
    {
        r.error = "Le dossier de code du sous-serveur n'existe pas.";
        return r;
    }
    const std::vector<std::string> allowed = SplitLines(allowedCommands);
    std::string text, error;
    Process::Result p;

    if (aiId == "claude")
    {
        const AgentInstall a = FindClaude();
        if (!a.found)
        {
            r.error = "Claude Code est introuvable.";
            return r;
        }
        std::vector<std::wstring> args = {a.exe, L"-p", L"--output-format", L"stream-json", L"--verbose",
                                          L"--include-partial-messages", L"--model", Platform::Widen(model),
                                          L"--permission-mode", L"acceptEdits", L"--no-session-persistence",
                                          L"--strict-mcp-config", L"--disallowedTools", L"WebFetch", L"WebSearch"};
        if (!thinking.empty() && thinking != "auto")
        {
            args.push_back(L"--effort");
            args.push_back(Platform::Widen(thinking));
        }
        if (!allowed.empty())
        {
            args.push_back(L"--allowedTools");
            // Exact commands only. Prefix rules can be escaped with shell
            // operators (for example "npm test && ...").
            for (const std::string& command : allowed)
                args.push_back(Platform::Widen("Bash(" + command + ")"));
        }
        bool rejected = false;
        long long resetsAt = 0;
        p = Process::Run(args, projectDir, instructions, [&](const std::string& line) {
            json ev;
            try { ev = json::parse(line); } catch (...) { return; }
            const std::string type = ev.value("type", "");
            if (type == "assistant")
            {
                for (const json& block : ev["message"].value("content", json::array()))
                {
                    if (block.value("type", "") == "tool_use")
                        onProgress("[" + block.value("name", std::string("outil")) + "] " +
                                   (block.contains("input") ? block["input"].dump().substr(0, 200) : std::string()));
                    else if (block.value("type", "") == "text")
                        text = block.value("text", text);
                }
            }
            else if (type == "rate_limit_event" && ev["rate_limit_info"].value("status", "") == "rejected")
            {
                rejected = true;
                resetsAt = ev["rate_limit_info"].value("resetsAt", 0LL);
            }
            else if (type == "result")
            {
                if (ev.value("is_error", false))
                    error = ev.value("result", std::string("erreur"));
                else
                    text = ev.value("result", text);
            }
        }, cancel, {}, 3600);
        if (rejected)
        {
            r.quota = true;
            r.reason = "limite du forfait";
            if (resetsAt > 0)
                r.untilIso = IsoFromEpoch(resetsAt);
            r.error = "Limite du forfait Claude atteinte.";
            return r;
        }
    }
    else if (aiId == "chatgpt")
    {
        const AgentInstall a = FindCodex();
        if (!a.found)
        {
            r.error = "Codex est introuvable.";
            return r;
        }
        std::vector<std::wstring> args = {a.exe, L"exec", L"--json", L"-m", Platform::Widen(model), L"--sandbox",
                                          L"workspace-write", L"-C", projectDir, L"--skip-git-repo-check"};
        if (!thinking.empty() && thinking != "auto")
        {
            args.push_back(L"-c");
            args.push_back(Platform::Widen("model_reasoning_effort=" + thinking));
        }
        args.push_back(L"-");
        p = Process::Run(args, projectDir, instructions, [&](const std::string& line) {
            json ev;
            try { ev = json::parse(line); } catch (...) { return; }
            const std::string type = ev.value("type", "");
            if (type == "item.completed")
            {
                const json& item = ev["item"];
                const std::string it = item.value("type", "");
                if (it == "agent_message")
                    text = item.value("text", text);
                else if (it == "command_execution")
                    onProgress("[commande] " + item.value("command", std::string()) + " → code " +
                               std::to_string(item.value("exit_code", 0)));
                else if (it == "file_change")
                    onProgress("[fichiers] " + (item.contains("changes") ? item["changes"].dump().substr(0, 200) : std::string()));
                else if (it == "error")
                {
                    const std::string m = item.value("message", "");
                    if (Lower(m).find("ignoring") == std::string::npos && Lower(m).find("skill descriptions") == std::string::npos)
                        onProgress("[avertissement] " + m.substr(0, 200));
                }
            }
            else if (type == "turn.failed" || type == "error")
                error = ev.contains("error") && ev["error"].is_object() ? ev["error"].value("message", line) : ev.value("message", line);
        }, cancel, kCodexEnv, 3600);
    }
    else if (aiId == "gemini")
    {
        const AgentInstall a = FindGemini();
        if (!a.found)
        {
            r.error = "Gemini CLI est introuvable.";
            return r;
        }
        if (IsAntigravity(a))
        {
            const std::string prompt = instructions +
                "\n\nTravaille uniquement dans le dossier de projet courant. "
                "Tu peux modifier les fichiers demandés, mais n'exécute aucune commande système.";
            std::vector<std::wstring> args = {a.exe, L"-p", Platform::Widen(prompt),
                                               L"--output-format", L"json", L"--disable-slash-commands",
                                               L"--mode", L"accept-edits", L"--sandbox", L"--model",
                                               Platform::Widen(AntigravityModel(model)),
                                               L"--print-timeout", L"3600s"};
            std::string output;
            p = Process::Run(args, projectDir, "", [&](const std::string& line) {
                output += line;
                output += '\n';
            }, cancel, {}, 3660);
            try
            {
                const json result = json::parse(output);
                const std::string status = result.value("status", "");
                text = result.value("response", std::string());
                if (status != "SUCCESS")
                    error = result.value("error", std::string("Antigravity n'a pas terminé le travail."));
                else if (!text.empty())
                    onProgress(text.substr(0, 300));
            }
            catch (...)
            {
                if (p.exitCode != 0)
                    error = Shorten(p.stderrText.empty() ? output : p.stderrText);
            }
        }
        else
        {
        // Shell denied except allowed prefixes; file edits approved by auto_edit.
        std::string toml = "[[rule]]\ntoolName = \"run_shell_command\"\ndecision = \"deny\"\npriority = 500\n";
        for (const std::string& command : allowed)
            toml += "\n[[rule]]\ntoolName = \"run_shell_command\"\nargsPattern = " +
                    TomlQuote("\"command\":\"" + RegexEscape(command) + "\"(?:,|})") +
                    "\ndecision = \"allow\"\npriority = 600\n";
        const fs::path policy = fs::temp_directory_path() /
                                Platform::Widen("agentchats-gemini-work-" + Platform::NewId() + ".toml");
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, policy.c_str(), L"wb") == 0 && f)
            {
                fwrite(toml.data(), 1, toml.size(), f);
                fclose(f);
            }
        }
        std::vector<std::wstring> args = {a.exe, a.script, L"-p", L"Exécute les instructions ci-dessus.", L"-m",
                                          Platform::Widen(model), L"-o", L"stream-json", L"--approval-mode", L"auto_edit",
                                          L"--policy", policy.wstring(), L"--skip-trust"};
        p = Process::Run(args, projectDir, instructions, [&](const std::string& line) {
            json ev;
            try { ev = json::parse(line); } catch (...) { return; }
            const std::string type = ev.value("type", "");
            if (type == "message" && ev.value("role", "") == "assistant")
                text += ev.value("content", "");
            else if (type == "tool_use")
                onProgress("[" + ev.value("tool_name", std::string("outil")) + "] " +
                           (ev.contains("parameters") ? ev["parameters"].dump().substr(0, 200) : std::string()));
            else if (type == "error")
                error = ev.value("message", line);
        }, cancel, kGeminiEnv, 3600);
        fs::remove(policy, ec);
        }
    }
    else
    {
        r.error = "Cette IA n'a pas d'agent de code.";
        return r;
    }

    if (!p.started)
    {
        r.error = p.error;
        return r;
    }
    if (p.cancelled)
    {
        r.error = "arrêté";
        return r;
    }
    if (!error.empty() || (p.exitCode != 0 && text.empty()))
    {
        ClassifyError(r, error.empty() ? Shorten(p.stderrText) : error);
        return r;
    }
    r.ok = true;
    r.text = text.empty() ? "(travail terminé, sans résumé)" : text;
    return r;
}

TurnResult RunTurn(const TurnRequest& request,
                   const std::function<void(const std::string& chunk)>& onChunk, const std::atomic<bool>& cancel)
{
    switch (request.kind)
    {
    case BackendKind::ClaudeCli: return RunClaude(request, onChunk, cancel);
    case BackendKind::CodexCli: return RunCodex(request, onChunk, cancel);
    case BackendKind::GeminiCli: return RunGeminiCli(request, onChunk, cancel);
    case BackendKind::OpenAICompat: return RunOpenAICompat(request, onChunk, cancel);
    case BackendKind::GeminiApi: return RunGeminiApi(request, onChunk, cancel);
    }
    return {};
}

// ---- Locating the agents ------------------------------------------------------------------

namespace
{
    std::wstring EnvPath(const wchar_t* name)
    {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
#else
        // The Windows profile variable means the home folder here.
        const std::string key = std::wstring(name) == L"USERPROFILE" ? "HOME" : Platform::Narrow(name);
        const char* value = std::getenv(key.c_str());
        return value ? Platform::Widen(value) : std::wstring();
#endif
    }
}

AgentInstall FindClaude()
{
    AgentInstall a;
    std::wstring exe = Process::FindOnPath(L"claude.exe");
    if (exe.empty())
    {
#ifdef _WIN32
        const fs::path local = fs::path(EnvPath(L"USERPROFILE")) / L".local" / L"bin" / L"claude.exe";
#else
        const fs::path local = fs::path(EnvPath(L"USERPROFILE")) / L".local" / L"bin" / L"claude";
#endif
        if (fs::exists(local))
            exe = local.wstring();
    }
    a.found = !exe.empty();
    a.exe = exe;
    a.description = a.found ? Platform::Narrow(exe) : "introuvable";
    return a;
}

AgentInstall FindCodex()
{
    AgentInstall a;
    std::wstring exe = Process::FindOnPath(L"codex.exe");
#ifdef _WIN32
    if (exe.empty())
    {
        // The Codex app keeps its CLI in versioned folders: take the most recent.
        const fs::path bin = fs::path(EnvPath(L"LOCALAPPDATA")) / L"OpenAI" / L"Codex" / L"bin";
        std::error_code ec;
        fs::file_time_type newest{};
        for (const auto& dir : fs::directory_iterator(bin, ec))
        {
            const fs::path candidate = dir.path() / L"codex.exe";
            if (fs::exists(candidate, ec))
            {
                const auto t = fs::last_write_time(candidate, ec);
                if (exe.empty() || t > newest)
                {
                    exe = candidate.wstring();
                    newest = t;
                }
            }
        }
    }
#endif
    a.found = !exe.empty();
    a.exe = exe;
    a.description = a.found ? Platform::Narrow(exe) : "introuvable";
    return a;
}

AgentInstall FindGemini()
{
    AgentInstall a;
    std::wstring agy = Process::FindOnPath(L"agy.exe");
    if (agy.empty())
    {
        const fs::path local = fs::path(EnvPath(L"LOCALAPPDATA")) / L"agy" / L"bin" / L"agy.exe";
        if (fs::exists(local))
            agy = local.wstring();
    }
    if (!agy.empty())
    {
        a.found = true;
        a.exe = agy;
        a.description = "Antigravity CLI : " + Platform::Narrow(agy);
        return a;
    }
    const std::wstring node = Process::FindOnPath(L"node.exe");
#ifdef _WIN32
    const fs::path script = fs::path(EnvPath(L"APPDATA")) / L"npm" / L"node_modules" / L"@google" / L"gemini-cli" /
                            L"bundle" / L"gemini.js";
#else
    // npm links a "gemini" launcher on PATH to the package's script: follow it.
    std::error_code linkEc;
    const std::wstring launcher = Process::FindOnPath(L"gemini");
    const fs::path script = launcher.empty() ? fs::path() : fs::canonical(fs::path(launcher), linkEc);
#endif
    a.found = !node.empty() && fs::exists(script);
    a.exe = node;
    a.script = script.wstring();
    a.description = a.found ? Platform::Narrow(script.wstring()) : "introuvable (npm install -g @google/gemini-cli)";
    return a;
}

LoginStatus CheckLogin(const std::string& aiId)
{
    LoginStatus s;
    std::atomic<bool> noCancel{false};
    std::string out;
    auto collect = [&](const std::string& line) { out += line + "\n"; };
    if (aiId == "claude")
    {
        const AgentInstall a = FindClaude();
        if (!a.found)
            return s;
        const Process::Result p = Process::Run({a.exe, L"auth", L"status", L"--json"}, L"", "", collect, noCancel);
        try
        {
            const json j = json::parse(out);
            s.known = true;
            s.loggedIn = j.value("loggedIn", false);
            s.plan = j.value("subscriptionType", std::string());
            s.planActive = s.loggedIn && j.value("authMethod", std::string()) == "claude.ai" && !s.plan.empty();
            // Claude Code exposes the authenticated subscription, but no
            // headless read-only usage counter. Runtime limit events still
            // move it to Exhausted; do not pretend the active plan is unknown.
            s.detail = s.planActive ? "forfait Claude " + s.plan + " actif · compteur non exposé par Claude CLI"
                                    : (s.loggedIn ? "compte Claude connecté sans forfait détecté" : "non connectée");
        }
        catch (...)
        {
            s.detail = Shorten(p.stderrText, 200);
        }
    }
    else if (aiId == "chatgpt")
    {
        const AgentInstall a = FindCodex();
        if (!a.found)
            return s;
        const std::string rpc =
            "{\"method\":\"initialize\",\"id\":0,\"params\":{\"clientInfo\":{\"name\":\"agents_chat\",\"title\":\"Agents Chat\",\"version\":\"0.1\"}}}\n"
            "{\"method\":\"initialized\",\"params\":{}}\n"
            "{\"method\":\"account/read\",\"id\":1,\"params\":{\"refreshToken\":false}}\n"
            "{\"method\":\"account/rateLimits/read\",\"id\":2}\n";
        json account, limits;
        const Process::Result appServer = Process::Run({a.exe, L"app-server", L"--stdio"}, L"", rpc,
            [&](const std::string& line) {
                try
                {
                    const json message = json::parse(line);
                    if (message.value("id", -1) == 1 && message.contains("result")) account = message["result"];
                    if (message.value("id", -1) == 2 && message.contains("result")) limits = message["result"];
                }
                catch (...) {}
            }, noCancel, kCodexEnv, 20);
        if (account.contains("account") && account["account"].is_object())
        {
            const json& acc = account["account"];
            s.known = true;
            s.loggedIn = true;
            s.plan = acc.value("planType", std::string());
            s.planActive = acc.value("type", std::string()) == "chatgpt" && !s.plan.empty();
            std::vector<const json*> rateBuckets;
            if (limits.contains("rateLimits") && limits["rateLimits"].is_object())
                rateBuckets.push_back(&limits["rateLimits"]);
            if (limits.contains("rateLimitsByLimitId") && limits["rateLimitsByLimitId"].is_object())
                for (const auto& [_, bucket] : limits["rateLimitsByLimitId"].items())
                    if (bucket.is_object()) rateBuckets.push_back(&bucket);
            if (!rateBuckets.empty())
            {
                s.quotaKnown = true;
                s.quotaAvailable = false;
                double remaining = 0.0;
                long long reset = 0;
                for (const json* rate : rateBuckets)
                {
                    bool bucketAvailable = rate->value("rateLimitReachedType", std::string()).empty();
                    double bucketRemaining = 100.0;
                    bool sawWindow = false;
                    for (const char* window : {"primary", "secondary"})
                        if (rate->contains(window) && (*rate)[window].is_object())
                        {
                            sawWindow = true;
                            const double used = (*rate)[window].value("usedPercent", 0.0);
                            bucketRemaining = std::min(bucketRemaining, std::max(0.0, 100.0 - used));
                            if (used >= 100.0)
                            {
                                bucketAvailable = false;
                                reset = std::max(reset, (*rate)[window].value("resetsAt", 0LL));
                            }
                        }
                    if (!sawWindow && rate->contains("usedPercent"))
                    {
                        const double used = rate->value("usedPercent", 0.0);
                        bucketRemaining = std::max(0.0, 100.0 - used);
                        bucketAvailable = bucketAvailable && used < 100.0;
                        if (!bucketAvailable) reset = std::max(reset, rate->value("resetsAt", 0LL));
                    }
                    s.quotaAvailable = s.quotaAvailable || bucketAvailable;
                    remaining = std::max(remaining, bucketRemaining);
                }
                s.remainingPercent = remaining;
                if (reset > 0) s.resetsAt = IsoFromEpoch(reset);
                const int rounded = static_cast<int>(remaining + 0.5);
                s.detail = s.quotaAvailable ? "forfait ChatGPT " + s.plan + " · jusqu'à " + std::to_string(rounded) + "% disponible"
                                            : "quota Codex épuisé";
            }
            else
                s.detail = "forfait ChatGPT " + s.plan + " · quota inconnu";
            return s;
        }

        out.clear();
        const Process::Result p = Process::Run({a.exe, L"login", L"status"}, L"", "", collect, noCancel, kCodexEnv);
        const std::string all = out + p.stderrText + appServer.stderrText;
        s.known = true;
        s.loggedIn = Lower(all).find("logged in") != std::string::npos && Lower(all).find("not logged") == std::string::npos;
        s.planActive = s.loggedIn;
        s.detail = s.loggedIn ? "compte ChatGPT connecté · forfait et quota inconnus" : "non connectée";
    }
    else if (aiId == "gemini")
    {
        const AgentInstall agent = FindGemini();
        if (IsAntigravity(agent))
        {
            EnsureAntigravityStatusHook();
            const Process::Result p = Process::Run({agent.exe, L"models"}, L"", "", collect, noCancel, {}, 60);
            s.known = true;
            s.loggedIn = p.started && p.exitCode == 0 && !out.empty();
            s.planActive = s.loggedIn;
            s.detail = s.loggedIn ? "forfait Antigravity détecté · quota inconnu" : "Antigravity CLI non connecté";
            if (s.loggedIn)
                ReadAntigravityQuota(s);
            return s;
        }
        // Legacy Gemini CLI has no status command: inspect its selected method.
        const fs::path settings = fs::path(EnvPath(L"USERPROFILE")) / L".gemini" / L"settings.json";
        s.known = true;
        try
        {
            FILE* f = nullptr;
            std::string content;
            if (_wfopen_s(&f, settings.c_str(), L"rb") == 0 && f)
            {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    content.append(buf, n);
                fclose(f);
            }
            const json j = json::parse(content);
            std::string type;
            if (j.contains("security") && j["security"].contains("auth"))
                type = j["security"]["auth"].value("selectedType", "");
            if (type.empty())
                type = j.value("selectedAuthType", "");
            // Google now rejects oauth-personal at runtime with
            // IneligibleTierError/UNSUPPORTED_CLIENT. Do not advertise it as
            // online merely because the obsolete setting remains on disk.
            if (type == "oauth-personal")
            {
                s.loggedIn = false;
                s.detail = "connexion personnelle refusée par Google ; utiliser une clé AI Studio";
            }
            else
            {
                s.loggedIn = !type.empty();
                s.planActive = false;
                s.detail = s.loggedIn ? "méthode : " + type : "non connectée";
            }
        }
        catch (...)
        {
            s.detail = "non connectée";
        }
    }
    return s;
}

bool OpenLoginConsole(const std::string& aiId, std::string& error)
{
    std::vector<std::wstring> args;
    if (aiId == "claude")
    {
        const AgentInstall a = FindClaude();
        if (a.found)
            args = {a.exe, L"auth", L"login"};
    }
    else if (aiId == "chatgpt")
    {
        const AgentInstall a = FindCodex();
        if (a.found)
            args = {a.exe, L"login"};
    }
    else if (aiId == "gemini")
    {
        const AgentInstall a = FindGemini();
        if (a.found)
            args = IsAntigravity(a) ? std::vector<std::wstring>{a.exe}
                                    : std::vector<std::wstring>{a.exe, a.script}; // first run asks how to sign in
    }
    if (args.empty())
    {
        error = "Agent introuvable sur cette machine.";
        return false;
    }
#ifndef _WIN32
    // Linux: the desktop's terminal, detached in its own session; one per agent at a time.
    static std::map<std::string, pid_t> openTerminals;
    if (const auto running = openTerminals.find(aiId); running != openTerminals.end())
    {
        if (waitpid(running->second, nullptr, WNOHANG) == 0)
        {
            error = "La console de connexion est déjà ouverte.";
            return false;
        }
        openTerminals.erase(running);
    }
    std::vector<std::string> terminal;
    if (const char* preferred = std::getenv("TERMINAL"); preferred && *preferred && !Process::FindOnPath(Platform::Widen(preferred)).empty())
        terminal = {preferred, "-e"};
    else if (!Process::FindOnPath(L"x-terminal-emulator").empty())
        terminal = {"x-terminal-emulator", "-e"};
    else if (!Process::FindOnPath(L"gnome-terminal").empty())
        terminal = {"gnome-terminal", "--"};
    else if (!Process::FindOnPath(L"konsole").empty())
        terminal = {"konsole", "-e"};
    else if (!Process::FindOnPath(L"xterm").empty())
        terminal = {"xterm", "-e"};
    if (terminal.empty())
    {
        error = "Aucun terminal trouvé : lance la connexion toi-même dans un terminal.";
        return false;
    }
    for (const std::wstring& arg : args)
        terminal.push_back(Platform::Narrow(arg));
    std::vector<char*> argv;
    for (std::string& a : terminal)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0)
    {
        error = "Lancement impossible.";
        return false;
    }
    if (pid == 0)
    {
        setsid();
        if (aiId == "gemini")
            setenv("GEMINI_CLI_NO_RELAUNCH", "true", 1);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    openTerminals[aiId] = pid;
    return true;
#else
    std::wstring cmdline;
    for (const std::wstring& arg : args)
        cmdline += (cmdline.empty() ? L"" : L" ") + Process::QuoteArg(arg);

    // One console per agent at a time.
    static std::map<std::string, HANDLE> open;
    const auto it = open.find(aiId);
    if (it != open.end())
    {
        if (WaitForSingleObject(it->second, 0) == WAIT_TIMEOUT)
        {
            error = "La console de connexion est déjà ouverte.";
            return false;
        }
        CloseHandle(it->second);
        open.erase(it);
    }
    // Own console, own process group, out of the app's job: it is not the app's memory.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring env;
    if (aiId == "gemini")
    {
        wchar_t* block = GetEnvironmentStringsW();
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
            (env += p).push_back(L'\0');
        FreeEnvironmentStringsW(block);
        (env += L"GEMINI_CLI_NO_RELAUNCH=true").push_back(L'\0');
        env.push_back(L'\0');
    }
    if (!CreateProcessW(args[0].c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB | CREATE_UNICODE_ENVIRONMENT,
                        env.empty() ? nullptr : env.data(), nullptr, &si, &pi))
    {
        error = "Lancement impossible (erreur " + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(pi.hThread);
    open[aiId] = pi.hProcess;
    return true;
#endif
}
