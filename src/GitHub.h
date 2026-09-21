#pragma once
#include <string>
#include <vector>

// GitHub issues through the official, already authenticated `gh` CLI. The app
// never stores a GitHub token: `gh` keeps it in the system keyring.
// Every call is blocking: run it off the UI thread.
namespace GitHub
{
    struct Issue
    {
        int number = 0;
        std::string title;
        std::string state; // "OPEN" or "CLOSED"
        std::string url;
        std::string labels; // comma-separated
    };

    struct Result
    {
        bool ok = false;
        std::string error;
    };

    // "https://github.com/owner/repo(.git)" -> "owner/repo" ("" if not a GitHub URL).
    std::string RepoSlug(const std::string& url);

    // Whether `gh` is installed and signed in (detail explains otherwise).
    bool Ready(std::string& detail);

    Result ListIssues(const std::string& slug, std::vector<Issue>& out); // open + recently closed
    Result CreateIssue(const std::string& slug, const std::string& title, const std::string& body, Issue& created);
    Result Comment(const std::string& slug, int number, const std::string& body);
    Result Close(const std::string& slug, int number, const std::string& comment);
}
