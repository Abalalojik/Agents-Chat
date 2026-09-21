#include "Platform.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>

namespace Platform
{
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
}
