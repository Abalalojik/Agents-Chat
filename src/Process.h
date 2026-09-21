#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Runs a child process, feeds it stdin, and delivers its stdout line by line.
// The process and all its children live in a Windows job object, so a cancel
// (the Stop button) kills the whole tree.
namespace Process
{
    struct Result
    {
        bool started = false;
        bool cancelled = false;
        unsigned long exitCode = 0;
        std::string stderrText; // last ~8 KB of stderr, for error reporting
        std::string error;      // why it could not start
        bool timedOut = false;
    };

    // Quotes one argument for CreateProcess (MSVC argv rules).
    std::wstring QuoteArg(const std::wstring& arg);

    // args[0] is the executable path. Environment additions are "NAME=value"; "-NAME" removes NAME.
    Result Run(const std::vector<std::wstring>& args,
               const std::wstring& workingDir,
               const std::string& stdinUtf8,
               const std::function<void(const std::string& line)>& onStdoutLine,
               const std::atomic<bool>& cancel,
               const std::vector<std::wstring>& extraEnv = {},
               unsigned timeoutSeconds = 0);

    // Hard ceiling for everything the app launches (the process and its children
    // together). Beyond it Windows refuses allocations and the job is ended.
    inline constexpr size_t kJobMemoryLimit = 3ull * 1024 * 1024 * 1024;

    // Full path of an executable found on PATH ("" if none).
    std::wstring FindOnPath(const std::wstring& exeName);
}
