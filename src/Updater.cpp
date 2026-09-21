#include "Updater.h"
#include "Http.h"
#include "Platform.h"
#include "Process.h"
#include "ReleaseSignature.h"

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

    std::wstring FindCMake()
    {
        std::wstring cmake = Process::FindOnPath(L"cmake.exe");
        if (!cmake.empty()) return cmake;
        for (const fs::path& candidate : {
                 fs::path(L"C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
                 fs::path(L"C:/Program Files/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
                 fs::path(L"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
                 fs::path(L"C:/Program Files/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")})
            if (fs::exists(candidate)) return candidate.wstring();
        return {};
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

void Updater::RefreshSource()
{
    if (!SourceReady() || m_busy.exchange(true)) return;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this] {
        auto done = [&](std::string text) { std::lock_guard<std::mutex> lock(m_mutex); m_status = std::move(text); m_busy = false; };
        const std::wstring git = Process::FindOnPath(L"git.exe");
        if (git.empty()) { done("Git n'est pas installé."); return; }
        std::atomic<bool> cancel{false};
        std::string out;
        auto run = [&](std::vector<std::wstring> args, unsigned timeout) {
            out.clear();
            args.insert(args.begin(), git);
            return Process::Run(args, m_source.wstring(), "", [&](const std::string& line) { out += line + "\n"; }, cancel,
                                {L"GIT_TERMINAL_PROMPT=0"}, timeout);
        };
        if (run({L"status", L"--porcelain"}, 60).exitCode != 0 || !out.empty())
        { done("Copie locale : des modifications sont en cours, elle n'est pas mise à jour."); return; }
        if (run({L"rev-parse", L"--abbrev-ref", L"HEAD"}, 30).exitCode != 0 || out.rfind("main", 0) != 0)
        { done("Copie locale : une branche de travail est en cours, elle n'est pas mise à jour."); return; }
        const Process::Result fetched = run({L"fetch", L"upstream"}, 120);
        if (fetched.exitCode != 0) { done("Copie locale : mise à jour impossible (" + fetched.stderrText.substr(0, 200) + ")."); return; }
        const Process::Result merged = run({L"merge", L"--ff-only", L"upstream/main"}, 60);
        done(merged.exitCode == 0 ? "Copie locale à jour avec GitHub." : "Copie locale : elle a divergé de GitHub, rien n'a été changé.");
    });
}

bool Updater::BuildLocal(const fs::path& sourceOverride)
{
    const fs::path source = sourceOverride.empty() ? m_source : sourceOverride;
    std::error_code sourceEc;
    if (!fs::is_regular_file(source / "CMakeLists.txt", sourceEc) || m_busy.exchange(true)) return false;
    if (m_thread.joinable()) m_thread.join();
    m_thread = std::thread([this, source] {
        BuildReport report;
        std::string output;
        auto collect = [&](const std::string& line) { output += line + "\n"; if (output.size() > 16000) output.erase(0, output.size() - 16000); };
        auto finish = [&](std::string text) {
            report.summary = text;
            report.log = output.size() > 4000 ? output.substr(output.size() - 4000) : output;
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status = std::move(text);
            m_report = std::move(report);
            m_busy = false;
        };
        const std::wstring cmake = FindCMake();
        if (cmake.empty()) { finish("Compilation locale impossible : CMake n'est pas installé ou n'est pas dans PATH."); return; }
        { std::lock_guard<std::mutex> lock(m_mutex); m_status = "Compilation de la version locale…"; }
        std::atomic<bool> cancel{false};
        const fs::path build = source / "build-local";
        Process::Result r = Process::Run({cmake, L"-S", source.wstring(), L"-B", build.wstring()}, source.wstring(), "", collect, cancel, {}, 600);
        if (r.exitCode == 0 && r.started && !r.timedOut)
            r = Process::Run({cmake, L"--build", build.wstring(), L"--config", L"Release", L"--target", L"AgentChats", L"AgentChatsTests"},
                             source.wstring(), "", collect, cancel, {}, 1800);
        if (!r.started || r.timedOut || r.exitCode != 0)
        {
            finish("Compilation locale échouée" + std::string(r.timedOut ? " (délai dépassé)." : ".") +
                   (r.stderrText.empty() ? "" : " " + r.stderrText.substr(0, 600)));
            return;
        }
        report.built = true;
        // The tests of the modified code decide; a failing build is never staged.
        { std::lock_guard<std::mutex> lock(m_mutex); m_status = "Tests de la version locale…"; }
        output += "\n--- AgentChatsTests ---\n";
        const fs::path tests = build / "Release" / "AgentChatsTests.exe";
        r = Process::Run({tests.wstring()}, build.wstring(), "", collect, cancel, {}, 600);
        report.testsPassed = r.started && !r.timedOut && r.exitCode == 0;
        if (!report.testsPassed)
        {
            finish("Version locale compilée, mais les tests échouent" + std::string(r.timedOut ? " (délai dépassé)" : "") +
                   " : elle ne sera pas installée.");
            return;
        }
        const fs::path built = build / "Release" / "AgentChats.exe";
        std::error_code ec; fs::create_directories(m_root, ec);
        if (!fs::copy_file(built, m_pending, fs::copy_options::overwrite_existing, ec))
        {
            finish("Tests réussis, mais la copie de l'exécutable a échoué : " + ec.message());
            return;
        }
        report.ok = true;
        { std::lock_guard<std::mutex> lock(m_mutex); m_ready = true; m_version = "locale"; }
        finish("Version locale compilée, tests réussis : prête à installer.");
    });
    return true;
}

std::optional<Updater::BuildReport> Updater::TakeReport()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::optional<BuildReport> out = std::move(m_report);
    m_report.reset();
    return out;
}

bool Updater::LocalBuildReady() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ready && m_version == "locale";
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
            if (release.status == 404)
            {
                setStatus("Aucune release publiée sur GitHub pour l'instant.");
                m_busy = false;
                return;
            }
            if (release.status != 200) throw std::runtime_error(release.error.empty() ? "GitHub répond " + std::to_string(release.status) : release.error);
            const json doc = json::parse(release.body);
            const std::string version = doc.value("tag_name", "");
            std::string exeUrl, hashUrl, sigUrl;
            for (const json& asset : doc.value("assets", json::array()))
            {
                const std::string name = asset.value("name", "");
                if (name == "AgentChats.exe") exeUrl = asset.value("browser_download_url", "");
                if (name == "AgentChats.exe.sha256") hashUrl = asset.value("browser_download_url", "");
                if (name == "AgentChats.exe.sig") sigUrl = asset.value("browser_download_url", "");
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_version = version; m_downloadUrl = exeUrl; m_hashUrl = hashUrl; m_ready = false;
            }
            if (VersionParts(version) <= VersionParts(kAgentChatsVersion))
                setStatus("Agents Chat est à jour (v" + std::string(kAgentChatsVersion) + ").");
            else if (exeUrl.empty() || sigUrl.empty())
                setStatus("Release " + version + " trouvée, mais non signée : elle ne sera pas installée automatiquement.");
            else if (!ReleaseSignature::HasTrustedKeys())
                setStatus("Release " + version + " disponible, mais cette copie d'Agents Chat n'embarque aucune clé de "
                          "signature : installe-la à la main une fois (voir RELEASING.md).");
            else if (!download)
                setStatus("Mise à jour " + version + " disponible.");
            else
            {
                fs::create_directories(m_root);
                std::ofstream out(m_pending, std::ios::binary | std::ios::trunc);
                const Http::Response binary = Http::Request("GET", exeUrl, {}, "", [&](const std::string& chunk) { out.write(chunk.data(), static_cast<std::streamsize>(chunk.size())); }, cancel);
                out.close();
                // Trust comes from the signature by an embedded key, never from the release itself.
                const Http::Response signature = Http::Request("GET", sigUrl, {}, "", {}, cancel);
                const std::string digest = ReleaseSignature::Sha256File(m_pending);
                if (binary.status < 200 || binary.status >= 300 || signature.status != 200 || digest.empty() ||
                    !ReleaseSignature::VerifyRelease(version, digest, Trim(signature.body)))
                {
                    std::error_code ec; fs::remove(m_pending, ec);
                    throw std::runtime_error("signature absente ou invalide : la mise à jour " + version + " a été rejetée");
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

fs::path Updater::PreviousPath()
{
    wchar_t current[32768];
    const DWORD n = GetModuleFileNameW(nullptr, current, static_cast<DWORD>(std::size(current)));
    if (!n || n >= std::size(current)) return {};
    return fs::path(std::wstring(current, n)).parent_path() / L"AgentChats.previous.exe";
}

bool Updater::HasPrevious() const
{
    std::error_code ec;
    const fs::path previous = PreviousPath();
    return !previous.empty() && fs::exists(previous, ec);
}

bool Updater::Rollback(void* hwnd)
{
    // Stage the kept version exactly like a downloaded one, then use the normal swap.
    const fs::path previous = PreviousPath();
    std::error_code ec;
    if (previous.empty() || !fs::exists(previous, ec) || m_busy) return false;
    fs::create_directories(m_root, ec);
    if (!fs::copy_file(previous, m_pending, fs::copy_options::overwrite_existing, ec)) return false;
    { std::lock_guard<std::mutex> lock(m_mutex); m_ready = true; m_version = "précédente"; }
    return Apply(hwnd);
}

bool Updater::ApplyPendingUpdate(const fs::path& destination, unsigned long parentPid)
{
    if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parentPid)) { WaitForSingleObject(process, 30000); CloseHandle(process); }
    wchar_t self[32768];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!n) return false;
    // Keep the version being replaced, for "Revenir à la version précédente".
    const fs::path previous = destination.parent_path() / L"AgentChats.previous.exe";
    if (_wcsicmp(fs::path(self).filename().c_str(), previous.filename().c_str()) != 0)
        CopyFileW(destination.c_str(), previous.c_str(), FALSE);
    if (!CopyFileW(self, destination.c_str(), FALSE)) return false;
    ShellExecuteW(nullptr, L"open", destination.c_str(), nullptr, destination.parent_path().c_str(), SW_SHOWNORMAL);
    MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}
