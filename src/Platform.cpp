#include "Platform.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#endif

#include <cstdio>
#include <vector>
#include <ctime>
#include <fstream>
#include <random>

namespace Platform
{
#ifdef _WIN32
    std::wstring Widen(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
        return out;
    }

    std::string Narrow(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    bool WriteFileAtomic(const std::filesystem::path& file, const std::string& text, std::string& error)
    {
        error.clear();
        std::filesystem::path tmp = file;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out)
            {
                error = "écriture du fichier temporaire impossible";
                return false;
            }
        }

        // std::filesystem::rename does not replace an existing destination on
        // Windows. MoveFileExW does, and WRITE_THROUGH asks Windows to flush the
        // move before reporting success.
        if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD code = GetLastError();
            DeleteFileW(tmp.c_str());
            error = "remplacement impossible (erreur Windows " + std::to_string(code) + ")";
            return false;
        }
        return true;
    }

    std::filesystem::path DataRoot()
    {
        std::filesystem::path root;
        wchar_t overrideDir[MAX_PATH];
        PWSTR localAppData = nullptr;
        if (GetEnvironmentVariableW(L"AGENTCHATS_DATA", overrideDir, MAX_PATH) > 0)
        {
            // Test or portable setups point the app at another data folder.
            root = overrideDir;
        }
        else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        {
            root = std::filesystem::path(localAppData) / L"AgentChats";
            CoTaskMemFree(localAppData);
        }
        else
        {
            root = std::filesystem::current_path() / L"AgentChatsData";
        }
        std::filesystem::create_directories(root);
        return root;
    }

    std::filesystem::path ProjectRoot()
    {
        std::wstring buffer(32768, L'\0');
        const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (count == 0 || count >= buffer.size())
            return {};
        buffer.resize(count);
        std::filesystem::path candidate = std::filesystem::path(buffer).parent_path();
        for (int depth = 0; depth < 6 && !candidate.empty(); ++depth)
        {
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate / L"CMakeLists.txt", ec) &&
                std::filesystem::is_regular_file(candidate / L"src" / L"App.cpp", ec))
                return std::filesystem::weakly_canonical(candidate, ec);
            const auto parent = candidate.parent_path();
            if (parent == candidate)
                break;
            candidate = parent;
        }
        return {};
    }

    std::string NewId()
    {
        static std::mt19937_64 rng{std::random_device{}()};
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
        return buf;
    }

    std::string NowIsoUtc()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_s(&utc, &now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buf;
    }

    std::string LocalTimeOfDay(const std::string& isoUtc)
    {
        std::tm utc{};
        if (sscanf_s(isoUtc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                        &utc.tm_year, &utc.tm_mon, &utc.tm_mday,
                        &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6)
            return {};
        utc.tm_year -= 1900;
        utc.tm_mon -= 1;
        const std::time_t t = _mkgmtime(&utc);
        std::tm local{};
        if (t == -1 || localtime_s(&local, &t) != 0)
            return {};
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", &local);
        return buf;
    }

    std::string LocalWhen(const std::string& isoUtc)
    {
        std::tm utc{};
        if (sscanf_s(isoUtc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                     &utc.tm_year, &utc.tm_mon, &utc.tm_mday,
                     &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6)
            return {};
        utc.tm_year -= 1900;
        utc.tm_mon -= 1;
        const std::time_t t = _mkgmtime(&utc);
        const std::time_t now = std::time(nullptr);
        std::tm local{}, today{};
        if (t == -1 || localtime_s(&local, &t) != 0 || localtime_s(&today, &now) != 0)
            return {};
        const bool sameDay = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday;
        char buf[16];
        std::strftime(buf, sizeof(buf), sameDay ? "%H:%M" : "%d/%m %H:%M", &local);
        return buf;
    }

    void OpenUrl(const std::string& url)
    {
        ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool MoveToTrash(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return true;
        std::wstring from = path.wstring();
        from.push_back(L'\0'); // double-null terminated list
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        return SHFileOperationW(&op) == 0;
    }

    std::string PickFolder(void* ownerHwnd, const wchar_t* title)
    {
        std::string result;
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return result;

        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(title);

        if (SUCCEEDED(dialog->Show(static_cast<HWND>(ownerHwnd))))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    result = Narrow(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
        return result;
    }
#else
    // POSIX: wchar_t is UTF-32; conversions are done by hand, independent of the C locale.
    std::wstring Widen(const std::string& utf8)
    {
        std::wstring out;
        out.reserve(utf8.size());
        for (size_t i = 0; i < utf8.size();)
        {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            unsigned code = 0xFFFD;
            size_t extra = 0;
            if (c < 0x80) { code = c; }
            else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { code = c & 0x07; extra = 3; }
            ++i;
            for (size_t k = 0; k < extra; ++k, ++i)
            {
                if (i >= utf8.size() || (static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80)
                {
                    code = 0xFFFD;
                    break;
                }
                code = (code << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
            }
            out.push_back(static_cast<wchar_t>(code));
        }
        return out;
    }

    std::string Narrow(const std::wstring& wide)
    {
        std::string out;
        out.reserve(wide.size());
        for (wchar_t w : wide)
        {
            const unsigned code = static_cast<unsigned>(w);
            if (code < 0x80)
                out.push_back(static_cast<char>(code));
            else if (code < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else if (code < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }
        return out;
    }

    bool WriteFileAtomic(const std::filesystem::path& file, const std::string& text, std::string& error)
    {
        error.clear();
        std::filesystem::path tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out)
            {
                error = "écriture du fichier temporaire impossible";
                return false;
            }
        }
        // rename(2) atomically replaces an existing destination on POSIX.
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
        if (ec)
        {
            std::filesystem::remove(tmp, ec);
            error = "remplacement impossible : " + ec.message();
            return false;
        }
        return true;
    }

    std::filesystem::path DataRoot()
    {
        std::filesystem::path root;
        if (const char* overrideDir = std::getenv("AGENTCHATS_DATA"); overrideDir && *overrideDir)
            root = overrideDir;
        else if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            root = std::filesystem::path(xdg) / "AgentChats";
        else if (const char* home = std::getenv("HOME"); home && *home)
            root = std::filesystem::path(home) / ".local" / "share" / "AgentChats";
        else
            root = std::filesystem::current_path() / "AgentChatsData";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        return root;
    }

    std::filesystem::path ProjectRoot()
    {
        std::error_code ec;
        std::filesystem::path candidate = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
        for (int depth = 0; depth < 6 && !candidate.empty(); ++depth)
        {
            if (std::filesystem::is_regular_file(candidate / "CMakeLists.txt", ec) &&
                std::filesystem::is_regular_file(candidate / "src" / "App.cpp", ec))
                return std::filesystem::weakly_canonical(candidate, ec);
            const auto parent = candidate.parent_path();
            if (parent == candidate)
                break;
            candidate = parent;
        }
        return {};
    }

    std::string NewId()
    {
        static std::mt19937_64 rng{std::random_device{}()};
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
        return buf;
    }

    std::string NowIsoUtc()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buf;
    }

    namespace
    {
        bool ParseIsoUtc(const std::string& isoUtc, std::time_t& out)
        {
            std::tm utc{};
            if (std::sscanf(isoUtc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &utc.tm_year, &utc.tm_mon, &utc.tm_mday,
                            &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6)
                return false;
            utc.tm_year -= 1900;
            utc.tm_mon -= 1;
            out = timegm(&utc);
            return out != -1;
        }
    }

    std::string LocalTimeOfDay(const std::string& isoUtc)
    {
        std::time_t t = 0;
        std::tm local{};
        if (!ParseIsoUtc(isoUtc, t) || !localtime_r(&t, &local))
            return {};
        char buf[8];
        std::strftime(buf, sizeof(buf), "%H:%M", &local);
        return buf;
    }

    std::string LocalWhen(const std::string& isoUtc)
    {
        std::time_t t = 0;
        const std::time_t now = std::time(nullptr);
        std::tm local{}, today{};
        if (!ParseIsoUtc(isoUtc, t) || !localtime_r(&t, &local) || !localtime_r(&now, &today))
            return {};
        const bool sameDay = local.tm_year == today.tm_year && local.tm_yday == today.tm_yday;
        char buf[16];
        std::strftime(buf, sizeof(buf), sameDay ? "%H:%M" : "%d/%m %H:%M", &local);
        return buf;
    }

    namespace
    {
        // Runs a helper program without a shell; returns its exit code (-1 if it could not start).
        int RunHelper(std::vector<std::string> args, bool wait)
        {
            std::vector<char*> argv;
            for (std::string& a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            const pid_t pid = fork();
            if (pid < 0)
                return -1;
            if (pid == 0)
            {
                if (!wait)
                    setsid(); // a detached browser outlives nothing of ours
                execvp(argv[0], argv.data());
                _exit(127);
            }
            if (!wait)
                return 0;
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }

    void OpenUrl(const std::string& url)
    {
        RunHelper({"xdg-open", url}, false);
    }

    bool MoveToTrash(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return true;
        // freedesktop trash through GIO; never a permanent delete as a fallback.
        return RunHelper({"gio", "trash", path.string()}, true) == 0 && !std::filesystem::exists(path, ec);
    }

    std::string PickFolder(void*, const wchar_t* title)
    {
        // zenity when present (GNOME and most desktops); otherwise the caller keeps its text field.
        if (std::system("command -v zenity >/dev/null 2>&1") != 0)
            return {};
        std::string safeTitle;
        for (char c : Narrow(title ? title : L""))
            if (c != '\'' && c != '\\')
                safeTitle.push_back(c);
        FILE* pipe = popen(("zenity --file-selection --directory --title='" + safeTitle + "' 2>/dev/null").c_str(), "r");
        if (!pipe)
            return {};
        std::string result;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe))
            result += buf;
        pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
        return result;
    }
#endif
}
