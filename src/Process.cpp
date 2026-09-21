#include "Process.h"
#include "Platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <chrono>
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

#ifdef _WIN32
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
#else
    // ---------------------------------------------------------------- POSIX
    std::wstring FindOnPath(const std::wstring& exeName)
    {
        // Callers name Windows executables ("git.exe"); on POSIX the suffix is dropped.
        std::string name = Platform::Narrow(exeName);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
            name.resize(name.size() - 4);
        if (name.find('/') != std::string::npos)
            return access(name.c_str(), X_OK) == 0 ? Platform::Widen(name) : std::wstring();
        const char* path = std::getenv("PATH");
        std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
        size_t start = 0;
        while (start <= dirs.size())
        {
            size_t end = dirs.find(':', start);
            if (end == std::string::npos)
                end = dirs.size();
            const std::string dir = dirs.substr(start, end - start);
            const std::string candidate = (dir.empty() ? "." : dir) + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0)
                return Platform::Widen(candidate);
            start = end + 1;
        }
        return {};
    }

    namespace
    {
        // Current environment plus additions; "-NAME" removes NAME.
        std::vector<std::string> BuildEnvironment(const std::vector<std::wstring>& extra)
        {
            std::vector<std::string> additions, removals;
            for (const std::wstring& e : extra)
            {
                if (!e.empty() && e[0] == L'-')
                    removals.push_back(Platform::Narrow(e.substr(1)));
                else
                    additions.push_back(Platform::Narrow(e));
            }
            auto nameOf = [](const std::string& entry) { return entry.substr(0, entry.find('=')); };
            std::vector<std::string> block;
            for (char** p = environ; p && *p; ++p)
            {
                const std::string entry(*p);
                const std::string name = nameOf(entry);
                bool dropped = std::find(removals.begin(), removals.end(), name) != removals.end();
                for (const std::string& a : additions)
                    if (nameOf(a) == name)
                        dropped = true;
                if (!dropped)
                    block.push_back(entry);
            }
            block.insert(block.end(), additions.begin(), additions.end());
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
        std::vector<std::string> argStrings;
        for (const std::wstring& a : args)
            argStrings.push_back(Platform::Narrow(a));
        std::vector<char*> argv;
        for (std::string& a : argStrings)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        std::vector<std::string> envStrings = BuildEnvironment(extraEnv);
        std::vector<char*> envp;
        for (std::string& e : envStrings)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        const std::string dir = Platform::Narrow(workingDir);

        int in[2], out[2], err[2], status[2];
        if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0 || pipe(status) != 0)
        {
            result.error = std::string("création des canaux impossible : ") + std::strerror(errno);
            return result;
        }
        fcntl(status[1], F_SETFD, FD_CLOEXEC);

        const pid_t pid = fork();
        if (pid < 0)
        {
            result.error = std::string("fork impossible : ") + std::strerror(errno);
            for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1], status[0], status[1]})
                close(fd);
            return result;
        }
        if (pid == 0)
        {
            // Child: own process group, so a cancel kills the whole tree.
            setpgid(0, 0);
            dup2(in[0], 0);
            dup2(out[1], 1);
            dup2(err[1], 2);
            for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1], status[0]})
                close(fd);
            if (!dir.empty() && chdir(dir.c_str()) != 0)
            {
                const int code = errno;
                (void)!write(status[1], &code, sizeof(code));
                _exit(127);
            }
            execve(argv[0], argv.data(), envp.data());
            const int code = errno;
            (void)!write(status[1], &code, sizeof(code));
            _exit(127);
        }
        setpgid(pid, pid);
        close(in[0]);
        close(out[1]);
        close(err[1]);
        close(status[1]);
        int execError = 0;
        const bool failedToStart = read(status[0], &execError, sizeof(execError)) == static_cast<ssize_t>(sizeof(execError));
        close(status[0]);
        if (failedToStart)
        {
            waitpid(pid, nullptr, 0);
            close(in[1]);
            close(out[0]);
            close(err[0]);
            result.error = "impossible de lancer " + argStrings[0] + " (" + std::strerror(execError) + ")";
            return result;
        }
        result.started = true;

        std::thread writer([fd = in[1], &stdinUtf8] {
            signal(SIGPIPE, SIG_IGN);
            size_t offset = 0;
            while (offset < stdinUtf8.size())
            {
                const ssize_t n = write(fd, stdinUtf8.data() + offset, std::min<size_t>(stdinUtf8.size() - offset, 64 * 1024));
                if (n <= 0)
                    break;
                offset += static_cast<size_t>(n);
            }
            close(fd);
        });

        std::atomic<bool> finished{false};
        std::atomic<bool> timedOut{false};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
        std::thread watchdog([&] {
            while (!finished)
            {
                if (cancel || (timeoutSeconds && std::chrono::steady_clock::now() > deadline))
                {
                    timedOut = !cancel;
                    kill(-pid, SIGKILL);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        std::string pending, stderrText;
        bool outputTooLarge = false;
        pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
        int openCount = 2;
        char buf[8192];
        while (openCount > 0)
        {
            if (poll(fds, 2, 200) < 0 && errno != EINTR)
                break;
            for (pollfd& p : fds)
            {
                if (p.fd < 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                const ssize_t n = read(p.fd, buf, sizeof(buf));
                if (n <= 0)
                {
                    close(p.fd);
                    p.fd = -1;
                    --openCount;
                    continue;
                }
                if (p.fd == err[0])
                {
                    stderrText.append(buf, static_cast<size_t>(n));
                    if (stderrText.size() > 16384)
                        stderrText.erase(0, stderrText.size() - 8192);
                    continue;
                }
                pending.append(buf, static_cast<size_t>(n));
                if (pending.size() > 4 * 1024 * 1024)
                {
                    outputTooLarge = true;
                    kill(-pid, SIGKILL);
                    pending.clear();
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
        }
        if (!pending.empty() && !outputTooLarge)
            onStdoutLine(pending);

        int statusCode = 0;
        waitpid(pid, &statusCode, 0);
        finished = true;
        watchdog.join();
        writer.join();
        result.exitCode = WIFEXITED(statusCode) ? static_cast<unsigned long>(WEXITSTATUS(statusCode)) : 1ul;
        result.cancelled = cancel.load();
        result.timedOut = timedOut.load();
        if (result.timedOut)
            result.error = "délai dépassé";
        else if (outputTooLarge)
            result.error = "sortie du processus trop volumineuse (ligne supérieure à 4 Mio)";
        result.stderrText = std::move(stderrText);
        return result;
    }
#endif
}
