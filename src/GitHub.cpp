#include "GitHub.h"
#include "Platform.h"
#include "Process.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>

using nlohmann::json;
namespace fs = std::filesystem;

namespace GitHub
{
    namespace
    {
        std::wstring GhPath()
        {
            std::wstring gh = Process::FindOnPath(L"gh.exe");
            if (gh.empty())
            {
                const fs::path installed = L"C:\\Program Files\\GitHub CLI\\gh.exe";
                std::error_code ec;
                if (fs::exists(installed, ec))
                    gh = installed.wstring();
            }
            return gh;
        }

        // Runs gh with arguments; stdout collected. Body texts go through stdin
        // ("--body-file -") so nothing depends on command-line quoting.
        Result RunGh(const std::vector<std::wstring>& args, const std::string& input, std::string& out)
        {
            Result r;
            const std::wstring gh = GhPath();
            if (gh.empty())
            {
                r.error = "GitHub CLI (gh) est introuvable.";
                return r;
            }
            std::vector<std::wstring> full = {gh};
            full.insert(full.end(), args.begin(), args.end());
            std::atomic<bool> noCancel{false};
            const Process::Result p = Process::Run(full, L"", input, [&](const std::string& line) { out += line + "\n"; },
                                                   noCancel, {L"GH_PROMPT_DISABLED=1", L"NO_COLOR=1"}, 60);
            if (!p.started)
                r.error = p.error;
            else if (p.timedOut)
                r.error = "gh ne répond pas (délai dépassé).";
            else if (p.exitCode != 0)
                r.error = p.stderrText.empty() ? "gh a échoué (code " + std::to_string(p.exitCode) + ")." : p.stderrText.substr(0, 400);
            else
                r.ok = true;
            return r;
        }

        int NumberFromUrl(const std::string& url)
        {
            const size_t slash = url.find_last_of('/');
            if (slash == std::string::npos)
                return 0;
            try
            {
                return std::stoi(url.substr(slash + 1));
            }
            catch (...)
            {
                return 0;
            }
        }
    }

    std::string RepoSlug(const std::string& url)
    {
        std::string s = url;
        for (const char* prefix : {"https://github.com/", "http://github.com/", "git@github.com:", "ssh://git@github.com/"})
            if (s.rfind(prefix, 0) == 0)
            {
                s = s.substr(std::string(prefix).size());
                while (!s.empty() && (s.back() == '/' || s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                    s.pop_back();
                if (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0)
                    s.resize(s.size() - 4);
                // owner/repo only, no extra path.
                const size_t first = s.find('/');
                if (first == std::string::npos || first == 0 || first + 1 >= s.size())
                    return {};
                const size_t second = s.find('/', first + 1);
                if (second != std::string::npos)
                    s.resize(second);
                for (char c : s)
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/'))
                        return {};
                return s;
            }
        return {};
    }

    bool Ready(std::string& detail)
    {
        std::string out;
        const Result r = RunGh({L"auth", L"status", L"--hostname", L"github.com"}, "", out);
        detail = r.ok ? "gh connecté" : (r.error.empty() ? "gh non connecté" : r.error);
        return r.ok;
    }

    Result ListIssues(const std::string& slug, std::vector<Issue>& out)
    {
        std::string text;
        Result r = RunGh({L"issue", L"list", L"--repo", Platform::Widen(slug), L"--state", L"all", L"--limit", L"100",
                          L"--json", L"number,title,state,url,labels"},
                         "", text);
        if (!r.ok)
            return r;
        try
        {
            for (const json& i : json::parse(text))
            {
                Issue issue;
                issue.number = i.value("number", 0);
                issue.title = i.value("title", "");
                issue.state = i.value("state", "");
                issue.url = i.value("url", "");
                for (const json& label : i.value("labels", json::array()))
                    issue.labels += (issue.labels.empty() ? "" : ", ") + label.value("name", std::string());
                if (issue.number > 0)
                    out.push_back(std::move(issue));
            }
        }
        catch (const std::exception& e)
        {
            r.ok = false;
            r.error = std::string("Réponse de gh illisible : ") + e.what();
        }
        return r;
    }

    Result CreateIssue(const std::string& slug, const std::string& title, const std::string& body, Issue& created)
    {
        std::string text;
        Result r = RunGh({L"issue", L"create", L"--repo", Platform::Widen(slug), L"--title", Platform::Widen(title),
                          L"--body-file", L"-"},
                         body, text);
        if (!r.ok)
            return r;
        // gh prints the new issue's URL.
        std::string url;
        for (size_t start = 0; start < text.size();)
        {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos)
                nl = text.size();
            const std::string line = text.substr(start, nl - start);
            if (line.rfind("https://github.com/", 0) == 0)
                url = line;
            start = nl + 1;
        }
        created.url = url;
        created.number = NumberFromUrl(url);
        created.title = title;
        created.state = "OPEN";
        if (created.number == 0)
        {
            r.ok = false;
            r.error = "Issue créée mais numéro introuvable dans la réponse de gh.";
        }
        return r;
    }

    Result Comment(const std::string& slug, int number, const std::string& body)
    {
        std::string text;
        return RunGh({L"issue", L"comment", std::to_wstring(number), L"--repo", Platform::Widen(slug), L"--body-file", L"-"},
                     body, text);
    }

    Result Close(const std::string& slug, int number, const std::string& comment)
    {
        std::string text;
        if (!comment.empty())
        {
            Result c = Comment(slug, number, comment);
            if (!c.ok)
                return c;
        }
        return RunGh({L"issue", L"close", std::to_wstring(number), L"--repo", Platform::Widen(slug)}, "", text);
    }

    std::string ContributionBranch(const std::string& currentBranch, const std::string& stamp)
    {
        if (currentBranch.empty() || currentBranch == "HEAD" || currentBranch == "main" || currentBranch == "master")
            return "amelioration/" + stamp;
        return currentBranch;
    }

    namespace
    {
        Result RunIn(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& dir,
                     const std::string& input, std::string& out, unsigned timeoutSeconds)
        {
            Result r;
            std::vector<std::wstring> full = {exe};
            full.insert(full.end(), args.begin(), args.end());
            std::atomic<bool> noCancel{false};
            const Process::Result p = Process::Run(full, dir, input, [&](const std::string& line) { out += line + "\n"; },
                                                   noCancel, {L"GH_PROMPT_DISABLED=1", L"GIT_TERMINAL_PROMPT=0", L"NO_COLOR=1"},
                                                   timeoutSeconds);
            if (!p.started)
                r.error = p.error;
            else if (p.timedOut)
                r.error = "délai dépassé.";
            else if (p.exitCode != 0)
                r.error = (p.stderrText.empty() ? out : p.stderrText).substr(0, 600);
            else
                r.ok = true;
            return r;
        }

        std::string FirstLine(const std::string& text)
        {
            const size_t nl = text.find_first_of("\r\n");
            return nl == std::string::npos ? text : text.substr(0, nl);
        }
    }

    Result ProposePullRequest(const std::string& repoDir, const std::string& title, const std::string& body,
                              std::string& prUrl)
    {
        Result r;
        const std::wstring git = Process::FindOnPath(L"git.exe");
        const std::wstring gh = GhPath();
        if (git.empty() || gh.empty())
        {
            r.error = git.empty() ? "Git est introuvable." : "GitHub CLI (gh) est introuvable.";
            return r;
        }
        const std::wstring dir = Platform::Widen(repoDir);
        std::string out;
        if (!(r = RunIn(git, {L"status", L"--porcelain"}, dir, "", out, 60)).ok)
            return r;
        const bool dirty = !out.empty();

        out.clear();
        if (!(r = RunIn(git, {L"rev-parse", L"--abbrev-ref", L"HEAD"}, dir, "", out, 30)).ok)
            return r;
        const std::string current = FirstLine(out);
        std::string stamp = Platform::NowIsoUtc(); // 2026-09-21T10:11:12Z -> 20260921-101112
        stamp.erase(std::remove_if(stamp.begin(), stamp.end(), [](char c) { return c == '-' || c == ':' || c == 'Z'; }), stamp.end());
        std::replace(stamp.begin(), stamp.end(), 'T', '-');
        const std::string branch = ContributionBranch(current, stamp);
        if (!dirty && branch != current)
        {
            r.ok = false;
            r.error = "Aucune modification locale à proposer.";
            return r;
        }

        out.clear();
        if (!(r = RunIn(git, {L"remote"}, dir, "", out, 30)).ok)
            return r;
        const std::wstring remote = out.find("origin") != std::string::npos ? L"origin" : L"upstream";

        if (branch != current)
        {
            out.clear();
            if (!(r = RunIn(git, {L"switch", L"-c", Platform::Widen(branch)}, dir, "", out, 60)).ok)
                return r;
        }
        if (dirty)
        {
            out.clear();
            if (!(r = RunIn(git, {L"add", L"-A"}, dir, "", out, 120)).ok)
                return r;
            out.clear();
            if (!(r = RunIn(git, {L"commit", L"-F", L"-"}, dir, title + "\n\n" + body + "\n", out, 120)).ok)
                return r;
        }
        out.clear();
        if (!(r = RunIn(git, {L"push", L"-u", remote, Platform::Widen(branch)}, dir, "", out, 300)).ok)
            return r;

        // An existing PR for this branch is updated by the push; otherwise open one.
        out.clear();
        if (RunIn(gh, {L"pr", L"view", Platform::Widen(branch), L"--json", L"url", L"--jq", L".url"}, dir, "", out, 60).ok &&
            FirstLine(out).rfind("https://", 0) == 0)
        {
            prUrl = FirstLine(out);
            return r;
        }
        out.clear();
        if (!(r = RunIn(gh, {L"pr", L"create", L"--head", Platform::Widen(branch), L"--title", Platform::Widen(title),
                             L"--body-file", L"-"},
                        dir, body, out, 120))
                 .ok)
            return r;
        for (size_t start = 0; start < out.size();)
        {
            size_t nl = out.find('\n', start);
            if (nl == std::string::npos)
                nl = out.size();
            const std::string line = out.substr(start, nl - start);
            if (line.rfind("https://github.com/", 0) == 0)
                prUrl = line;
            start = nl + 1;
        }
        return r;
    }
}
