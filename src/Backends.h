#pragma once
#include <atomic>
#include <functional>
#include <string>

class Settings;

// How one AI turn is produced.
enum class BackendKind
{
    ClaudeCli,    // `claude -p`, Claude Pro plan
    CodexCli,     // `codex exec`, ChatGPT plan
    GeminiCli,    // Gemini CLI, Google AI Pro plan
    OpenAICompat, // OpenAI-format HTTP API (OpenAI, Mistral, OpenRouter, DeepSeek, xAI)
    GeminiApi,    // Google AI Studio API
};

// API providers whose keys live in Settings.
namespace Provider
{
    inline constexpr const char* OpenAI = "openai";
    inline constexpr const char* Mistral = "mistral";
    inline constexpr const char* OpenRouter = "openrouter";
    inline constexpr const char* DeepSeek = "deepseek";
    inline constexpr const char* XAI = "xai";
    inline constexpr const char* GeminiApi = "gemini_api";
}

struct TurnRequest
{
    BackendKind kind = BackendKind::ClaudeCli;
    std::string provider;     // for API kinds
    std::string apiKey;       // resolved on the UI thread (Settings is not thread-safe)
    std::string model;
    std::string thinking;     // provider vocabulary, "auto" = not passed
    std::string systemPrompt;
    std::string prompt;       // conversation + instruction for this turn
    std::wstring workDir;     // neutral folder for CLI chat mode
};

struct TurnResult
{
    bool ok = false;
    std::string text;
    // Quota-type failure: the AI goes offline (until untilIso, or until the user
    // puts it back when empty).
    bool quota = false;
    std::string untilIso;
    std::string reason; // short French reason for the status line
    std::string error;  // full error for the salon
};

// Runs one chat turn. Tools of the CLIs are all disabled: in chat mode the AIs
// only talk; the app executes their requests itself (with approvals).
TurnResult RunTurn(const TurnRequest& request,
                   const std::function<void(const std::string& chunk)>& onChunk,
                   const std::atomic<bool>& cancel);

// Runs a piece of code work with the AI's code twin, inside the project folder.
// Physical barriers: Codex runs in its workspace-write sandbox (writes confined
// to the project); Claude Code and Gemini CLI may edit files in the project but
// every shell command is denied unless it starts with an allowed prefix.
TurnResult RunCodeWork(const std::string& aiId, const std::string& model, const std::string& thinking,
                       const std::string& instructions, const std::wstring& projectDir,
                       const std::string& allowedCommands,
                       const std::function<void(const std::string& line)>& onProgress,
                       const std::atomic<bool>& cancel);

// ---- Locating the agents and signing in -------------------------------------------------

struct AgentInstall
{
    bool found = false;
    std::wstring exe;        // claude.exe / codex.exe / node.exe
    std::wstring script;     // gemini.js when exe is node
    std::string description; // where it was found, for the Options window
};

AgentInstall FindClaude();
AgentInstall FindCodex();
AgentInstall FindGemini();

// Signed-in check, through each CLI's own status command (never reads credentials).
struct LoginStatus
{
    bool known = false;
    bool loggedIn = false;
    bool planActive = false;
    bool quotaKnown = false;
    bool quotaAvailable = false;
    double remainingPercent = -1.0;
    std::string plan;
    std::string resetsAt;
    std::string detail;
};
LoginStatus CheckLogin(const std::string& aiId);

// Opens a console window running the official sign-in procedure.
bool OpenLoginConsole(const std::string& aiId, std::string& error);
