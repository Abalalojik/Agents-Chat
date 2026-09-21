#include "Updater.h"
#include "Http.h"
#include "Platform.h"
#include "Process.h"

#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    std::string Trim(std::string s)
    {
        const size_t first = s.find_first_not_of(" \t\r\n");
        const size_t last = s.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    }

    std::string Sha256(const fs::path& path)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectSize = 0, bytes = 0, hashSize = 0;
        std::string result;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0);
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &bytes, 0);
        std::vector<unsigned char> object(objectSize), digest(hashSize);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) == 0)
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<unsigned char> buffer(64 * 1024);
            while (in)
            {
                in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                if (in.gcount() > 0) BCryptHashData(hash, buffer.data(), static_cast<ULONG>(in.gcount()), 0);
            }
            if (in.eof() && BCryptFinishHash(hash, digest.data(), hashSize, 0) == 0)
            {
                static constexpr char hex[] = "0123456789abcdef";
                for (unsigned char b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
            }
        }
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }

    std::vector<int> VersionParts(std::string value)
    {
        if (!value.empty() && (value[0] == 'v' || value[0] == 'V')) value.erase(0, 1);
        std::vector<int> out;
        std::stringstream stream(value);
        std::string part;
        while (std::getline(stream, part, '.') && out.size() < 3)
        {
            try { out.push_back(std::stoi(part)); } catch (...) { out.push_back(0); }
        }
        while (out.size() < 3) out.push_back(0);
        return out;
    }
}

Updater::Updater(fs::path dataRoot)
    : m_root(dataRoot / "updates"), m_pending(m_root / "AgentChats.new.exe"),
      m_source(dataRoot / "projects" / "Agents-Chat") {}
Updater::~Updater() { if (m_thread.joinable()) m_thread.join(); }

void Updater::Check() { RunCheck(false); }
void Updater::Download() { RunCheck(true); }

bool Updater::SourceReady() const { return fs::exists(m_source / ".git") && fs::exists(m_source / "CMakeLists.txt"); }

void Updater::PrepareSource()
{
    if (SourceReady() || m_busy.exchange(true)) return;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this] {
        auto done = [&](std::string text) { std::lock_guard<std::mutex> lock(m_mutex); m_status = std::move(text); m_busy = false; };
        const std::wstring git = Process::FindOnPath(L"git.exe");
        if (git.empty()) { done("Auto-amélioration indisponible : Git n'est pas installé."); return; }
        std::error_code ec; fs::create_directories(m_source.parent_path(), ec);
        std::atomic<bool> cancel{false};
        const Process::Result result = Process::Run({git, L"clone", L"--origin", L"upstream", L"https://github.com/Abalalojik/Agents-Chat.git", m_source.wstring()},
            m_source.parent_path().wstring(), "", [](const std::string&) {}, cancel, {}, 300);
        done(result.exitCode == 0 && SourceReady() ? "Copie locale du code prête pour l'auto-amélioration."
                                                   : "Clonage du code impossible : " + result.stderrText);
    });
}

void Updater::BuildLocal()
{
    if (!SourceReady() || m_busy.exchange(true)) return;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this] {
        auto done = [&](std::string text) { std::lock_guard<std::mutex> lock(m_mutex); m_status = std::move(text); m_busy = false; };
        const std::wstring cmake = Process::FindOnPath(L"cmake.exe");
        if (cmake.empty()) { done("Compilation locale impossible : CMake n'est pas installé ou n'est pas dans PATH."); return; }
        std::atomic<bool> cancel{false}; std::string output;
        const fs::path build = m_source / "build-local";
        auto collect = [&](const std::string& line) { output += line + "\n"; if (output.size() > 8000) output.erase(0, output.size() - 8000); };
        Process::Result r = Process::Run({cmake, L"-S", m_source.wstring(), L"-B", build.wstring()}, m_source.wstring(), "", collect, cancel, {}, 600);
        if (r.exitCode == 0)
            r = Process::Run({cmake, L"--build", build.wstring(), L"--config", L"Release"}, m_source.wstring(), "", collect, cancel, {}, 1200);
        const fs::path built = build / "Release" / "AgentChats.exe";
        std::error_code ec; fs::create_directories(m_root, ec);
        if (r.exitCode == 0 && fs::copy_file(built, m_pending, fs::copy_options::overwrite_existing, ec))
        {
            { std::lock_guard<std::mutex> lock(m_mutex); m_ready = true; m_version = "locale"; }
            done("Version locale compilée et prête à installer.");
        }
        else done("Compilation locale échouée : " + (r.stderrText.empty() ? output : r.stderrText));
    });
}

void Updater::PrepareContribution()
{
    if (!SourceReady() || m_busy.exchange(true)) return;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this] {
        auto done = [&](std::string text) { std::lock_guard<std::mutex> lock(m_mutex); m_status = std::move(text); m_busy = false; };
        const std::wstring git = Process::FindOnPath(L"git.exe");
        if (git.empty()) { done("Préparation impossible : Git n'est pas installé."); return; }
        std::atomic<bool> cancel{false}; std::string diff;
        const Process::Result result = Process::Run({git, L"diff", L"--binary", L"HEAD"}, m_source.wstring(), "",
            [&](const std::string& line) { diff += line + "\n"; }, cancel, {}, 120);
        if (result.exitCode != 0) { done("Préparation impossible : " + result.stderrText); return; }
        if (diff.empty()) { done("Aucune modification locale à proposer."); return; }
        std::string error;
        const fs::path patch = m_root / "Agents-Chat-contribution.patch";
        fs::create_directories(m_root);
        if (!Platform::WriteFileAtomic(patch, diff, error)) { done("Écriture du patch impossible : " + error); return; }
        done("Contribution préparée pour révision : " + Platform::Narrow(patch.wstring()) +
             ". Aucun fichier n'a été publié. Après révision, l'application pourra proposer son envoi en PR.");
    });
}

void Updater::RunCheck(bool download)
{
    if (m_busy.exchange(true)) return;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this, download] {
        std::atomic<bool> cancel{false};
        auto setStatus = [&](std::string value) { std::lock_guard<std::mutex> lock(m_mutex); m_status = std::move(value); };
        setStatus(download ? "Téléchargement…" : "Recherche d'une mise à jour…");
        const Http::Response release = Http::Request("GET", "https://api.github.com/repos/Abalalojik/Agents-Chat/releases/latest",
            {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}}, "", {}, cancel);
        try
        {
            if (release.status != 200) throw std::runtime_error(release.error.empty() ? "GitHub répond " + std::to_string(release.status) : release.error);
            const json doc = json::parse(release.body);
            const std::string version = doc.value("tag_name", "");
            std::string exeUrl, hashUrl;
            for (const json& asset : doc.value("assets", json::array()))
            {
                const std::string name = asset.value("name", "");
                if (name == "AgentChats.exe") exeUrl = asset.value("browser_download_url", "");
                if (name == "AgentChats.exe.sha256") hashUrl = asset.value("browser_download_url", "");
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_version = version; m_downloadUrl = exeUrl; m_hashUrl = hashUrl; m_ready = false;
            }
            if (VersionParts(version) <= VersionParts(kAgentChatsVersion))
                setStatus("Agents Chat est à jour (v" + std::string(kAgentChatsVersion) + ").");
            else if (exeUrl.empty() || hashUrl.empty())
                setStatus("Release " + version + " trouvée, mais elle ne contient pas l'exécutable signé par SHA-256.");
            else if (!download)
                setStatus("Mise à jour " + version + " disponible.");
            else
            {
                fs::create_directories(m_root);
                std::ofstream out(m_pending, std::ios::binary | std::ios::trunc);
                const Http::Response binary = Http::Request("GET", exeUrl, {}, "", [&](const std::string& chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); }, cancel);
                out.close();
                const Http::Response expected = Http::Request("GET", hashUrl, {}, "", {}, cancel);
                std::string wanted = Trim(expected.body);
                const size_t separator = wanted.find_first_of(" \t");
                if (separator != std::string::npos) wanted.resize(separator);
                std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (binary.status < 200 || binary.status >= 300 || expected.status != 200 || wanted.size() != 64 || Sha256(m_pending) != wanted)
                {
                    std::error_code ec; fs::remove(m_pending, ec);
                    throw std::runtime_error("le téléchargement ou sa vérification SHA-256 a échoué");
                }
                { std::lock_guard<std::mutex> lock(m_mutex); m_ready = true; }
                setStatus("Mise à jour " + version + " vérifiée et prête à installer.");
            }
        }
        catch (const std::exception& e) { setStatus(std::string("Mise à jour impossible : ") + e.what()); }
        m_busy = false;
    });
}

std::string Updater::Status() const { std::lock_guard<std::mutex> lock(m_mutex); return m_status; }
std::string Updater::AvailableVersion() const { std::lock_guard<std::mutex> lock(m_mutex); return m_version; }
bool Updater::Ready() const { std::lock_guard<std::mutex> lock(m_mutex); return m_ready; }

bool Updater::Apply(void* hwnd)
{
    if (!Ready() || !fs::exists(m_pending)) return false;
    wchar_t current[32768];
    const DWORD n = GetModuleFileNameW(nullptr, current, static_cast<DWORD>(std::size(current)));
    if (!n || n >= std::size(current)) return false;
    std::wstring command = Process::QuoteArg(m_pending.wstring()) + L" --apply-update " +
                           Process::QuoteArg(std::wstring(current, n)) + L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(m_pending.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    PostMessageW(static_cast<HWND>(hwnd), WM_CLOSE, 0, 0);
    return true;
}

bool Updater::ApplyPendingUpdate(const fs::path& destination, unsigned long parentPid)
{
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parentPid)) { WaitForSingleObject(process, 30000); CloseHandle(process); }
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!n || !CopyFileW(self, destination.c_str(), FALSE)) return false;
    ShellExecuteW(nullptr, L"open", destination.c_str(), nullptr, destination.parent_path().c_str(), SW_SHOWNORMAL);
    MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}
