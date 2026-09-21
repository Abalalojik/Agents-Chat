#include "Process.h"
#include "Platform.h"

#include <windows.h>

#include <thread>

namespace Process
{
    std::wstring QuoteArg(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            return arg;
        std::wstring out = L"\"";
        for (size_t i = 0;; ++i)
        {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == L'\\')
            {
                ++i;
                ++backslashes;
            }
            if (i == arg.size())
            {
                out.append(backslashes * 2, L'\\');
                break;
            }
            if (arg[i] == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
            }
            else
            {
                out.append(backslashes, L'\\');
                out.push_back(arg[i]);
            }
        }
        out.push_back(L'"');
        return out;
    }

    std::wstring FindOnPath(const std::wstring& exeName)
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = SearchPathW(nullptr, exeName.c_str(), nullptr, MAX_PATH, buf, nullptr);
        return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring();
    }

    namespace
    {
        // Current environment block plus additions (sorted order is not required by CreateProcess).
        // An extra entry "-NAME" removes NAME instead of setting it.
        std::wstring BuildEnvironment(const std::vector<std::wstring>& extra)
        {
            std::wstring block;
            wchar_t* env = GetEnvironmentStringsW();
            for (const wchar_t* p = env; *p; p += wcslen(p) + 1)
            {
                const std::wstring entry(p);
                bool overridden = false;
                for (const std::wstring& e : extra)
                {
                    if (!e.empty() && e[0] == L'-')
                    {
                        const size_t len = e.size() - 1;
                        if (entry.size() > len && entry[len] == L'=' && _wcsnicmp(entry.c_str(), e.c_str() + 1, len) == 0)
                            overridden = true;
                        continue;
                    }
                    const size_t eq = e.find(L'=');
                    if (eq != std::wstring::npos && _wcsnicmp(entry.c_str(), e.c_str(), eq + 1) == 0)
                        overridden = true;
                }
                if (!overridden)
                {
                    block += entry;
                    block.push_back(L'\0');
                }
            }
            FreeEnvironmentStringsW(env);
            for (const std::wstring& e : extra)
            {
                if (!e.empty() && e[0] == L'-')
                    continue;
                block += e;
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }
    }

    Result Run(const std::vector<std::wstring>& args,
               const std::wstring& workingDir,
               const std::string& stdinUtf8,
               const std::function<void(const std::string& line)>& onStdoutLine,
               const std::atomic<bool>& cancel,
               const std::vector<std::wstring>& extraEnv,
               unsigned timeoutSeconds)
    {
        Result result;
        if (args.empty())
        {
            result.error = "commande vide";
            return result;
        }

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        auto close = [](HANDLE& handle) {
            if (handle)
                CloseHandle(handle);
            handle = nullptr;
        };
        if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0) ||
            !CreatePipe(&errRead, &errWrite, &sa, 0))
        {
            close(inRead); close(inWrite); close(outRead); close(outWrite); close(errRead); close(errWrite);
            result.error = "création des canaux impossible (erreur " + std::to_string(GetLastError()) + ")";
            return result;
        }
        // Our ends must not be inherited by the child.
        if (!SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0))
        {
            close(inRead); close(inWrite); close(outRead); close(outWrite); close(errRead); close(errWrite);
            result.error = "configuration des canaux impossible (erreur " + std::to_string(GetLastError()) + ")";
            return result;
        }

        std::wstring cmdline;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i)
                cmdline.push_back(L' ');
            cmdline += QuoteArg(args[i]);
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = errWrite;

        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job)
        {
            close(inRead); close(inWrite); close(outRead); close(outWrite); close(errRead); close(errWrite);
            result.error = "création du groupe de processus impossible (erreur " + std::to_string(GetLastError()) + ")";
            return result;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = kJobMemoryLimit;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            close(inRead); close(inWrite); close(outRead); close(outWrite); close(errRead); close(errWrite); close(job);
            result.error = "configuration du groupe de processus impossible (erreur " + std::to_string(GetLastError()) + ")";
            return result;
        }

        std::wstring env = BuildEnvironment(extraEnv);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(args[0].c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                       env.data(), workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
        close(inRead);
        close(outWrite);
        close(errWrite);
        if (!ok)
        {
            result.error = "impossible de lancer " + Platform::Narrow(args[0]) + " (erreur " + std::to_string(GetLastError()) + ")";
            close(inWrite); close(outRead); close(errRead); close(job);
            return result;
        }
        if (!AssignProcessToJobObject(job, pi.hProcess))
        {
            const DWORD code = GetLastError();
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            close(inWrite); close(outRead); close(errRead); close(job);
            result.error = "isolation du processus impossible (erreur " + std::to_string(code) + ")";
            return result;
        }
        if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
        {
            const DWORD code = GetLastError();
            TerminateJobObject(job, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            close(inWrite); close(outRead); close(errRead); close(job);
            result.error = "démarrage du processus impossible (erreur " + std::to_string(code) + ")";
            return result;
        }
        CloseHandle(pi.hThread);
        result.started = true;

        // stdin on its own thread so a large prompt never deadlocks against stdout.
        std::thread writer([inWrite, &stdinUtf8] {
            size_t offset = 0;
            while (offset < stdinUtf8.size())
            {
                DWORD written = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(stdinUtf8.size() - offset, 64 * 1024));
                if (!WriteFile(inWrite, stdinUtf8.data() + offset, chunk, &written, nullptr) || written == 0)
                    break;
                offset += written;
            }
            CloseHandle(inWrite);
        });

        std::string stderrText;
        std::thread errReader([errRead, &stderrText] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(errRead, buf, sizeof(buf), &n, nullptr) && n > 0)
            {
                stderrText.append(buf, n);
                if (stderrText.size() > 16384)
                    stderrText.erase(0, stderrText.size() - 8192);
            }
        });

        // Watchdog: a cancel kills the whole job, which closes the pipes and ends the reads.
        std::atomic<bool> finished{false};
        std::atomic<bool> timedOut{false};
        const ULONGLONG deadline = timeoutSeconds ? GetTickCount64() + timeoutSeconds * 1000ull : 0;
        std::thread watchdog([&] {
            while (!finished)
            {
                if (cancel || (deadline && GetTickCount64() > deadline))
                {
                    timedOut = !cancel;
                    TerminateJobObject(job, 1);
                    return;
                }
                Sleep(50);
            }
        });

        std::string pending;
        bool outputTooLarge = false;
        char buf[8192];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0)
        {
            pending.append(buf, n);
            if (pending.size() > 4 * 1024 * 1024)
            {
                outputTooLarge = true;
                TerminateJobObject(job, 1);
                break;
            }
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                pending.erase(0, nl + 1);
                if (!line.empty())
                    onStdoutLine(line);
            }
        }
        if (!pending.empty() && !outputTooLarge)
            onStdoutLine(pending);

        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &result.exitCode);
        finished = true;
        watchdog.join();
        writer.join();
        errReader.join();
        result.cancelled = cancel.load();
        result.timedOut = timedOut.load();
        if (result.timedOut)
            result.error = "délai dépassé";
        else if (outputTooLarge)
            result.error = "sortie du processus trop volumineuse (ligne supérieure à 4 Mio)";
        result.stderrText = std::move(stderrText);

        CloseHandle(pi.hProcess);
        close(outRead);
        close(errRead);
        close(job);
        return result;
    }
}
