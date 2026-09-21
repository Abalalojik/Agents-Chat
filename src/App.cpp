#include "App.h"
#include "ModelCatalog.h"
#include "Platform.h"
#include "Process.h"
#include "Tools.h"

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    constexpr float kSubserverColumnWidth = 72.0f;
    constexpr float kChannelColumnWidth = 240.0f;
    constexpr float kMembersColumnWidth = 250.0f;

    constexpr ChannelType kChannelTypes[] = {
        ChannelType::Analyse, ChannelType::Detente, ChannelType::ConsolidationLore, ChannelType::Code, ChannelType::Bugs};
    constexpr ModelLevel kLevels[] = {ModelLevel::Leger, ModelLevel::Normal, ModelLevel::Fort};

    const char* kPlanAis[] = {"claude", "chatgpt", "gemini"};    // run on her plans (CLIs)
    const char* kTroupe[] = {"gemini", "mistral", "deepseek", "grok"}; // free troupe in Détente

    ImVec4 Rgb(unsigned hex, float alpha = 1.0f)
    {
        return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
    }

    // Palette (neutral dark theme)
    const ImVec4 kColServers = Rgb(0x1E1F22);
    const ImVec4 kColChannels = Rgb(0x2B2D31);
    const ImVec4 kColChat = Rgb(0x313338);
    const ImVec4 kColDim = Rgb(0x949BA4);
    const ImVec4 kColAccent = Rgb(0x5B6EE1);
    const ImVec4 kColError = Rgb(0xF23F43);
    const ImVec4 kColOk = Rgb(0x23A55A);
    const ImVec4 kColWarn = Rgb(0xF0B232);

    struct Participant
    {
        const char* id;
        const char* name;
        ImVec4 color;
    };

    const Participant kParticipants[] = {
        {"user", "Toi", Rgb(0x0284C7)},
        {"chatgpt", "ChatGPT", Rgb(0x10A37F)},
        {"claude", "Claude", Rgb(0xEA580C)},
        {"gemini", "Gemini", Rgb(0x7C3AED)},
        {"system", "Système", Rgb(0x64748B)},
        {"mistral", "Mistral", Rgb(0xFA520F)},
        {"deepseek", "DeepSeek", Rgb(0x4D6BFE)},
        {"grok", "Grok", Rgb(0xD4D4D8)},
        {"codex", "Codex", Rgb(0x10A37F)},
        {"claude-code", "Claude Code", Rgb(0xEA580C)},
        {"gemini-cli", "Antigravity CLI", Rgb(0x7C3AED)},
    };

    const Participant& ParticipantFor(const std::string& id)
    {
        for (const Participant& p : kParticipants)
            if (id == p.id)
                return p;
        return kParticipants[4];
    }

    std::string Trim(const std::string& s)
    {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    std::string GithubUrlFor(const std::string& codePath)
    {
        const std::wstring git = Process::FindOnPath(L"git.exe");
        if (git.empty() || codePath.empty())
            return {};
        std::string remote;
        std::atomic<bool> cancel{false};
        const Process::Result result = Process::Run({git, L"config", L"--get", L"remote.origin.url"},
                                                    Platform::Widen(codePath), "",
                                                    [&](const std::string& line) { if (remote.empty()) remote = Trim(line); },
                                                    cancel, {}, 10);
        if (!result.started || result.exitCode != 0 || remote.empty())
            return {};
        if (remote.rfind("git@github.com:", 0) == 0)
            remote = "https://github.com/" + remote.substr(15);
        else if (remote.rfind("ssh://git@github.com/", 0) == 0)
            remote = "https://github.com/" + remote.substr(21);
        if (remote.size() > 4 && remote.substr(remote.size() - 4) == ".git")
            remote.resize(remote.size() - 4);
        return remote.rfind("https://github.com/", 0) == 0 ? remote : std::string();
    }

    std::string Initials(const std::string& name)
    {
        std::string out;
        int count = 0;
        bool atWordStart = true;
        for (size_t i = 0; i < name.size() && count < 2;)
        {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            if (c == ' ')
                atWordStart = true;
            else if (atWordStart)
            {
                out += name.substr(i, len);
                ++count;
                atWordStart = false;
            }
            i += len;
        }
        return out.empty() ? "?" : out;
    }

    void Badge(int count)
    {
        if (count <= 0)
            return;
        const ImVec2 max = ImGui::GetItemRectMax();
        const float r = 9.0f * ImGui::GetStyle().FontScaleDpi;
        const ImVec2 center(max.x - r * 0.6f, max.y - r * 0.6f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(center, r, ImGui::GetColorU32(kColError));
        const std::string text = count > 99 ? "99+" : std::to_string(count);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), IM_COL32_WHITE, text.c_str());
    }

    bool IsDirectory(const std::string& utf8Path)
    {
        std::error_code ec;
        return !utf8Path.empty() && fs::is_directory(Platform::Widen(utf8Path), ec);
    }

    ImVec4 PresenceColor(PresenceState state)
    {
        switch (state)
        {
        case PresenceState::Online: return kColOk;
        case PresenceState::Offline: return kColError;
        case PresenceState::NotConnected: break;
        }
        return kColDim;
    }

    std::string PresenceText(const Presence& presence, bool feminine)
    {
        switch (presence.state)
        {
        case PresenceState::Online: return "en ligne";
        case PresenceState::NotConnected: return feminine ? "non branchée" : "non branché";
        case PresenceState::Offline: break;
        }
        std::string text = "hors ligne";
        if (!presence.untilIso.empty())
            text += " jusqu'à " + Platform::LocalWhen(presence.untilIso);
        if (!presence.reason.empty())
            text += " (" + presence.reason + ")";
        return text;
    }

    const char* TaskStatusLabel(const std::string& s)
    {
        if (s == "en_cours") return "en cours";
        if (s == "fait") return "fait";
        if (s == "bloque") return "bloqué";
        return "à faire";
    }

    ImVec4 TaskStatusColor(const std::string& s)
    {
        if (s == "en_cours") return kColAccent;
        if (s == "fait") return kColOk;
        if (s == "bloque") return kColError;
        return kColDim;
    }

    // Combo over a list of values; with allowOther, a field accepts any other value.
    bool ValueCombo(const char* id, std::string& value, const std::vector<std::string>& options, bool allowOther)
    {
        bool changed = false;
        if (ImGui::BeginCombo(id, value.empty() ? "—" : value.c_str()))
        {
            for (const std::string& option : options)
                if (ImGui::Selectable(option.c_str(), value == option))
                {
                    value = option;
                    changed = true;
                }
            if (allowOther)
            {
                ImGui::Separator();
                static std::string other;
                ImGui::TextColored(kColDim, "Autre modèle :");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##other", &other, ImGuiInputTextFlags_EnterReturnsTrue) && !Trim(other).empty())
                {
                    value = Trim(other);
                    other.clear();
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    std::string ReadWhole(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool WriteWhole(const fs::path& p, const std::string& text)
    {
        std::string error;
        return Platform::WriteFileAtomic(p, text, error);
    }

    const char* InboxKindLabel(const std::string& kind)
    {
        if (kind == "question") return "Question";
        if (kind == "correction") return "Correction";
        if (kind == "skill") return "Demande de skill";
        if (kind == "code") return "Travail de code";
        return "Demande";
    }
}

// ===========================================================================
// Construction, availability
// ===========================================================================

App::App(Store& store, Settings& settings, void* hwnd)
    : m_store(store), m_settings(settings), m_hwnd(hwnd), m_cloud(store.Root())
{
    if (!m_store.Subservers().empty())
    {
        const Subserver& first = m_store.Subservers().front();
        m_selectedSubserver = first.id;
        if (!first.channels.empty())
            m_selectedChannel = first.channels.front().id;
    }
    ApplyAvailability();
    RefreshAvailability();
}

App::~App()
{
    m_conductor.Stop();
    m_codeWorker.StopAll();
    if (m_availThread.joinable())
        m_availThread.join();
}

void App::RefreshAvailability()
{
    if (m_availRunning)
        return;
    if (m_availThread.joinable())
        m_availThread.join();
    m_availRunning = true;
    m_availThread = std::thread([this] {
        Availability a;
        const AgentInstall c = FindClaude(), x = FindCodex(), g = FindGemini();
        a.claudePath = c.description;
        a.codexPath = x.description;
        a.geminiPath = g.description;
        if (c.found)
        {
            const LoginStatus s = CheckLogin("claude");
            a.claude = s.loggedIn;
            a.claudeDetail = s.detail;
        }
        else
            a.claudeDetail = "non installé";
        if (x.found)
        {
            const LoginStatus s = CheckLogin("chatgpt");
            a.codex = s.loggedIn;
            a.codexDetail = s.detail;
        }
        else
            a.codexDetail = "non installé";
        if (g.found)
        {
            const LoginStatus s = CheckLogin("gemini");
            a.gemini = s.loggedIn;
            a.geminiDetail = s.detail;
        }
        else
            a.geminiDetail = "non installé";
        {
            std::lock_guard<std::mutex> lock(m_availMutex);
            m_avail = a;
        }
        m_availReady = true;
        m_availRunning = false;
    });
}

void App::ApplyAvailability()
{
    Availability a;
    {
        std::lock_guard<std::mutex> lock(m_availMutex);
        a = m_avail;
    }
    m_settings.SetAvailable("claude", "chat", a.claude);
    m_settings.SetAvailable("claude", "code", a.claude);
    m_settings.SetAvailable("chatgpt", "chat", a.codex);
    m_settings.SetAvailable("chatgpt", "code", a.codex);
    m_settings.SetAvailable("gemini", "chat", a.gemini || m_settings.HasApiKey(Provider::GeminiApi));
    // Antigravity is the research specialist. Code execution belongs to Codex;
    // keeping this tier disabled also prevents legacy Gemini CLI arguments from
    // being sent accidentally to agy.exe.
    m_settings.SetAvailable("gemini", "code", false);
    m_settings.SetAvailable("gemini", "cli", a.gemini);
    m_settings.SetAvailable("gemini", "api", m_settings.HasApiKey(Provider::GeminiApi));
    m_settings.SetAvailable("mistral", "chat", m_settings.HasApiKey(Provider::Mistral));
    m_settings.SetAvailable("deepseek", "chat",
                            m_settings.HasApiKey(Provider::DeepSeek) || m_settings.HasApiKey(Provider::OpenRouter));
    m_settings.SetAvailable("grok", "chat", m_settings.HasApiKey(Provider::XAI));
}

std::vector<std::string> App::SalonMembers(const Channel& channel, bool forDefaultSpeakers) const
{
    auto online = [&](const std::string& ai) {
        return const_cast<Settings&>(m_settings).GetPresence(ai, "chat").state == PresenceState::Online;
    };
    std::vector<std::string> out;
    if (channel.type == ChannelType::Detente)
    {
        for (const char* ai : kTroupe)
            if (online(ai))
                out.push_back(ai);
        // ChatGPT and Claude run on the plans: in Détente they speak when named, or as relief.
        const bool relief = out.empty();
        for (const char* ai : {"claude", "chatgpt"})
            if (online(ai) && (!forDefaultSpeakers || relief))
                out.push_back(ai);
        return out;
    }
    for (const char* ai : kPlanAis)
        if (online(ai))
            out.push_back(ai);
    return out;
}

// ===========================================================================
// Frame
// ===========================================================================

void App::Frame()
{
    m_cloud.Tick();
    if (m_availReady.exchange(false))
        ApplyAvailability();
    ProcessEvents();
    if (m_startQueued && !m_conductor.Busy())
    {
        m_startQueued = false;
        for (const Subserver& s : m_store.Subservers())
            for (const Channel& c : s.channels)
                if (c.id == m_queuedChannel)
                {
                    Subserver* sub = m_store.FindSubserver(s.id);
                    StartJob(*sub, *m_store.FindChannel(*sub, c.id), {});
                }
        m_queuedChannel.clear();
        m_queuedText.clear();
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(3);

    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float height = ImGui::GetContentRegionAvail().y;
    const float total = ImGui::GetContentRegionAvail().x;
    const float center = std::max(200.0f, total - (kSubserverColumnWidth + kChannelColumnWidth + kMembersColumnWidth) * scale);

    DrawSubserverColumn(height);
    ImGui::SameLine(0, 0);
    DrawChannelColumn(height);
    ImGui::SameLine(0, 0);
    DrawCenter(center, height);
    ImGui::SameLine(0, 0);
    DrawMembersColumn(height);

    DrawCreateSubserverPopup();
    DrawEditSourcesPopup();
    DrawCreateChannelPopup();
    DrawRenameDeletePopups();
    ImGui::End();

    DrawOptionsWindow();
    DrawChatOptionsWindow();
}

// ===========================================================================
// Column 1: boîte aux lettres + sous-serveurs
// ===========================================================================

void App::DrawSubserverColumn(float height)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float width = kSubserverColumnWidth * scale;
    const float button = 48.0f * scale;
    const float pad = (width - button) * 0.5f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColServers);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 12.0f * scale));
    ImGui::BeginChild("##subservers", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, button * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8.0f * scale));

    ImGui::PushStyleColor(ImGuiCol_Button, m_showInbox ? kColAccent : kColChat);
    if (ImGui::Button("@##inbox", ImVec2(button, button)))
    {
        m_showInbox = true;
        m_showTodo = false;
    }
    ImGui::PopStyleColor();
    Badge(m_store.PendingInboxCount());
    ImGui::SetItemTooltip("Boîte aux lettres");

    ImGui::Separator();

    for (const Subserver& sub : m_store.Subservers())
    {
        const bool selected = !m_showInbox && sub.id == m_selectedSubserver;
        ImGui::PushID(sub.id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? kColAccent : kColChat);
        if (ImGui::Button(Initials(sub.name).c_str(), ImVec2(button, button)))
        {
            m_showInbox = false;
            m_showTodo = false;
            if (m_selectedSubserver != sub.id)
            {
                m_selectedSubserver = sub.id;
                m_selectedChannel = sub.channels.empty() ? std::string() : sub.channels.front().id;
                m_scrollToBottom = true;
            }
        }
        ImGui::PopStyleColor();
        Badge(m_store.PendingInboxCount(sub.id));
        ImGui::SetItemTooltip("%s  (clic droit : renommer, supprimer)", sub.name.c_str());
        if (ImGui::BeginPopupContextItem("##subMenu"))
        {
            ImGui::TextColored(kColDim, "%s", sub.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Renommer…"))
            {
                m_renameSubserver = sub.id;
                m_renameChannel.clear();
                m_renameText = sub.name;
                m_openRename = true;
            }
            if (ImGui::MenuItem("Supprimer…"))
            {
                m_deleteSubserver = sub.id;
                m_deleteChannel.clear();
                m_openDelete = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kColOk);
    ImGui::PushStyleColor(ImGuiCol_Button, kColChat);
    if (ImGui::Button("+##newsub", ImVec2(button, button)))
        m_openCreateSubserver = true;
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Nouveau sous-serveur");

    const float optionsY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - button;
    if (ImGui::GetCursorPosY() < optionsY)
        ImGui::SetCursorPosY(optionsY);
    ImGui::PushStyleColor(ImGuiCol_Button, kColChat);
    if (ImGui::Button("...##appOptions", ImVec2(button, button)))
    {
        m_showOptions = true;
        m_focusOptions = true;
    }
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Options de l'application");

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ===========================================================================
// Column 2: salons
// ===========================================================================

void App::DrawChannelColumn(float height)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColChannels);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale, 12.0f * scale));
    ImGui::BeginChild("##channels", ImVec2(kChannelColumnWidth * scale, height), ImGuiChildFlags_AlwaysUseWindowPadding);

    Subserver* sub = (m_showInbox || m_showTodo) ? nullptr : m_store.FindSubserver(m_selectedSubserver);
    if (m_showInbox)
    {
        ImGui::TextUnformatted("Boîte aux lettres");
        ImGui::Separator();
        ImGui::TextColored(kColDim, "Les demandes qui te sont\nadressées, tous sous-serveurs\nconfondus.");
        ImGui::Spacing();
        ImGui::Checkbox("Voir les demandes traitées", &m_showInboxHistory);
    }
    else if (m_showTodo)
    {
        ImGui::TextUnformatted("PM & tâches");
        ImGui::Separator();
        ImGui::TextColored(kColDim, "Une vue commune de ce que\nchacun a à faire, tous\nsalons confondus.");
    }
    else if (!sub)
    {
        ImGui::TextColored(kColDim, "Aucun sous-serveur.");
    }
    else
    {
        ImGui::TextUnformatted(sub->name.c_str());
        if (ImGui::Button("...##serverOptions", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
        {
            m_renameText = sub->name;
            m_editVault = sub->vaultPath;
            m_editLore = sub->lorePath;
            m_editCode = sub->codePath;
            m_openEditSources = true;
        }
        ImGui::SetItemTooltip("Options du sous-serveur");
        ImGui::Separator();

        auto source = [](const char* label, const std::string& path) {
            ImGui::TextColored(kColDim, "%s", label);
            ImGui::SameLine();
            if (path.empty())
            {
                ImGui::TextColored(kColDim, "—");
                return;
            }
            const std::string name = Platform::Narrow(fs::path(Platform::Widen(path)).filename().wstring());
            ImGui::TextColored(IsDirectory(path) ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : kColError, "%s", name.c_str());
            ImGui::SetItemTooltip("%s", path.c_str());
        };
        source("Vault", sub->vaultPath);
        source("Lore ", sub->lorePath);
        source("Code ", sub->codePath);
        ImGui::Spacing();

        for (ChannelType type : kChannelTypes)
        {
            ImGui::PushID(static_cast<int>(type));
            ImGui::Spacing();
            ImGui::TextColored(kColDim, "%s", ChannelTypeLabel(type));
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 0));
            if (ImGui::SmallButton("+"))
            {
                m_newChannelType = static_cast<int>(type);
                m_openCreateChannel = true;
            }
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip("Nouveau salon %s", ChannelTypeLabel(type));

            for (const Channel& ch : sub->channels)
            {
                if (ch.type != type)
                    continue;
                ImGui::PushID(ch.id.c_str());
                std::string label = "# " + ch.name;
                if (m_conductor.Busy() && m_jobChannel == ch.id)
                    label += "  …";
                if (ImGui::Selectable(label.c_str(), ch.id == m_selectedChannel))
                {
                    m_selectedChannel = ch.id;
                    m_showTodo = false;
                    m_scrollToBottom = true;
                }
                if (ImGui::BeginPopupContextItem("##chanMenu"))
                {
                    ImGui::TextColored(kColDim, "# %s", ch.name.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Renommer…"))
                    {
                        m_renameSubserver = sub->id;
                        m_renameChannel = ch.id;
                        m_renameText = ch.name;
                        m_openRename = true;
                    }
                    if (ImGui::MenuItem("Supprimer…"))
                    {
                        m_deleteSubserver = sub->id;
                        m_deleteChannel = ch.id;
                        m_openDelete = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
    }

    const float barHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float barY = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - barHeight;
    if (ImGui::GetCursorPosY() < barY)
        ImGui::SetCursorPosY(barY);
    ImGui::Separator();
    if (ImGui::Button("Options du chat", ImVec2(-1, 0)))
    {
        m_showChatOptions = true;
        m_focusChatOptions = true;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ===========================================================================
// Center
// ===========================================================================

void App::DrawCenter(float width, float height)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColChat);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 12.0f * scale));
    ImGui::BeginChild("##center", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding);

    DrawErrorBar();
    const float inner = ImGui::GetContentRegionAvail().y;

    Subserver* sub = m_store.FindSubserver(m_selectedSubserver);
    Channel* ch = sub ? m_store.FindChannel(*sub, m_selectedChannel) : nullptr;
    if (m_showInbox)
        DrawInboxView();
    else if (m_showTodo)
        DrawTodoView();
    else if (sub && ch)
        DrawChannelView(*sub, *ch, ImGui::GetContentRegionAvail().x, inner);
    else
        DrawWelcome();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::DrawErrorBar()
{
    std::string error = m_store.LastError();
    if (error.empty())
        error = m_settings.LastError();
    if (error.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, kColError);
    ImGui::TextWrapped("%s", error.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("OK##err"))
        m_store.ClearError();
    ImGui::Separator();
}

void App::DrawMessage(Subserver& subserver, Channel& channel, const Message& msg)
{
    const Participant& who = ParticipantFor(msg.sender);
    ImGui::Spacing();
    if (msg.sender == "system")
    {
        ImGui::PushTextWrapPos(0.0f);
        if (msg.kind == "memory")
        {
            const bool exists = m_store.FindMemory(msg.ref) != nullptr;
            ImGui::TextColored(kColDim, "%s  %s", Platform::LocalTimeOfDay(msg.timestamp).c_str(), msg.content.c_str());
            if (exists)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Oublier"))
                    m_store.Forget(msg.ref);
            }
            else
            {
                ImGui::SameLine();
                ImGui::TextColored(kColDim, "(oublié)");
            }
        }
        else
        {
            ImGui::TextColored(kColDim, "%s  %s", Platform::LocalTimeOfDay(msg.timestamp).c_str(), msg.content.c_str());
        }
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextColored(who.color, "%s", who.name);
    const auto role = channel.roles.find(msg.sender);
    if (role != channel.roles.end())
    {
        ImGui::SameLine();
        ImGui::TextColored(kColDim, "[%s]", role->second.name.c_str());
    }
    ImGui::SameLine();
    ImGui::TextColored(kColDim, "%s", Platform::LocalTimeOfDay(msg.timestamp).c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(msg.content.c_str());
    ImGui::PopTextWrapPos();
    (void)subserver;
}

void App::DrawChannelView(Subserver& subserver, Channel& channel, float width, float height)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;

    // Header
    ImGui::Text("# %s", channel.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kColDim, "·  %s  ·  %s  ·", ChannelTypeLabel(channel.type), subserver.name.c_str());
    ImGui::SameLine();
    std::string language = channel.language;
    if (LanguagePicker("##channelLanguage", language, 150.0f * scale))
        m_store.SetChannelLanguage(channel, language);
    ImGui::SetItemTooltip("Langue dans laquelle les IA répondent dans ce salon");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f * scale);
    if (ImGui::BeginCombo("##channelLevel", ModelLevelLabel(channel.level)))
    {
        for (ModelLevel level : kLevels)
            if (ImGui::Selectable(ModelLevelLabel(level), channel.level == level))
                m_store.SetChannelLevel(channel, level);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Niveau de modèle des IA dans ce salon (réglable par IA dans « Membres »)");
    if (channel.type == ChannelType::Bugs)
    {
        ImGui::SameLine();
        if (ImGui::Button("Issues GitHub"))
        {
            const std::string repo = GithubUrlFor(subserver.codePath);
            if (repo.empty())
                m_store.SetError("Impossible de trouver un dépôt GitHub dans le dossier de code de ce sous-serveur.");
            else
                ShellExecuteW(nullptr, L"open", Platform::Widen(repo + "/issues").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("Nouveau bug"))
        {
            const std::string repo = GithubUrlFor(subserver.codePath);
            if (repo.empty())
                m_store.SetError("Impossible de trouver un dépôt GitHub dans le dossier de code de ce sous-serveur.");
            else
                ShellExecuteW(nullptr, L"open", Platform::Widen(repo + "/issues/new").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::Separator();

    const bool busyHere = m_conductor.Busy() && m_jobChannel == channel.id;
    const float composerHeight = ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const float listHeight = height - ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y - composerHeight -
                             ImGui::GetStyle().ItemSpacing.y * 2.0f - ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##messages", ImVec2(width, std::max(50.0f, listHeight)));
    const std::vector<Message>& messages = m_store.Messages(subserver, channel);
    if (messages.empty())
        ImGui::TextColored(kColDim, "Début du salon # %s.", channel.name.c_str());

    // Messages have very different heights, so no uniform-height clipper: draw the most
    // recent ones and let older ones be revealed on demand.
    size_t& shown = m_shownCount[channel.id];
    if (shown == 0)
        shown = 200;
    const size_t first = messages.size() > shown ? messages.size() - shown : 0;
    if (first > 0 && ImGui::SmallButton("Afficher des messages plus anciens"))
        shown += 200;
    for (size_t i = first; i < messages.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        DrawMessage(subserver, channel, messages[i]);
        ImGui::PopID();
    }

    // Live: what is being written right now
    if (busyHere)
    {
        for (const std::string& notice : m_notices)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kColDim, "   %s", notice.c_str());
            ImGui::PopTextWrapPos();
        }
        for (const Pending& p : m_pending)
        {
            const Participant& who = ParticipantFor(p.ai);
            ImGui::Spacing();
            ImGui::TextColored(who.color, "%s", who.name);
            ImGui::SameLine();
            ImGui::TextColored(kColDim, "écrit…");
            if (!p.text.empty())
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(p.text.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    for (const auto& [jobId, progress] : m_codeProgress)
    {
        if (m_codeJobChannel[jobId] != channel.id)
            continue;
        const std::string ai = m_codeJobAi[jobId];
        ImGui::Spacing();
        ImGui::TextColored(ParticipantFor(CodeTwinSender(ai)).color, "%s", CodeTwinOf(ai) ? CodeTwinOf(ai) : "Agent");
        ImGui::SameLine();
        ImGui::TextColored(kColDim, "travaille…");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kColDim, "%s", progress.c_str());
        ImGui::PopTextWrapPos();
    }
    if (m_scrollToBottom)
    {
        ImGui::SetScrollHereY(1.0f);
        m_scrollToBottom = false;
    }
    ImGui::EndChild();

    // Composer
    ImGui::Spacing();
    const bool otherJob = m_conductor.Busy() && !busyHere;
    const float stopWidth = (busyHere || !m_codeProgress.empty()) ? 90.0f * scale : 0.0f;
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine;
    ImGui::BeginDisabled(otherJob);
    const bool submitted = ImGui::InputTextMultiline("##composer", &m_composer,
                                                     ImVec2(width - stopWidth - (stopWidth > 0 ? 8.0f * scale : 0), composerHeight), flags);
    ImGui::EndDisabled();
    if (stopWidth > 0)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, kColError);
        if (ImGui::Button("Arrêter", ImVec2(stopWidth, composerHeight)))
        {
            m_conductor.Stop();
            m_codeWorker.StopAll();
        }
        ImGui::PopStyleColor();
    }
    if (otherJob)
        ImGui::TextColored(kColDim, "Les IA répondent dans un autre salon ; attends la fin ou arrête-les.");
    else if (busyHere)
        ImGui::TextColored(kColDim, m_queuedChannel == channel.id ? "Ton message est posté ; les IA y répondront à la fin de ce tour."
                                                                  : "Les IA répondent… Tu peux écrire : elles y répondront à la fin de ce tour.");
    else
        ImGui::TextColored(kColDim, "Entrée pour envoyer · Ctrl+Entrée pour aller à la ligne · @claude, @chatgpt, @gemini, @tous");

    if (submitted && !otherJob)
    {
        const std::string text = Trim(m_composer);
        if (!text.empty())
        {
            if (busyHere)
            {
                // Posted now, answered when the current round ends.
                m_store.AppendMessage(subserver, channel, "user", text);
                m_queuedText = text;
                m_queuedChannel = channel.id;
                m_scrollToBottom = true;
            }
            else
                SendUserMessage(subserver, channel, text);
            m_composer.clear();
        }
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void App::DrawWelcome()
{
    ImGui::Spacing();
    ImGui::TextUnformatted("Bienvenue.");
    ImGui::Spacing();
    if (m_store.Subservers().empty())
    {
        ImGui::TextWrapped("Commence par créer un sous-serveur : un univers ou un projet, avec son vault, "
                           "son dossier lore ou son dossier de code.");
        if (ImGui::Button("Créer un sous-serveur"))
            m_openCreateSubserver = true;
    }
    else
        ImGui::TextColored(kColDim, "Choisis un salon à gauche, ou crée-en un avec le « + » d'un type de salon.");
}

// ===========================================================================
// Boîte aux lettres
// ===========================================================================

void App::DrawInboxView()
{
    ImGui::TextUnformatted("Boîte aux lettres");
    ImGui::Separator();
    ImGui::BeginChild("##inboxList");
    int shown = 0;
    // Pending first (blocking before the rest), then history when asked.
    for (int pass = 0; pass < 3; ++pass)
    {
        if (pass == 2 && !m_showInboxHistory)
            break;
        // Copy ids: deciding an item may reallocate the list.
        std::vector<std::string> ids;
        for (const InboxItem& item : m_store.Inbox())
        {
            const bool pending = item.status == "attente";
            if ((pass == 0 && pending && item.blocking) || (pass == 1 && pending && !item.blocking) || (pass == 2 && !pending))
                ids.push_back(item.id);
        }
        if (pass == 2)
            std::reverse(ids.begin(), ids.end());
        if (pass == 2 && !ids.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(kColDim, "DEMANDES TRAITÉES");
        }
        for (const std::string& id : ids)
            if (const InboxItem* item = m_store.FindInboxItem(id))
            {
                InboxItem copy = *item;
                DrawInboxItem(copy);
                ++shown;
            }
    }
    if (shown == 0)
        ImGui::TextColored(kColDim, "Aucune demande en attente.");
    ImGui::EndChild();
}

void App::DrawInboxItem(const InboxItem& item)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    json args;
    try
    {
        args = json::parse(item.payload);
    }
    catch (...)
    {
    }
    Subserver* sub = m_store.FindSubserver(item.subserverId);
    Channel* ch = sub ? m_store.FindChannel(*sub, item.channelId) : nullptr;
    const bool pending = item.status == "attente";

    ImGui::PushID(item.id.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColChannels);
    ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);

    ImGui::TextColored(ParticipantFor(item.ai).color, "%s", ParticipantFor(item.ai).name);
    ImGui::SameLine();
    ImGui::Text("· %s", InboxKindLabel(item.kind));
    ImGui::SameLine();
    ImGui::TextColored(kColDim, "· %s # %s · %s", sub ? sub->name.c_str() : "?", ch ? ch->name.c_str() : "?",
                       Platform::LocalWhen(item.createdAt).c_str());
    if (!pending)
    {
        ImGui::SameLine();
        ImGui::TextColored(item.status == "accepte" ? kColOk : kColDim, "— %s", item.status == "accepte" ? "accepté" : "refusé");
    }

    ImGui::PushTextWrapPos(0.0f);
    if (item.kind == "question")
    {
        ImGui::TextUnformatted(args.value("question", std::string("?")).c_str());
        if (!pending && !item.answer.empty())
            ImGui::TextColored(kColDim, "Ta réponse : %s", item.answer.c_str());
    }
    else if (item.kind == "correction")
    {
        ImGui::TextColored(kColDim, "%s : %s", args.value("source", std::string("vault")).c_str(), args.value("chemin", std::string()).c_str());
        ImGui::TextColored(kColError, "Avant :");
        ImGui::TextColored(Rgb(0xF4A6A8), "%s", args.value("ancien", std::string("(nouvelle note)")).c_str());
        ImGui::TextColored(kColOk, "Après :");
        ImGui::TextColored(Rgb(0x9FE3B5), "%s", args.value("nouveau", std::string()).c_str());
    }
    else if (item.kind == "skill")
    {
        ImGui::Text("Skill : %s", args.value("nom", std::string("?")).c_str());
        ImGui::TextUnformatted(args.value("besoin", std::string()).c_str());
    }
    else if (item.kind == "code")
    {
        ImGui::TextColored(kColDim, "Pour %s, dans %s :", CodeTwinOf(item.ai) ? CodeTwinOf(item.ai) : "?",
                           sub ? sub->codePath.c_str() : "?");
        ImGui::TextUnformatted(args.value("instructions", std::string()).c_str());
    }
    if (!pending && !item.answer.empty() && item.kind != "question")
        ImGui::TextColored(kColDim, "%s", item.answer.c_str());
    ImGui::PopTextWrapPos();

    if (pending)
    {
        ImGui::Spacing();
        if (item.kind == "question")
        {
            std::string& answer = m_answerInput[item.id];
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##answer", "Ta réponse…", &answer);
            if (ImGui::Button("Répondre") && !Trim(answer).empty() && sub && ch)
            {
                const std::string text = "Réponse à la question de " + std::string(AiDisplayName(item.ai)) + " : " + Trim(answer);
                m_store.DecideInboxItem(item.id, "accepte", Trim(answer));
                m_answerInput.erase(item.id);
                m_store.AppendMessage(*sub, *ch, "user", text);
                if (!m_conductor.Busy())
                    StartJob(*sub, *ch, {item.ai});
            }
        }
        else
        {
            const char* acceptLabel = item.kind == "correction" ? "Appliquer" : item.kind == "code" ? "Lancer" : "Accepter";
            ImGui::PushStyleColor(ImGuiCol_Button, kColOk);
            if (ImGui::Button(acceptLabel))
            {
                if (item.kind == "correction")
                    ApplyCorrection(item);
                else if (item.kind == "code")
                    LaunchCodeWork(item);
                else if (item.kind == "skill")
                    RequestSkill(item);
            }
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (ImGui::Button(item.kind == "question" ? "Ignorer" : "Refuser"))
        {
            m_store.DecideInboxItem(item.id, "refuse", "");
            PostSystem(item.subserverId, item.channelId,
                       std::string(InboxKindLabel(item.kind)) + " de " + AiDisplayName(item.ai) + " refusée.");
        }
        ImGui::SameLine();
        if (ch && ImGui::Button("Aller au salon"))
        {
            m_showInbox = false;
            m_selectedSubserver = item.subserverId;
            m_selectedChannel = item.channelId;
            m_scrollToBottom = true;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PopID();
    (void)scale;
}

void App::ApplyCorrection(const InboxItem& item)
{
    json args = json::parse(item.payload, nullptr, false);
    Subserver* sub = m_store.FindSubserver(item.subserverId);
    if (!sub || args.is_discarded())
        return;
    const bool lore = args.value("source", "vault") == "lore";
    const fs::path root = Platform::Widen(lore ? sub->lorePath : sub->vaultPath);
    const std::string relative = args.value("chemin", "");
    const fs::path target = Tools::Confine(root, relative);
    const std::string before = args.value("ancien", "");
    const std::string after = args.value("nouveau", "");
    std::string outcome;

    const std::wstring ext = target.extension().wstring();
    Channel* ch = m_store.FindChannel(*sub, item.channelId);
    const std::string zone = target.empty() ? "" : Tools::ZoneOf(target, Platform::Widen(sub->vaultPath), Platform::Widen(sub->lorePath));
    const bool zoneAllowed = ch && ((zone == "lore" && ch->type == ChannelType::ConsolidationLore) ||
                                    (zone == "vault" && ch->type == ChannelType::Analyse));
    if (target.empty() || _wcsicmp(ext.c_str(), L".md") != 0)
        outcome = "Chemin refusé (hors du périmètre ou pas une note .md) : " + relative;
    else if (!zoneAllowed)
        outcome = "Refusé : « " + relative + " » est dans le " + (zone.empty() ? "?" : zone) +
                  ", qui ne se modifie pas depuis ce salon.";
    else
    {
        std::error_code ec;
        const bool exists = fs::exists(target, ec);
        std::string text = exists ? ReadWhole(target) : std::string();
        bool ok = false;
        if (before.empty())
        {
            if (!exists && lore) // new lore note
            {
                fs::create_directories(target.parent_path(), ec);
                text = after;
                ok = true;
            }
            else
                outcome = "Le texte « avant » est vide : rien à remplacer.";
        }
        else
        {
            const size_t at = text.find(before);
            if (at == std::string::npos)
                outcome = "Le texte « avant » n'a pas été trouvé tel quel dans la note (elle a peut-être changé).";
            else
            {
                text.replace(at, before.size(), after);
                ok = true;
            }
        }
        if (ok)
        {
            // Keep the previous version so the change can be undone.
            const fs::path backups = m_store.Root() / "backups" / item.subserverId;
            bool backupReady = true;
            if (exists)
            {
                fs::create_directories(backups, ec);
                if (ec)
                {
                    backupReady = false;
                    outcome = "Correction annulée : création du dossier de sauvegarde impossible (" + ec.message() + ").";
                }
                const std::string stamp = Platform::NowIsoUtc();
                std::string safeStamp;
                for (char c : stamp)
                    safeStamp += (c == ':' ? '-' : c);
                const fs::path backup = backups / (Platform::Widen(safeStamp + "_" + item.id + "_") + target.filename().wstring());
                if (backupReady && !fs::copy_file(target, backup, fs::copy_options::none, ec))
                {
                    backupReady = false;
                    outcome = "Correction annulée : sauvegarde de la version précédente impossible (" + ec.message() + ").";
                }
            }
            if (backupReady && WriteWhole(target, text))
                outcome = "Correction appliquée à « " + relative + " »" + (exists ? " (ancienne version gardée dans backups)." : " (nouvelle note).");
            else if (backupReady)
                outcome = "Écriture impossible dans « " + relative + " ».";
        }
    }
    const bool applied = outcome.rfind("Correction appliquée", 0) == 0;
    m_store.DecideInboxItem(item.id, applied ? "accepte" : "refuse", outcome);
    PostSystem(item.subserverId, item.channelId, outcome);
}

void App::LaunchCodeWork(const InboxItem& item)
{
    json args = json::parse(item.payload, nullptr, false);
    Subserver* sub = m_store.FindSubserver(item.subserverId);
    Channel* ch = sub ? m_store.FindChannel(*sub, item.channelId) : nullptr;
    if (!sub || !ch || args.is_discarded())
        return;
    if (m_settings.GetPresence(item.ai, "code").state != PresenceState::Online)
    {
        m_store.DecideInboxItem(item.id, "refuse", "agent de code hors ligne");
        PostSystem(item.subserverId, item.channelId,
                   std::string(CodeTwinOf(item.ai) ? CodeTwinOf(item.ai) : "L'agent") + " est hors ligne : rien n'a été envoyé.");
        return;
    }
    const ModelChoice choice = m_settings.Choice(item.ai, ch->LevelFor(item.ai));
    CodeJob job;
    job.subserverId = sub->id;
    job.channelId = ch->id;
    job.ai = item.ai;
    job.model = choice.model;
    job.thinking = choice.thinking;
    job.instructions = args.value("instructions", "");
    job.projectDir = Platform::Widen(sub->codePath);
    job.allowedCommands = m_settings.AllowedCommands();
    const std::string jobId = m_codeWorker.Start(job);
    m_codeProgress[jobId] = "démarrage…";
    m_codeJobChannel[jobId] = ch->id;
    m_codeJobAi[jobId] = item.ai;
    m_store.DecideInboxItem(item.id, "accepte", "lancé");
    PostSystem(item.subserverId, item.channelId,
               std::string(CodeTwinOf(item.ai)) + " commence le travail demandé par " + AiDisplayName(item.ai) + ".");
}

void App::RequestSkill(const InboxItem& item)
{
    json args = json::parse(item.payload, nullptr, false);
    if (args.is_discarded())
        return;
    // The Atelier: a sous-serveur dedicated to building skills, created on first use.
    Subserver* atelier = nullptr;
    for (const Subserver& s : m_store.Subservers())
        if (s.name == "Atelier")
            atelier = m_store.FindSubserver(s.id);
    if (!atelier)
    {
        const fs::path skills = m_store.Root() / "atelier" / "skills";
        std::error_code ec;
        fs::create_directories(skills, ec);
        atelier = m_store.CreateSubserver("Atelier", "", "", Platform::Narrow(skills.wstring()));
    }
    if (!atelier)
        return;
    Channel* fabrication = nullptr;
    for (Channel& c : atelier->channels)
        if (c.type == ChannelType::Code)
            fabrication = &c;
    if (!fabrication)
        fabrication = m_store.CreateChannel(*atelier, "fabrication", ChannelType::Code, "Français");
    if (!fabrication)
        return;
    m_store.DecideInboxItem(item.id, "accepte", "envoyée à l'Atelier");
    const std::string request = "@" + item.ai + " Fabrique la skill « " + args.value("nom", std::string("?")) +
                                " » : " + args.value("besoin", std::string()) +
                                "\nÉcris-la dans le dossier du projet (un dossier par skill, avec un SKILL.md qui décrit quand et comment l'utiliser) "
                                "en confiant le travail à ton agent de code.";
    m_store.AppendMessage(*atelier, *fabrication, "user", request);
    PostSystem(item.subserverId, item.channelId, "Demande de skill envoyée à l'Atelier.");
    if (!m_conductor.Busy())
        StartJob(*atelier, *fabrication, {item.ai});
}

// ===========================================================================
// Members + task board
// ===========================================================================

void App::DrawPresenceMenu(const std::string& aiId, const char* tier, const char* label, const Presence& presence)
{
    ImGui::SetItemTooltip("Clic droit : mettre en pause ou remettre en ligne");
    if (!ImGui::BeginPopupContextItem("##presence"))
        return;
    ImGui::TextColored(kColDim, "%s", label);
    ImGui::Separator();
    if (presence.state == PresenceState::Offline)
    {
        if (ImGui::MenuItem("Remettre en ligne"))
            m_settings.PutBackOnline(aiId, tier);
    }
    else if (ImGui::MenuItem("Mettre hors ligne (pause)"))
        m_settings.SetPresence(aiId, tier, {PresenceState::Offline, "pause manuelle", ""});
    ImGui::EndPopup();
}

void App::DrawRoleMenu(Channel& channel, const std::string& ai)
{
    if (!ImGui::BeginPopup("##role"))
        return;
    ImGui::TextColored(kColDim, "Rôle de %s dans # %s", AiDisplayName(ai), channel.name.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Aucun rôle", nullptr, channel.roles.count(ai) == 0))
        m_store.SetRole(channel, ai, nullptr);
    for (const RolePreset& preset : kRolePresets)
    {
        const auto it = channel.roles.find(ai);
        const bool current = it != channel.roles.end() && it->second.name == preset.name;
        if (ImGui::MenuItem(preset.name, nullptr, current))
        {
            TeamRole role{preset.name, preset.instructions, preset.isLead};
            m_store.SetRole(channel, ai, &role);
        }
    }
    ImGui::Separator();
    static std::string customName, customInstructions;
    ImGui::TextColored(kColDim, "Rôle personnalisé");
    ImGui::SetNextItemWidth(260.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##customName", "Nom du rôle", &customName);
    ImGui::InputTextMultiline("##customInstr", &customInstructions, ImVec2(260.0f * ImGui::GetStyle().FontScaleDpi, 70.0f * ImGui::GetStyle().FontScaleDpi));
    if (ImGui::Button("Donner ce rôle") && !Trim(customName).empty())
    {
        TeamRole role{Trim(customName), Trim(customInstructions), false};
        m_store.SetRole(channel, ai, &role);
        customName.clear();
        customInstructions.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::DrawMemberRow(Channel* channel, const std::string& ai, bool troupe)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const Participant& p = ParticipantFor(ai);
    const Presence chat = m_settings.GetPresence(ai, "chat");
    ImGui::PushID(ai.c_str());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float r = 5.0f * scale;
    dl->AddCircleFilled(ImVec2(pos.x + r, pos.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(PresenceColor(chat.state)));
    ImGui::Dummy(ImVec2(r * 2.0f + 6.0f * scale, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextColored(p.color, "%s", p.name);

    const float indent = r * 2.0f + 6.0f * scale + ImGui::GetStyle().ItemSpacing.x;
    ImGui::Indent(indent);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(PresenceColor(chat.state), "%s", PresenceText(chat, true).c_str());
    if (!troupe)
        if (const char* twin = CodeTwinOf(ai))
        {
            ImGui::PushID("code");
            const Presence code = m_settings.GetPresence(ai, "code");
            const bool working = m_codeWorker.IsRunning(ai);
            const ImVec4 color = working ? kColAccent : code.state == PresenceState::NotConnected ? kColDim : PresenceColor(code.state);
            ImGui::TextColored(color, "%s : %s", twin, working ? "travaille…" : PresenceText(code, false).c_str());
            ImGui::PopID();
        }
    if (channel)
    {
        const auto role = channel->roles.find(ai);
        const std::string roleLabel = role != channel->roles.end() ? "rôle : " + role->second.name : "rôle : aucun";
        ImGui::TextColored(role != channel->roles.end() && role->second.isLead ? kColWarn : kColDim,
                           "%s", roleLabel.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent(indent);
    ImGui::Spacing();
    ImGui::PopID();
}

void App::DrawMembersColumn(float height)
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColChannels);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, 12.0f * scale));
    ImGui::BeginChild("##members", ImVec2(kMembersColumnWidth * scale, height), ImGuiChildFlags_AlwaysUseWindowPadding);

    Subserver* sub = (m_showInbox || m_showTodo) ? nullptr : m_store.FindSubserver(m_selectedSubserver);
    Channel* channel = sub ? m_store.FindChannel(*sub, m_selectedChannel) : nullptr;

    ImGui::TextColored(kColDim, "MEMBRES");
    ImGui::Spacing();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float r = 5.0f * scale;
        dl->AddCircleFilled(ImVec2(pos.x + r, pos.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(kColOk));
        ImGui::Dummy(ImVec2(r * 2.0f + 6.0f * scale, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextColored(ParticipantFor("user").color, "%s",
                           m_settings.UserName().empty() ? "Toi" : m_settings.UserName().c_str());
        if (ImGui::SmallButton("Ma todo"))
            OpenTodo("user");
        ImGui::Spacing();
    }
    for (const char* ai : kPlanAis)
        DrawMemberRow(channel, ai, false);

    const bool detente = channel && channel->type == ChannelType::Detente;
    ImGui::TextColored(kColDim, detente ? "TROUPE GRATUITE" : "TROUPE GRATUITE (salons Détente)");
    ImGui::Spacing();
    for (const char* ai : {"mistral", "deepseek", "grok"})
        DrawMemberRow(detente ? channel : nullptr, ai, true);

    if (channel)
        DrawTaskBoard(*channel);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::DrawTaskBoard(Channel& channel)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kColDim, "TÂCHES DU SALON");
    const std::vector<TaskItem> tasks = m_store.Tasks(channel.id);
    if (tasks.empty())
        ImGui::TextColored(kColDim, "Aucune tâche.");
    for (const TaskItem& t : tasks)
    {
        ImGui::PushID(t.id.c_str());
        ImGui::TextColored(TaskStatusColor(t.status), "●");
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(t.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextColored(kColDim, "   %s · %s", t.assignee.empty() ? "personne" : AiDisplayName(t.assignee), TaskStatusLabel(t.status));
        if (ImGui::BeginPopupContextItem("##task"))
        {
            for (const char* s : {"a_faire", "en_cours", "fait", "bloque"})
                if (ImGui::MenuItem(TaskStatusLabel(s), nullptr, t.status == s))
                    m_store.SetTaskStatus(channel.id, t.id, s);
            ImGui::Separator();
            if (ImGui::MenuItem("Supprimer"))
                m_store.DeleteTask(t.id);
            ImGui::EndPopup();
        }
        ImGui::SetItemTooltip("Clic droit : changer le statut");
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##newTask", "Nouvelle tâche…", &m_newTaskTitle, ImGuiInputTextFlags_EnterReturnsTrue) &&
        !Trim(m_newTaskTitle).empty())
    {
        m_store.AddTask(channel.id, Trim(m_newTaskTitle), "user", "user");
        m_newTaskTitle.clear();
    }
}

void App::OpenTodo(const std::string& owner)
{
    m_todoOwner = owner;
    m_showInbox = false;
    m_showTodo = true;
    if (m_todoChannel.empty())
        m_todoChannel = m_selectedChannel;
}

void App::OpenPrivateMessage(const std::string& ai)
{
    Subserver* privateSub = nullptr;
    for (const Subserver& candidate : m_store.Subservers())
        if (candidate.name == "Messages privés")
        {
            privateSub = m_store.FindSubserver(candidate.id);
            break;
        }
    if (!privateSub)
        privateSub = m_store.CreateSubserver("Messages privés", "", "", "");
    if (!privateSub)
        return;

    Channel* pm = nullptr;
    const std::string name = AiDisplayName(ai);
    for (Channel& candidate : privateSub->channels)
        if (candidate.name == name)
        {
            pm = &candidate;
            break;
        }
    if (!pm)
        pm = m_store.CreateChannel(*privateSub, name, ChannelType::Detente, "Français");
    if (!pm)
        return;

    TeamRole role{"Interlocuteur privé", "Tu réponds directement à l'utilisatrice dans ce PM.", true};
    m_store.SetRole(*pm, ai, &role);
    m_selectedSubserver = privateSub->id;
    m_selectedChannel = pm->id;
    m_showInbox = false;
    m_showTodo = false;
    m_scrollToBottom = true;
}

void App::DrawTodoView()
{
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::Text("PM & tâches — %s", AiDisplayName(m_todoOwner));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * scale);
    if (ImGui::BeginCombo("##todoOwner", AiDisplayName(m_todoOwner)))
    {
        for (const char* owner : {"user", "chatgpt", "claude", "gemini", "mistral", "deepseek", "grok"})
            if (ImGui::Selectable(AiDisplayName(owner), m_todoOwner == owner))
                m_todoOwner = owner;
        ImGui::EndCombo();
    }
    if (m_todoOwner != "user")
    {
        ImGui::SameLine();
        if (ImGui::Button((std::string("Ouvrir le PM avec ") + AiDisplayName(m_todoOwner)).c_str()))
            OpenPrivateMessage(m_todoOwner);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Afficher les tâches terminées", &m_todoShowDone);
    ImGui::Separator();

    struct LocatedTask { TaskItem task; std::string subserverId, subserverName, channelName; };
    std::vector<LocatedTask> tasks;
    for (const TaskItem& task : m_store.AllTasks())
    {
        if (task.assignee != m_todoOwner || (!m_todoShowDone && task.status == "fait"))
            continue;
        LocatedTask located{task, {}, "Projet inconnu", "Salon inconnu"};
        for (const Subserver& sub : m_store.Subservers())
            for (const Channel& channel : sub.channels)
                if (channel.id == task.channelId)
                {
                    located.subserverId = sub.id;
                    located.subserverName = sub.name;
                    located.channelName = channel.name;
                }
        tasks.push_back(std::move(located));
    }

    ImGui::BeginChild("##todoList", ImVec2(0, ImGui::GetContentRegionAvail().y - 88.0f * scale), ImGuiChildFlags_Borders);
    if (tasks.empty())
        ImGui::TextColored(kColDim, "Aucune tâche %s.", m_todoShowDone ? "" : "active");
    for (const LocatedTask& located : tasks)
    {
        const TaskItem& task = located.task;
        ImGui::PushID(task.id.c_str());
        ImGui::TextColored(TaskStatusColor(task.status), "●");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", task.title.c_str());
        ImGui::TextColored(kColDim, "%s  ·  # %s", located.subserverName.c_str(), located.channelName.c_str());
        ImGui::SameLine();
        if (!located.subserverId.empty() && ImGui::SmallButton("Ouvrir"))
        {
            m_selectedSubserver = located.subserverId;
            m_selectedChannel = task.channelId;
            m_showTodo = false;
            m_scrollToBottom = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(115.0f * scale);
        if (ImGui::BeginCombo("##status", TaskStatusLabel(task.status)))
        {
            for (const char* status : {"a_faire", "en_cours", "fait", "bloque"})
                if (ImGui::Selectable(TaskStatusLabel(status), task.status == status))
                    m_store.SetTaskStatus(task.channelId, task.id, status);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(115.0f * scale);
        if (ImGui::BeginCombo("##assignee", AiDisplayName(task.assignee)))
        {
            for (const char* owner : {"user", "chatgpt", "claude", "gemini", "mistral", "deepseek", "grok"})
                if (ImGui::Selectable(AiDisplayName(owner), task.assignee == owner))
                    m_store.SetTaskAssignee(task.id, owner);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Supprimer"))
            m_store.DeleteTask(task.id);
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (m_todoChannel.empty())
        m_todoChannel = m_selectedChannel;
    ImGui::SetNextItemWidth(230.0f * scale);
    if (ImGui::BeginCombo("##todoChannel", "Salon de destination"))
    {
        for (const Subserver& sub : m_store.Subservers())
            for (const Channel& channel : sub.channels)
            {
                const std::string label = sub.name + " / # " + channel.name;
                if (ImGui::Selectable(label.c_str(), m_todoChannel == channel.id))
                    m_todoChannel = channel.id;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    const bool add = ImGui::InputTextWithHint("##todoNew", "Ajouter une tâche…", &m_todoNewTitle,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    if (add && !Trim(m_todoNewTitle).empty() && !m_todoChannel.empty())
    {
        m_store.AddTask(m_todoChannel, Trim(m_todoNewTitle), m_todoOwner, "user");
        m_todoNewTitle.clear();
    }
}

// ===========================================================================
// Conversation plumbing
// ===========================================================================

void App::PostSystem(const std::string& subserverId, const std::string& channelId, const std::string& text,
                     const std::string& sender, const std::string& kind, const std::string& ref)
{
    Subserver* sub = m_store.FindSubserver(subserverId);
    Channel* ch = sub ? m_store.FindChannel(*sub, channelId) : nullptr;
    if (sub && ch)
        m_store.AppendMessage(*sub, *ch, sender, text, kind, ref);
    if (channelId == m_selectedChannel)
        m_scrollToBottom = true;
}

void App::SendUserMessage(Subserver& subserver, Channel& channel, const std::string& text)
{
    if (!m_store.AppendMessage(subserver, channel, "user", text))
        return;
    m_scrollToBottom = true;
    StartJob(subserver, channel, {});
}

bool App::StartJob(Subserver& subserver, Channel& channel, const std::vector<std::string>& speakers)
{
    JobInput in;
    in.subserverId = subserver.id;
    in.channelId = channel.id;
    in.subserverName = subserver.name;
    in.channelName = channel.name;
    in.type = channel.type;
    in.language = channel.language;
    in.userName = m_settings.UserName();
    in.history = m_store.Messages(subserver, channel);
    in.members = SalonMembers(channel, false);
    in.defaultSpeakers = SalonMembers(channel, true);
    in.forcedSpeakers = speakers;
    in.maxTurns = m_settings.MaxTurns();

    // Backend per AI
    // Subscription-first routing: the signed-in CLI uses the user's existing plan.
    // A configured Gemini API key is only a fallback when the CLI is unavailable;
    // never prefer token-billed traffic silently.
    const bool geminiCli = m_settings.IsAvailable("gemini", "cli");
    const bool geminiApi = !geminiCli && m_settings.HasApiKey(Provider::GeminiApi) &&
                           m_settings.GetPresence("gemini", "api").state != PresenceState::Offline;
    for (const std::string& ai : in.members)
    {
        if (ai == "claude")
            in.backend[ai] = BackendKind::ClaudeCli;
        else if (ai == "chatgpt")
            in.backend[ai] = BackendKind::CodexCli;
        else if (ai == "gemini")
            in.backend[ai] = geminiApi ? BackendKind::GeminiApi : BackendKind::GeminiCli;
        else
        {
            in.backend[ai] = BackendKind::OpenAICompat;
            if (ai == "mistral")
                in.provider[ai] = Provider::Mistral;
            else if (ai == "grok")
                in.provider[ai] = Provider::XAI;
            else if (ai == "deepseek")
                in.provider[ai] = m_settings.HasApiKey(Provider::DeepSeek) ? Provider::DeepSeek : Provider::OpenRouter;
        }
        in.model[ai] = m_settings.Choice(ai, channel.LevelFor(ai));
        if (in.backend[ai] == BackendKind::GeminiApi)
            in.apiKey[ai] = m_settings.ApiKey(Provider::GeminiApi);
        else if (in.backend[ai] == BackendKind::OpenAICompat)
            in.apiKey[ai] = m_settings.ApiKey(in.provider[ai]);
    }

    // Roles
    in.lead = channel.Lead();
    for (const auto& [ai, role] : channel.roles)
    {
        in.roleName[ai] = role.name;
        in.roleInstructions[ai] = role.instructions;
    }

    // Tools and what they can read
    in.toolGuide = Tools::Guide(channel.type, IsDirectory(subserver.vaultPath), IsDirectory(subserver.lorePath),
                                IsDirectory(subserver.codePath));
    in.sources.vault = IsDirectory(subserver.vaultPath) ? fs::path(Platform::Widen(subserver.vaultPath)) : fs::path();
    in.sources.lore = IsDirectory(subserver.lorePath) ? fs::path(Platform::Widen(subserver.lorePath)) : fs::path();
    in.sources.dataRoot = m_store.Root();
    in.sources.subserverId = subserver.id;
    in.sources.channelId = channel.id;
    for (const Channel& c : subserver.channels)
        in.sources.channelIds.push_back(c.id);

    // Context: memory, tasks, code sessions
    std::string ctx;
    const auto notes = m_store.MemoryFor(subserver.id, channel.id);
    if (!notes.empty())
    {
        ctx += "MÉMOIRE COMMUNE (ce que l'équipe a retenu) :\n";
        for (const MemoryNote* n : notes)
            ctx += "- [" + std::string(n->level == "toi" ? "toi" : n->level == "subserver" ? "sous-serveur" : "salon") + "] " + n->text + "\n";
    }
    const std::vector<TaskItem> tasks = m_store.Tasks(channel.id);
    if (!tasks.empty())
    {
        ctx += "TÂCHES DU SALON :\n";
        for (const TaskItem& t : tasks)
            ctx += "- " + t.title + " — " + (t.assignee.empty() ? "personne" : t.assignee) + " — " + TaskStatusLabel(t.status) + "\n";
    }
    const std::vector<std::string> running = m_codeWorker.RunningSummaries();
    if (!running.empty())
    {
        ctx += "SESSIONS DE CODE EN COURS :\n";
        for (const std::string& r : running)
            ctx += "- " + r + "\n";
    }
    std::string offline;
    for (const char* ai : {"claude", "chatgpt", "gemini", "mistral", "deepseek", "grok"})
    {
        const Presence p = m_settings.GetPresence(ai, "chat");
        if (p.state == PresenceState::Offline)
            offline += std::string("- ") + AiDisplayName(ai) + " : " + PresenceText(p, true) + "\n";
    }
    if (!offline.empty())
        ctx += "IA HORS LIGNE (ne leur confie rien) :\n" + offline;
    in.context = ctx;

    const fs::path work = m_store.Root() / "runtime" / "chat";
    std::error_code ec;
    fs::create_directories(work, ec);
    in.workDir = work.wstring();

    if (!m_conductor.Start(std::move(in)))
        return false;
    m_jobChannel = channel.id;
    m_pending.clear();
    m_notices.clear();
    return true;
}

void App::ProcessEvents()
{
    for (const ConductorEvent& ev : m_conductor.Drain())
    {
        auto pendingFor = [&](const std::string& ai) -> Pending& {
            for (Pending& p : m_pending)
                if (p.ai == ai)
                    return p;
            m_pending.push_back({ai, ""});
            return m_pending.back();
        };
        auto dropPending = [&](const std::string& ai) {
            m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), [&](const Pending& p) { return p.ai == ai; }), m_pending.end());
        };
        switch (ev.kind)
        {
        case ConductorEvent::Kind::TurnStart:
            pendingFor(ev.ai).text.clear();
            break;
        case ConductorEvent::Kind::Chunk:
            pendingFor(ev.ai).text += ev.text;
            break;
        case ConductorEvent::Kind::TurnDone:
            dropPending(ev.ai);
            PostSystem(ev.subserverId, ev.channelId, ev.text, ev.ai);
            break;
        case ConductorEvent::Kind::TurnPassed:
            dropPending(ev.ai);
            break;
        case ConductorEvent::Kind::TurnFailed:
            dropPending(ev.ai);
            if (ev.result.quota)
            {
                // A free Gemini API limit only switches Gemini to its plan (Gemini CLI).
                const bool geminiApiLimit = ev.ai == "gemini" && m_settings.HasApiKey(Provider::GeminiApi) &&
                                            m_settings.GetPresence("gemini", "api").state == PresenceState::Online &&
                                            ev.result.error.find("HTTP") != std::string::npos;
                const std::string tier = geminiApiLimit ? "api" : "chat";
                m_settings.SetPresence(ev.ai, tier, {PresenceState::Offline, ev.result.reason, ev.result.untilIso});
                if (geminiApiLimit && !m_settings.IsAvailable("gemini", "cli"))
                    m_settings.SetPresence("gemini", "chat",
                                           {PresenceState::Offline, ev.result.reason, ev.result.untilIso});
                std::string line = std::string(AiDisplayName(ev.ai)) +
                                   (geminiApiLimit ? " (offre gratuite) passe sur le forfait : " : " est ") +
                                   PresenceText(m_settings.GetPresence(ev.ai, tier), true) + ".";
                if (!ev.result.error.empty())
                    line += "  " + ev.result.error.substr(0, 300);
                PostSystem(ev.subserverId, ev.channelId, line);
            }
            else if (ev.result.error != "arrêté")
                PostSystem(ev.subserverId, ev.channelId,
                           std::string(AiDisplayName(ev.ai)) + " : erreur — " + ev.result.error.substr(0, 400));
            break;
        case ConductorEvent::Kind::Notice:
            m_notices.push_back(ev.text.size() > 200 ? ev.text.substr(0, 200) + "…" : ev.text);
            break;
        case ConductorEvent::Kind::Action:
            HandleAction(ev);
            break;
        case ConductorEvent::Kind::JobDone:
            m_pending.clear();
            if (!m_queuedChannel.empty())
                m_startQueued = true;
            for (const std::string& n : m_notices)
                if (n.find(" consulte : ") == std::string::npos) // keep only notices worth keeping
                    PostSystem(ev.subserverId, ev.channelId, n);
            m_notices.clear();
            break;
        }
    }

    for (const CodeEvent& ev : m_codeWorker.Drain())
    {
        switch (ev.kind)
        {
        case CodeEvent::Kind::Progress:
        {
            std::string& p = m_codeProgress[ev.jobId];
            if (p == "démarrage…")
                p.clear();
            p += ev.text + "\n";
            // Keep the last few lines only.
            size_t lines = std::count(p.begin(), p.end(), '\n');
            while (lines > 6)
            {
                p.erase(0, p.find('\n') + 1);
                --lines;
            }
            break;
        }
        case CodeEvent::Kind::Done:
        case CodeEvent::Kind::Failed:
        {
            m_codeProgress.erase(ev.jobId);
            m_codeJobChannel.erase(ev.jobId);
            m_codeJobAi.erase(ev.jobId);
            if (ev.kind == CodeEvent::Kind::Failed && ev.result.quota)
                m_settings.SetPresence(ev.ai, "code", {PresenceState::Offline, ev.result.reason, ev.result.untilIso});
            const std::string twin = CodeTwinOf(ev.ai) ? CodeTwinOf(ev.ai) : "Agent";
            const std::string report = ev.kind == CodeEvent::Kind::Done
                                           ? ev.text
                                           : "Le travail n'a pas abouti : " + ev.text.substr(0, 500);
            PostSystem(ev.subserverId, ev.channelId, report, CodeTwinSender(ev.ai));
            // The chat AI checks its twin's work.
            Subserver* sub = m_store.FindSubserver(ev.subserverId);
            Channel* ch = sub ? m_store.FindChannel(*sub, ev.channelId) : nullptr;
            if (sub && ch && ev.kind == CodeEvent::Kind::Done && !m_conductor.Busy())
                StartJob(*sub, *ch, {ev.ai});
            (void)twin;
            break;
        }
        }
    }
}

void App::HandleAction(const ConductorEvent& ev)
{
    json args = json::parse(ev.text, nullptr, false);
    if (args.is_discarded())
        return;
    const std::string name = args.value("nom", "");
    Subserver* sub = m_store.FindSubserver(ev.subserverId);
    Channel* ch = sub ? m_store.FindChannel(*sub, ev.channelId) : nullptr;
    if (!sub || !ch)
        return;
    const std::string who = AiDisplayName(ev.ai);

    if (name == "retenir")
    {
        const std::string level = args.value("niveau", "salon");
        const std::string text = Trim(args.value("texte", ""));
        if (text.empty())
            return;
        const std::string key = level == "toi" ? "toi" : level == "sous-serveur" ? "subserver" : "salon";
        const std::string scope = key == "toi" ? "" : key == "subserver" ? sub->id : ch->id;
        bool created = false, promoted = false;
        if (const MemoryNote* note = m_store.AddMemory(key, scope, ev.ai, text, &created, &promoted))
        {
            if (!created)
            {
                const std::string detail = promoted ? " (portée élargie)" : "";
                PostSystem(sub->id, ch->id, who + " a reconnu un fait déjà présent dans la mémoire commune" + detail + ".");
                return;
            }
            const std::string noteId = note->id;
            PostSystem(sub->id, ch->id, who + " a retenu : " + text, "system", "memory", noteId);
        }
    }
    else if (name == "te_demander")
    {
        InboxItem item;
        item.kind = "question";
        item.subserverId = sub->id;
        item.channelId = ch->id;
        item.ai = ev.ai;
        item.payload = args.dump();
        item.blocking = true;
        m_store.AddInboxItem(item);
        PostSystem(sub->id, ch->id, who + " te pose une question (boîte aux lettres) : " + args.value("question", std::string()));
    }
    else if (name == "proposer_correction")
    {
        // The zone is decided by where the file really is, not by what the AI claims.
        const bool claimsLore = args.value("source", "vault") == "lore";
        const fs::path root = Platform::Widen(claimsLore ? sub->lorePath : sub->vaultPath);
        const fs::path target = Tools::Confine(root, args.value("chemin", ""));
        const std::string zone = target.empty() ? "" : Tools::ZoneOf(target, Platform::Widen(sub->vaultPath), Platform::Widen(sub->lorePath));
        const bool allowed = (zone == "lore" && ch->type == ChannelType::ConsolidationLore) ||
                             (zone == "vault" && ch->type == ChannelType::Analyse);
        if (!allowed)
        {
            const std::string what = zone.empty() ? "un fichier hors du vault et du lore" : zone == "lore" ? "le lore" : "le vault";
            PostSystem(sub->id, ch->id, who + " a voulu modifier " + what + " (« " + args.value("chemin", std::string()) +
                                            " »), ce qui n'est pas permis dans un salon " + ChannelTypeLabel(ch->type) + ".");
            return;
        }
        InboxItem item;
        item.kind = "correction";
        item.subserverId = sub->id;
        item.channelId = ch->id;
        item.ai = ev.ai;
        item.payload = args.dump();
        m_store.AddInboxItem(item);
        PostSystem(sub->id, ch->id, who + " propose une correction de « " + args.value("chemin", std::string()) + " » (boîte aux lettres).");
    }
    else if (name == "demander_skill")
    {
        InboxItem item;
        item.kind = "skill";
        item.subserverId = sub->id;
        item.channelId = ch->id;
        item.ai = ev.ai;
        item.payload = args.dump();
        m_store.AddInboxItem(item);
        PostSystem(sub->id, ch->id, who + " demande une skill : " + args.value("nom", std::string()) + " (boîte aux lettres).");
    }
    else if (name == "travail_code")
    {
        if ((ch->type != ChannelType::Code && ch->type != ChannelType::Bugs) || !CodeTwinOf(ev.ai))
        {
            PostSystem(sub->id, ch->id, who + " a voulu lancer un agent de code : ce n'est possible que dans un salon Code, "
                                            "pour ChatGPT, Claude et Gemini.");
            return;
        }
        if (!IsDirectory(sub->codePath))
        {
            PostSystem(sub->id, ch->id, "Ce sous-serveur n'a pas de dossier de code (bouton « Sources »).");
            return;
        }
        InboxItem item;
        item.kind = "code";
        item.subserverId = sub->id;
        item.channelId = ch->id;
        item.ai = ev.ai;
        item.payload = args.dump();
        m_store.AddInboxItem(item);
        PostSystem(sub->id, ch->id, who + " veut confier un travail à " + CodeTwinOf(ev.ai) + " (boîte aux lettres).");
    }
    else if (name == "tache")
    {
        const std::string action = args.value("action", "creer");
        const std::string title = Trim(args.value("titre", ""));
        if (title.empty())
            return;
        if (action == "creer")
        {
            m_store.AddTask(ch->id, title, args.value("assigne", std::string()), ev.ai);
            PostSystem(sub->id, ch->id, who + " ajoute la tâche « " + title + " »" +
                                            (args.value("assigne", std::string()).empty() ? "" : " pour " + std::string(AiDisplayName(args.value("assigne", std::string())))) + ".");
        }
        else if (m_store.SetTaskStatus(ch->id, title, args.value("statut", std::string("en_cours"))))
            PostSystem(sub->id, ch->id, who + " : « " + title + " » → " + TaskStatusLabel(args.value("statut", std::string("en_cours"))) + ".");
    }
    else
    {
        PostSystem(sub->id, ch->id, who + " a demandé un outil inconnu : " + name);
    }
}

// ===========================================================================
// Popups
// ===========================================================================

bool App::FolderField(const char* label, std::string& path, const wchar_t* pickerTitle)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-90.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputText("##path", &path);
    ImGui::SameLine();
    if (ImGui::Button("Parcourir…"))
    {
        const std::string picked = Platform::PickFolder(m_hwnd, pickerTitle);
        if (!picked.empty())
            path = picked;
    }
    const bool valid = Trim(path).empty() || IsDirectory(Trim(path));
    if (!valid)
        ImGui::TextColored(kColError, "Ce dossier n'existe pas.");
    ImGui::PopID();
    return valid;
}

bool App::LanguagePicker(const char* id, std::string& language, float width)
{
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, language.c_str()))
    {
        for (const char* lang : kLanguages)
            if (ImGui::Selectable(lang, language == lang))
            {
                language = lang;
                changed = true;
            }
        ImGui::Separator();
        static std::string other;
        ImGui::TextColored(kColDim, "Autre langue :");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##other", &other, ImGuiInputTextFlags_EnterReturnsTrue) && !Trim(other).empty())
        {
            language = Trim(other);
            other.clear();
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void App::DrawCreateSubserverPopup()
{
    if (m_openCreateSubserver)
    {
        m_newSubName.clear();
        m_newSubVault.clear();
        m_newSubLore.clear();
        m_newSubCode.clear();
        ImGui::OpenPopup("Nouveau sous-serveur");
        m_openCreateSubserver = false;
    }
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 0.0f));
    if (!ImGui::BeginPopupModal("Nouveau sous-serveur", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted("Nom");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##name", &m_newSubName);
    ImGui::Spacing();
    ImGui::TextColored(kColDim, "Sources partagées par tous les salons (facultatives, modifiables plus tard).");
    bool valid = FolderField("Vault Obsidian", m_newSubVault, L"Choisir le vault Obsidian");
    valid = FolderField("Dossier lore", m_newSubLore, L"Choisir le dossier lore") && valid;
    valid = FolderField("Dossier de code", m_newSubCode, L"Choisir le dossier de code") && valid;
    const std::string name = Trim(m_newSubName);
    ImGui::Spacing();
    ImGui::BeginDisabled(name.empty() || !valid);
    if (ImGui::Button("Créer"))
    {
        if (Subserver* sub = m_store.CreateSubserver(name, Trim(m_newSubVault), Trim(m_newSubLore), Trim(m_newSubCode)))
        {
            m_selectedSubserver = sub->id;
            m_selectedChannel.clear();
            m_showInbox = false;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Annuler"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DrawEditSourcesPopup()
{
    if (m_openEditSources)
    {
        ImGui::OpenPopup("Options du sous-serveur");
        m_openEditSources = false;
    }
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale, 0.0f));
    if (!ImGui::BeginPopupModal("Options du sous-serveur", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    Subserver* sub = m_store.FindSubserver(m_selectedSubserver);
    if (!sub)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextUnformatted("Nom");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##serverName", &m_renameText);
    ImGui::SeparatorText("Sources partagées");
    bool valid = FolderField("Vault Obsidian", m_editVault, L"Choisir le vault Obsidian");
    valid = FolderField("Dossier lore", m_editLore, L"Choisir le dossier lore") && valid;
    valid = FolderField("Dossier de code", m_editCode, L"Choisir le dossier de code") && valid;
    ImGui::BeginDisabled(!valid || Trim(m_renameText).empty());
    if (ImGui::Button("Enregistrer"))
    {
        m_store.RenameSubserver(*sub, Trim(m_renameText));
        m_store.UpdateSources(*sub, Trim(m_editVault), Trim(m_editLore), Trim(m_editCode));
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Annuler"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DrawCreateChannelPopup()
{
    if (m_openCreateChannel)
    {
        m_newChannelName.clear();
        m_newChannelLanguage = kLanguages[0];
        ImGui::OpenPopup("Nouveau salon");
        m_openCreateChannel = false;
    }
    if (!ImGui::BeginPopupModal("Nouveau salon", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    Subserver* sub = m_store.FindSubserver(m_selectedSubserver);
    if (!sub)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextUnformatted("Nom du salon");
    ImGui::SetNextItemWidth(320.0f * ImGui::GetStyle().FontScaleDpi);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##name", &m_newChannelName);
    ImGui::TextUnformatted("Type");
    for (int i = 0; i < 5; ++i)
    {
        ImGui::RadioButton(ChannelTypeLabel(kChannelTypes[i]), &m_newChannelType, i);
        if (i < 4)
            ImGui::SameLine();
    }
    ImGui::TextUnformatted("Langue des IA dans ce salon");
    LanguagePicker("##newChannelLanguage", m_newChannelLanguage, 200.0f * ImGui::GetStyle().FontScaleDpi);

    const ChannelType type = kChannelTypes[std::clamp(m_newChannelType, 0, 4)];
    const char* missing = nullptr;
    if ((type == ChannelType::Analyse || type == ChannelType::Detente) && sub->vaultPath.empty())
        missing = "Ce sous-serveur n'a pas de vault : les IA n'auront rien à lire ou corriger.";
    else if (type == ChannelType::ConsolidationLore && sub->lorePath.empty())
        missing = "Ce sous-serveur n'a pas de dossier lore.";
    else if ((type == ChannelType::Code || type == ChannelType::Bugs) && sub->codePath.empty())
        missing = "Ce sous-serveur n'a pas de dossier de code.";
    if (missing)
        ImGui::TextColored(kColWarn, "%s", missing);

    const std::string name = Trim(m_newChannelName);
    const std::string language = Trim(m_newChannelLanguage);
    ImGui::Spacing();
    ImGui::BeginDisabled(name.empty() || language.empty());
    if (ImGui::Button("Créer"))
    {
        if (Channel* ch = m_store.CreateChannel(*sub, name, type, language))
        {
            m_selectedChannel = ch->id;
            m_scrollToBottom = true;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Annuler"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::DrawRenameDeletePopups()
{
    if (m_openRename)
    {
        ImGui::OpenPopup("Renommer");
        m_openRename = false;
    }
    if (m_openDelete)
    {
        ImGui::OpenPopup("Supprimer");
        m_openDelete = false;
    }
    Subserver* sub = m_store.FindSubserver(m_renameSubserver);
    if (ImGui::BeginPopupModal("Renommer", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(320.0f * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rename", &m_renameText, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = Trim(m_renameText);
        ImGui::BeginDisabled(name.empty());
        if ((ImGui::Button("Renommer") || enter) && !name.empty() && sub)
        {
            if (m_renameChannel.empty())
                m_store.RenameSubserver(*sub, name);
            else if (Channel* ch = m_store.FindChannel(*sub, m_renameChannel))
                m_store.RenameChannel(*ch, name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Annuler"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    Subserver* del = m_store.FindSubserver(m_deleteSubserver);
    if (ImGui::BeginPopupModal("Supprimer", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        Channel* ch = del && !m_deleteChannel.empty() ? m_store.FindChannel(*del, m_deleteChannel) : nullptr;
        if (!del || (!m_deleteChannel.empty() && !ch))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        if (ch)
            ImGui::Text("Supprimer le salon « # %s » ?", ch->name.c_str());
        else
            ImGui::Text("Supprimer le sous-serveur « %s » et tous ses salons ?", del->name.c_str());
        ImGui::TextColored(kColDim, "Les conversations partent à la Corbeille de Windows (récupérables).\n"
                                    "Ton vault, ton lore et ton dossier de code ne sont pas touchés.");
        const bool busy = m_conductor.Busy() && (ch ? m_jobChannel == ch->id : true);
        if (busy)
            ImGui::TextColored(kColWarn, "Les IA sont en train de répondre : arrête-les d'abord.");
        ImGui::BeginDisabled(busy);
        ImGui::PushStyleColor(ImGuiCol_Button, kColError);
        if (ImGui::Button("Supprimer"))
        {
            if (ch)
            {
                const std::string id = ch->id;
                if (m_store.DeleteChannel(*del, id) && m_selectedChannel == id)
                    m_selectedChannel = del->channels.empty() ? std::string() : del->channels.front().id;
            }
            else
            {
                const std::string id = del->id;
                if (m_store.DeleteSubserver(id) && m_selectedSubserver == id)
                {
                    m_selectedSubserver = m_store.Subservers().empty() ? std::string() : m_store.Subservers().front().id;
                    Subserver* next = m_store.FindSubserver(m_selectedSubserver);
                    m_selectedChannel = next && !next->channels.empty() ? next->channels.front().id : std::string();
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Annuler"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ===========================================================================
// Options
// ===========================================================================

void App::DrawOptionsWindow()
{
    if (!m_showOptions)
        return;
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowSize(ImVec2(1000.0f * scale, 600.0f * scale), ImGuiCond_FirstUseEver);
    // Bring to front once when opened; doing it every frame closes every combo instantly.
    if (m_focusOptions)
    {
        ImGui::SetNextWindowFocus();
        m_focusOptions = false;
    }
    if (ImGui::Begin("Options", &m_showOptions, ImGuiWindowFlags_NoCollapse))
    {
        if (ImGui::BeginTabBar("##optionsTabs"))
        {
            if (ImGui::BeginTabItem("Général"))
            {
                DrawGeneralTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Connexions"))
            {
                DrawConnectionsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Données personnelles"))
            {
                DrawCloudTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Modèles"))
            {
                DrawModelsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mémoire"))
            {
                DrawMemoryTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void App::DrawChatOptionsWindow()
{
    if (!m_showChatOptions)
        return;
    const float scale = ImGui::GetStyle().FontScaleDpi;
    ImGui::SetNextWindowSize(ImVec2(760.0f * scale, 540.0f * scale), ImGuiCond_FirstUseEver);
    if (m_focusChatOptions)
    {
        ImGui::SetNextWindowFocus();
        m_focusChatOptions = false;
    }
    if (!ImGui::Begin("Options du chat", &m_showChatOptions, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    Subserver* sub = m_store.FindSubserver(m_selectedSubserver);
    Channel* channel = sub ? m_store.FindChannel(*sub, m_selectedChannel) : nullptr;
    if (!sub || !channel)
    {
        ImGui::TextColored(kColDim, "Sélectionne un salon pour régler ses participants.");
        ImGui::End();
        return;
    }

    ImGui::Text("# %s", channel->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kColDim, "· %s · %s", ChannelTypeLabel(channel->type), sub->name.c_str());
    ImGui::Separator();

    const float tabsWidth = 150.0f * scale;
    ImGui::BeginChild("##aiTabs", ImVec2(tabsWidth, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(kColDim, "AGENTS");
    for (const char* ai : {"chatgpt", "claude", "gemini", "mistral", "deepseek", "grok"})
    {
        const Presence presence = m_settings.GetPresence(ai, "chat");
        ImGui::PushID(ai);
        ImGui::TextColored(PresenceColor(presence.state), "●");
        ImGui::SameLine();
        if (ImGui::Selectable(AiDisplayName(ai), m_chatOptionsAi == ai))
            m_chatOptionsAi = ai;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##aiOptions", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);

    const std::string ai = m_chatOptionsAi;
    const Participant& participant = ParticipantFor(ai);
    ImGui::TextColored(participant.color, "%s", participant.name);
    ImGui::SameLine();
    if (ImGui::Button("PM / tâches"))
        OpenTodo(ai);

    auto presenceControl = [&](const char* tier, const char* title) {
        ImGui::PushID(tier);
        const Presence presence = m_settings.GetPresence(ai, tier);
        ImGui::TextUnformatted(title);
        ImGui::SameLine();
        ImGui::TextColored(PresenceColor(presence.state), "%s", PresenceText(presence, true).c_str());
        if (presence.state == PresenceState::Offline)
        {
            if (ImGui::Button("Remettre en ligne"))
                m_settings.PutBackOnline(ai, tier);
        }
        else
        {
            ImGui::BeginDisabled(presence.state == PresenceState::NotConnected);
            if (ImGui::Button("Mettre en pause"))
                m_settings.SetPresence(ai, tier, {PresenceState::Offline, "pause manuelle", ""});
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    };

    ImGui::Spacing();
    presenceControl("chat", "Conversation :");
    if (CodeTwinOf(ai))
    {
        ImGui::Spacing();
        presenceControl("code", "Agent de code :");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Niveau dans ce salon");
    const bool overridden = channel->aiLevels.count(ai) > 0;
    const std::string current = overridden ? ModelLevelLabel(channel->LevelFor(ai))
                                           : std::string("Niveau du salon (") + ModelLevelLabel(channel->level) + ")";
    ImGui::SetNextItemWidth(230.0f * scale);
    if (ImGui::BeginCombo("##chatAiLevel", current.c_str()))
    {
        if (ImGui::Selectable("Niveau du salon", !overridden))
            m_store.SetAiLevel(*channel, ai, nullptr);
        ImGui::Separator();
        for (ModelLevel level : kLevels)
            if (ImGui::Selectable(ModelLevelLabel(level), overridden && channel->LevelFor(ai) == level))
                m_store.SetAiLevel(*channel, ai, &level);
        ImGui::EndCombo();
    }
    const ModelChoice choice = m_settings.Choice(ai, channel->LevelFor(ai));
    ImGui::TextColored(kColDim, "Modèle : %s · réflexion : %s", choice.model.c_str(), choice.thinking.c_str());

    ImGui::Spacing();
    ImGui::SeparatorText("Rôle dans ce salon");
    const auto role = channel->roles.find(ai);
    ImGui::TextWrapped("%s", role != channel->roles.end() ? role->second.name.c_str() : "Aucun rôle");
    if (role != channel->roles.end() && !role->second.instructions.empty())
        ImGui::TextColored(kColDim, "%s", role->second.instructions.c_str());
    if (ImGui::Button("Modifier le rôle"))
        ImGui::OpenPopup("##role");
    DrawRoleMenu(*channel, ai);

    ImGui::EndChild();
    ImGui::End();
}

void App::DrawCloudTab()
{
    if (!m_cloudLoaded)
    {
        m_microsoftClientIdInput = m_cloud.MicrosoftClientId();
        m_googleClientIdInput = m_cloud.GoogleClientId();
        m_cloudLoaded = true;
    }
    ImGui::TextUnformatted("Hotmail / Outlook.com personnel");
    ImGui::TextWrapped("Synchronisation directe par Microsoft Graph, sans modèle et sans compte professionnel. "
                       "Crée une application publique compatible avec les comptes Microsoft personnels, puis colle son Client ID.");
    ImGui::SetNextItemWidth(520.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##msClient", "Client ID Microsoft…", &m_microsoftClientIdInput);
    ImGui::SameLine();
    if (ImGui::Button("Enregistrer##ms"))
        m_cloud.SaveMicrosoftClientId(Trim(m_microsoftClientIdInput));
    ImGui::TextColored(m_cloud.HasMicrosoftAccount() ? kColOk : kColDim, "%s", m_cloud.Status().c_str());
    if (!m_cloud.UserCode().empty())
        ImGui::TextWrapped("Code : %s — %s", m_cloud.UserCode().c_str(), m_cloud.VerificationUrl().c_str());
    ImGui::BeginDisabled(m_cloud.Busy() || Trim(m_microsoftClientIdInput).empty());
    if (ImGui::Button("Connecter le compte personnel")) m_cloud.StartMicrosoftLogin();
    ImGui::SameLine();
    if (ImGui::Button("Synchroniser maintenant")) m_cloud.SyncNow();
    ImGui::EndDisabled();
    if (m_cloud.HasMicrosoftAccount())
    {
        ImGui::SameLine();
        if (ImGui::Button("Déconnecter")) m_cloud.Disconnect();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Google Gmail / Calendar");
    ImGui::TextWrapped("OAuth Google local en lecture seule. Dans Google Cloud, crée un client OAuth de type « Application de bureau » "
                       "et active Gmail API ainsi que Google Calendar API.");
    ImGui::SetNextItemWidth(520.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##googleClient", "Client ID Google…", &m_googleClientIdInput);
    ImGui::SetNextItemWidth(520.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##googleSecret", "Secret du client Google…", &m_googleClientSecretInput,
                             ImGuiInputTextFlags_Password);
    if (ImGui::Button("Enregistrer##google") && !Trim(m_googleClientIdInput).empty() && !Trim(m_googleClientSecretInput).empty())
    {
        m_cloud.SaveGoogleCredentials(Trim(m_googleClientIdInput), Trim(m_googleClientSecretInput));
        m_googleClientSecretInput.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_cloud.Busy() || !m_cloud.HasGoogleCredentials());
    if (ImGui::Button("Connecter Google")) m_cloud.StartGoogleLogin();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(m_cloud.HasGoogleAccount() ? kColOk : kColDim,
                       m_cloud.HasGoogleAccount() ? "Google connecté" : "Google non connecté");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Banque — Synci / SimpleFIN");
    ImGui::TextWrapped("Accès strictement en lecture seule aux comptes choisis dans Synci. Le jeton d'installation est utilisé une fois ; "
                       "l'adresse d'accès permanente et le cache bancaire sont ensuite chiffrés par Windows (DPAPI). "
                       "Agents Chat ne reçoit jamais tes identifiants Caisse d'Épargne.");
    ImGui::TextColored(kColDim, "Dans Synci : Destinations > SimpleFIN > choisis les comptes > copie le jeton d'installation.");
    ImGui::SetNextItemWidth(620.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##synciToken", "Jeton d'installation SimpleFIN Synci…", &m_synciSetupTokenInput,
                             ImGuiInputTextFlags_Password);
    ImGui::BeginDisabled(m_cloud.Busy() || Trim(m_synciSetupTokenInput).empty());
    if (ImGui::Button("Connecter Synci"))
    {
        m_cloud.StartSynciConnect(Trim(m_synciSetupTokenInput));
        m_synciSetupTokenInput.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_cloud.Busy() || !m_cloud.HasSynciAccount());
    if (ImGui::Button("Synchroniser la banque")) m_cloud.SyncNow();
    ImGui::EndDisabled();
    if (m_cloud.HasSynciAccount())
    {
        ImGui::SameLine();
        if (ImGui::Button("Déconnecter Synci")) m_cloud.DisconnectSynci();
    }
    ImGui::TextColored(m_cloud.HasSynciAccount() ? kColOk : kColDim,
                       m_cloud.HasSynciAccount() ? "Synci connecté (lecture seule)" : "Synci non connecté");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Synchronisation locale automatique");
    ImGui::TextWrapped("Le Planificateur de tâches Windows lance Agents Chat silencieusement toutes les 15 minutes. "
                       "Il met uniquement les caches à jour : aucun modèle d'IA, aucun token et aucun cron facturé.");
    auto schedulerCommand = [&](bool remove) {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::vector<std::wstring> args{L"C:\\Windows\\System32\\schtasks.exe", remove ? L"/Delete" : L"/Create",
                                       L"/TN", L"AgentChats\\Synchronisation locale"};
        if (remove)
            args.push_back(L"/F");
        else
        {
            args.insert(args.end(), {L"/TR", Process::QuoteArg(executable) + L" --sync-cloud", L"/SC", L"MINUTE",
                                     L"/MO", L"15", L"/RL", L"LIMITED", L"/F"});
        }
        std::atomic<bool> cancel{false};
        const Process::Result result = Process::Run(args, fs::path(executable).parent_path().wstring(), "", {}, cancel, {}, 20);
        m_localSchedulerStatus = result.started && result.exitCode == 0
            ? (remove ? "Planification locale désactivée." : "Planification locale active : toutes les 15 minutes.")
            : "Échec du Planificateur Windows : " + (!result.error.empty() ? result.error : result.stderrText);
    };
    if (ImGui::Button("Activer le cron local")) schedulerCommand(false);
    ImGui::SameLine();
    if (ImGui::Button("Désactiver le cron local")) schedulerCommand(true);
    if (!m_localSchedulerStatus.empty()) ImGui::TextWrapped("%s", m_localSchedulerStatus.c_str());
}

void App::DrawGeneralTab()
{
    if (!m_generalLoaded)
    {
        m_userNameInput = m_settings.UserName();
        m_allowedInput = m_settings.AllowedCommands();
        m_generalLoaded = true;
    }
    ImGui::TextUnformatted("Ton prénom (pour que les IA s'adressent à toi)");
    ImGui::SetNextItemWidth(300.0f * ImGui::GetStyle().FontScaleDpi);
    if (ImGui::InputText("##userName", &m_userNameInput, ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit())
        m_settings.SetUserName(Trim(m_userNameInput));
    ImGui::Spacing();

    int turns = m_settings.MaxTurns();
    ImGui::TextUnformatted("Nombre maximal de tours d'IA par message");
    ImGui::SetNextItemWidth(300.0f * ImGui::GetStyle().FontScaleDpi);
    if (ImGui::SliderInt("##turns", &turns, 1, 30))
        m_settings.SetMaxTurns(turns);
    ImGui::Spacing();

    ImGui::TextUnformatted("Commandes que les agents de code peuvent lancer sans demander");
    ImGui::TextColored(kColDim, "Une commande exacte par ligne (ex. « npm test », « git status »). "
                                "Codex travaille de toute façon dans son bac à sable limité au dossier du projet.");
    ImGui::InputTextMultiline("##allowed", &m_allowedInput, ImVec2(-1, 120.0f * ImGui::GetStyle().FontScaleDpi));
    if (ImGui::IsItemDeactivatedAfterEdit())
        m_settings.SetAllowedCommands(m_allowedInput);
}

void App::DrawConnectionsTab()
{
    Availability a;
    {
        std::lock_guard<std::mutex> lock(m_availMutex);
        a = m_avail;
    }
    ImGui::TextWrapped("Tes forfaits passent par les agents officiels, installés sur ta machine. « Se connecter » ouvre "
                       "la procédure officielle dans une console : suis-la dans ton navigateur, puis clique sur « Vérifier ». "
                       "L'application ne voit jamais tes identifiants.");
    ImGui::Spacing();
    struct Row
    {
        const char* ai;
        const char* label;
        bool ok;
        const std::string* detail;
        const std::string* path;
    };
    const Row rows[] = {{"claude", "Claude Code (forfait Claude Pro)", a.claude, &a.claudeDetail, &a.claudePath},
                        {"chatgpt", "Codex (forfait ChatGPT)", a.codex, &a.codexDetail, &a.codexPath},
                        {"gemini", "Gemini CLI (forfait Google AI Pro)", a.gemini, &a.geminiDetail, &a.geminiPath}};
    for (const Row& row : rows)
    {
        ImGui::PushID(row.ai);
        ImGui::TextColored(ParticipantFor(row.ai).color, "%s", row.label);
        ImGui::SameLine();
        ImGui::TextColored(row.ok ? kColOk : kColWarn, "%s", m_availRunning ? "vérification…" : row.detail->c_str());
        ImGui::TextColored(kColDim, "   %s", row.path->c_str());
        if (ImGui::Button("Se connecter"))
        {
            std::string error;
            if (!OpenLoginConsole(row.ai, error))
                m_store.SetError(error);
        }
        ImGui::SameLine();
        if (ImGui::Button("Vérifier"))
            RefreshAvailability();
        ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Clés API (troupe gratuite et options)");
    ImGui::TextColored(kColDim, "Chiffrées par Windows (DPAPI) pour ton compte ; jamais réaffichées.");
    struct KeyRow
    {
        const char* provider;
        const char* label;
    };
    const KeyRow keys[] = {{Provider::GeminiApi, "Google AI Studio (Gemini gratuit)"},
                           {Provider::Mistral, "Mistral"},
                           {Provider::OpenRouter, "OpenRouter (DeepSeek gratuit)"},
                           {Provider::DeepSeek, "DeepSeek (payant)"},
                           {Provider::XAI, "xAI (Grok)"},
                           {Provider::OpenAI, "OpenAI API"}};
    for (const KeyRow& k : keys)
    {
        ImGui::PushID(k.provider);
        ImGui::Text("%s", k.label);
        ImGui::SameLine(300.0f * ImGui::GetStyle().FontScaleDpi);
        const bool has = m_settings.HasApiKey(k.provider);
        ImGui::TextColored(has ? kColOk : kColDim, has ? "enregistrée" : "aucune");
        ImGui::SameLine(420.0f * ImGui::GetStyle().FontScaleDpi);
        std::string& input = m_keyInput[k.provider];
        ImGui::SetNextItemWidth(300.0f * ImGui::GetStyle().FontScaleDpi);
        ImGui::InputTextWithHint("##key", "coller la clé…", &input, ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        if (ImGui::Button("Enregistrer") && !Trim(input).empty())
        {
            m_settings.SetApiKey(k.provider, Trim(input));
            if (std::string(k.provider) == Provider::GeminiApi)
            {
                m_settings.PutBackOnline("gemini", "api");
                m_settings.PutBackOnline("gemini", "chat");
            }
            input.clear();
            ApplyAvailability();
        }
        if (has)
        {
            ImGui::SameLine();
            if (ImGui::Button("Supprimer"))
            {
                m_settings.SetApiKey(k.provider, "");
                ApplyAvailability();
            }
        }
        ImGui::PopID();
    }
}

void App::DrawModelsTab()
{
    ImGui::TextWrapped("Pour chaque IA, le modèle et le niveau de réflexion utilisés à chaque niveau. "
                       "Un salon choisit un niveau ; tu peux aussi changer le niveau d'une IA dans « Membres ».");
    ImGui::Spacing();
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("##models", 4, flags))
        return;
    ImGui::TableSetupColumn("IA", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    for (ModelLevel level : kLevels)
        ImGui::TableSetupColumn(ModelLevelLabel(level), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (const AiCatalogEntry& entry : ModelCatalog())
    {
        ImGui::PushID(entry.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ParticipantFor(entry.id).color, "%s", entry.name);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kColDim, "%s", entry.backend);
        ImGui::PopTextWrapPos();
        for (ModelLevel level : kLevels)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(level));
            ModelChoice choice = m_settings.Choice(entry.id, level);
            bool changed = false;
            ImGui::SetNextItemWidth(-1);
            changed |= ValueCombo("##model", choice.model, entry.models, true);
            ImGui::SetItemTooltip("Modèle");
            ImGui::TextColored(kColDim, "réflexion");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            changed |= ValueCombo("##thinking", choice.thinking, entry.thinking, false);
            if (changed)
                m_settings.SetChoice(entry.id, level, choice);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::DrawMemoryTab()
{
    ImGui::TextWrapped("Ce que les IA ont retenu. « Toi » les suit partout ; « sous-serveur » et « salon » restent là où ils ont été appris.");
    ImGui::Spacing();
    if (m_store.Memory().empty())
        ImGui::TextColored(kColDim, "Rien pour l'instant.");
    std::vector<MemoryNote> notes = m_store.Memory(); // copy: edits may reallocate
    for (const MemoryNote& n : notes)
    {
        ImGui::PushID(n.id.c_str());
        std::string scope = "toi";
        if (n.level == "subserver")
        {
            Subserver* s = m_store.FindSubserver(n.scopeId);
            scope = "sous-serveur " + (s ? s->name : std::string("?"));
        }
        else if (n.level == "salon")
            scope = "salon";
        ImGui::TextColored(kColDim, "[%s] %s", scope.c_str(), AiDisplayName(n.ai));
        ImGui::SameLine();
        if (m_memoryEditId == n.id)
        {
            ImGui::SetNextItemWidth(-160.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::InputText("##edit", &m_memoryEditText);
            ImGui::SameLine();
            if (ImGui::SmallButton("OK"))
            {
                m_store.UpdateMemory(n.id, Trim(m_memoryEditText));
                m_memoryEditId.clear();
            }
        }
        else
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 160.0f * ImGui::GetStyle().FontScaleDpi);
            ImGui::TextUnformatted(n.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine();
            if (ImGui::SmallButton("Modifier"))
            {
                m_memoryEditId = n.id;
                m_memoryEditText = n.text;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Oublier"))
            m_store.Forget(n.id);
        ImGui::PopID();
    }
}
