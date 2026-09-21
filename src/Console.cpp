#include "Console.h"
#include "Platform.h"
#include "Process.h"

#ifdef _WIN32
#include <windows.h>
#else
extern char** environ;
#endif

#include <algorithm>
#include <cwctype>
#include <regex>

namespace fs = std::filesystem;

namespace Console
{
    const char* ProfileName(Profile profile)
    {
        switch (profile)
        {
#ifdef _WIN32
        case Profile::Cmd: return "CMD";
#else
        case Profile::Cmd: return "sh";
#endif
        case Profile::Gcloud: return "gcloud";
        default: return "PowerShell";
        }
    }

    Profile ProfileFromName(const std::string& name)
    {
        if (name == "CMD" || name == "sh") return Profile::Cmd;
        if (name == "gcloud") return Profile::Gcloud;
        return Profile::PowerShell;
    }

    bool IsSecretEnvName(const std::wstring& name)
    {
        std::wstring upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
        for (const wchar_t* part : {L"TOKEN", L"SECRET", L"PASSWORD", L"PASSWD", L"PWD_", L"APIKEY", L"API_KEY", L"_KEY",
                                    L"CREDENTIAL", L"PRIVATE", L"SESSION", L"COOKIE", L"AUTH"})
            if (upper.find(part) != std::wstring::npos)
                return true;
        for (const wchar_t* prefix : {L"AWS_", L"AZURE_", L"GOOGLE_APPLICATION_CREDENTIALS", L"CLOUDSDK_AUTH"})
            if (upper.rfind(prefix, 0) == 0)
                return true;
        return false;
    }

    std::vector<std::wstring> SecretEnvRemovals()
    {
        std::vector<std::wstring> out;
#ifdef _WIN32
        wchar_t* env = GetEnvironmentStringsW();
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1)
        {
            const std::wstring entry(p);
#else
        for (char** p = environ; p && *p; ++p)
        {
            const std::wstring entry = Platform::Widen(*p);
#endif
            const size_t eq = entry.find(L'=', 1); // "=C:=C:\\…" entries start with '='
            if (eq == std::wstring::npos)
                continue;
            const std::wstring name = entry.substr(0, eq);
            if (IsSecretEnvName(name))
                out.push_back(L"-" + name);
        }
#ifdef _WIN32
        FreeEnvironmentStringsW(env);
#endif
        return out;
    }

    std::string Mask(const std::string& text, const std::vector<std::string>& secrets)
    {
        std::string out = text;
        for (const std::string& secret : secrets)
        {
            if (secret.size() < 8)
                continue;
            for (size_t at = out.find(secret); at != std::string::npos; at = out.find(secret, at + 4))
                out.replace(at, secret.size(), "••••");
        }
        static const std::regex shapes(
            R"((sk-(ant-)?[A-Za-z0-9_\-]{16,})|(gh[pousr]_[A-Za-z0-9]{20,})|(github_pat_[A-Za-z0-9_]{20,})|)"
            R"((AIza[0-9A-Za-z_\-]{30,})|(xai-[A-Za-z0-9]{20,})|(ya29\.[0-9A-Za-z_\-]{20,})|(AKIA[0-9A-Z]{16}))");
        out = std::regex_replace(out, shapes, "••••");
        static const std::regex bearer(R"((Authorization:\s*(?:Bearer|Basic|token)?|Bearer|password=|token=)\s*[^\s"']+)", std::regex::icase);
        out = std::regex_replace(out, bearer, "$1 ••••");
        return out;
    }

    bool ValidGcloud(const std::string& command, std::string& error)
    {
        if (command.rfind("gcloud ", 0) != 0)
        {
            error = "Le profil gcloud n'accepte que des commandes « gcloud … ».";
            return false;
        }
        if (command.find_first_of("&|;<>`$()%^\r\n") != std::string::npos)
        {
            error = "Le profil gcloud refuse l'enchaînement de commandes et les redirections.";
            return false;
        }
        return true;
    }

    Prepared Prepare(Profile profile, const std::string& command, const fs::path& scratchDir)
    {
        Prepared p;
        if (command.empty() || command.find('\0') != std::string::npos)
        {
            p.error = "Commande vide.";
            return p;
        }
        if (profile == Profile::Gcloud && !ValidGcloud(command, p.error))
            return p;

#ifndef _WIN32
        // Linux: "CMD" and gcloud run through /bin/sh, PowerShell through pwsh when installed;
        // the command goes into a script file, never through a quoted command line.
        std::error_code linuxEc;
        fs::create_directories(scratchDir, linuxEc);
        const bool powershell = profile == Profile::PowerShell;
        const std::wstring shell = powershell ? Process::FindOnPath(L"pwsh") : std::wstring(L"/bin/sh");
        if (shell.empty())
        {
            p.error = "PowerShell (pwsh) n'est pas installé sur ce système.";
            return p;
        }
        p.script = scratchDir / (Platform::Widen(Platform::NewId()) + (powershell ? L".ps1" : L".sh"));
        std::string linuxError;
        if (!Platform::WriteFileAtomic(p.script, command + "\n", linuxError))
        {
            p.error = "Préparation de la commande impossible : " + linuxError;
            p.script.clear();
            return p;
        }
        if (powershell)
            p.args = {shell, L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-File", p.script.wstring()};
        else
            p.args = {shell, p.script.wstring()};
        return p;
#else
        wchar_t system[MAX_PATH];
        const UINT n = GetSystemDirectoryW(system, MAX_PATH);
        const fs::path systemDir = (n > 0 && n < MAX_PATH) ? fs::path(system) : fs::path(L"C:\\Windows\\System32");

        if (profile == Profile::PowerShell)
        {
            // -EncodedCommand: UTF-16LE base64, immune to quoting.
            const std::wstring script = L"[Console]::OutputEncoding=[Text.Encoding]::UTF8; $ProgressPreference='SilentlyContinue'; " +
                                        Platform::Widen(command);
            std::string bytes(reinterpret_cast<const char*>(script.data()), script.size() * sizeof(wchar_t));
            static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::wstring b64;
            for (size_t i = 0; i < bytes.size(); i += 3)
            {
                const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                                   (i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) << 8 : 0) |
                                   (i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0);
                b64.push_back(static_cast<wchar_t>(kB64[(v >> 18) & 63]));
                b64.push_back(static_cast<wchar_t>(kB64[(v >> 12) & 63]));
                b64.push_back(i + 1 < bytes.size() ? static_cast<wchar_t>(kB64[(v >> 6) & 63]) : L'=');
                b64.push_back(i + 2 < bytes.size() ? static_cast<wchar_t>(kB64[v & 63]) : L'=');
            }
            p.args = {(systemDir / L"WindowsPowerShell\\v1.0\\powershell.exe").wstring(), L"-NoLogo", L"-NoProfile",
                      L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-EncodedCommand", b64};
            return p;
        }

        // CMD and gcloud: a temporary UTF-8 batch file, so cmd.exe parses the line itself.
        std::error_code ec;
        fs::create_directories(scratchDir, ec);
        p.script = scratchDir / (Platform::Widen(Platform::NewId()) + L".cmd");
        std::string error;
        if (!Platform::WriteFileAtomic(p.script, "@echo off\r\nchcp 65001 >nul\r\n" + command + "\r\n", error))
        {
            p.error = "Préparation de la commande impossible : " + error;
            p.script.clear();
            return p;
        }
        p.args = {(systemDir / L"cmd.exe").wstring(), L"/d", L"/c", p.script.wstring()};
        return p;
#endif
    }
}
