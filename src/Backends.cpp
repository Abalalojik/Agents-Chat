#include "Backends.h"
#include "Http.h"
#include "Platform.h"
#include "Process.h"
#include "Settings.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <map>

using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
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
        wchar_t profile[32768];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, static_cast<DWORD>(std::size(profile)));
        return (n > 0 && n < std::size(profile))
                   ? std::vector<std::wstring>{L"HOME=" + std::wstring(profile)}
                   : std::vector<std::wstring>{};
    }

    const std::vector<std::wstring> kCodexEnv = CodexEnvironment();

    bool IsAntigravity(const AgentInstall& agent)
    {
        return agent.found && agent.script.empty() &&
               Lower(Platform::Narrow(fs::path(agent.exe).filename().wstring())) == "agy.exe";
    }

    std::string AntigravityModel(const std::string& configured)
    {
        // Preserve explicit Antigravity model ids. Migrate the retired Gemini
        // CLI catalogue transparently for existing settings.json files.
        if (configured.rfind("gemini-3.8-", 0) == 0 || configured.rfind("gemini-3.7-", 0) == 0 ||
            configured.rfind("gemini-3.6-", 0) == 0 || configured.rfind("gemini-3.1-pro-", 0) == 0)
            return configured;
        const std::string l = Lower(configured);
        if (l.find("pro") != std::string::npos)
            return "gemini-3.1-pro-high";
        if (l.find("lite") != std::string::npos)
            return "gemini-3.8-flash-low";
        return "gemini-3.8-flash-medium";
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
            std::vector<std::wstring> args = {gemini.exe, L"-p", Platform::Widen(input), L"--output-format", L"json",
                                               L"--disable-slash-commands", L"--model",
                                               Platform::Widen(AntigravityModel(req.model)), L"--print-timeout", L"300s"};
            std::string output;
            const Process::Result p = Process::Run(args, req.workDir, "", [&](const std::string& line) {
                output += line;
                output += '\n';
            }, cancel, {}, 330);
            if (!p.started) { r.error = p.error; return r; }
            if (p.cancelled) { r.error = "arrêté"; return r; }
            try
            {
                const json result = json::parse(output);
                const std::string status = result.value("status", "");
                const std::string response = result.value("response", "");
                if (status != "SUCCESS" || response.empty())
                {
                    ClassifyError(r, result.value("error", std::string("Antigravity n'a renvoyé aucune réponse.")));
                    return r;
                }
                r.ok = true;
                r.text = response;
                onChunk(response);
                return r;
            }
            catch (...)
            {
                ClassifyError(r, p.exitCode == 0 ? "Réponse JSON Antigravity invalide." : Shorten(p.stderrText));
                return r;
            }
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
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
        return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
    }
}

AgentInstall FindClaude()
{
    AgentInstall a;
    std::wstring exe = Process::FindOnPath(L"claude.exe");
    if (exe.empty())
    {
        const fs::path local = fs::path(EnvPath(L"USERPROFILE")) / L".local" / L"bin" / L"claude.exe";
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
    const fs::path script = fs::path(EnvPath(L"APPDATA")) / L"npm" / L"node_modules" / L"@google" / L"gemini-cli" /
                            L"bundle" / L"gemini.js";
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
            s.detail = s.loggedIn ? "connectée (" + j.value("authMethod", std::string("?")) + ")" : "non connectée";
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
        const Process::Result p = Process::Run({a.exe, L"login", L"status"}, L"", "", collect, noCancel, kCodexEnv);
        const std::string all = out + p.stderrText;
        s.known = true;
        s.loggedIn = Lower(all).find("logged in") != std::string::npos && Lower(all).find("not logged") == std::string::npos;
        s.detail = s.loggedIn ? "connectée (ChatGPT)" : "non connectée";
    }
    else if (aiId == "gemini")
    {
        const AgentInstall agent = FindGemini();
        if (IsAntigravity(agent))
        {
            const Process::Result p = Process::Run({agent.exe, L"models"}, L"", "", collect, noCancel, {}, 60);
            s.known = true;
            s.loggedIn = p.started && p.exitCode == 0 && !out.empty();
            s.detail = s.loggedIn ? "connectée (Antigravity CLI)" : "Antigravity CLI non connecté";
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
}
