#include "Store.h"
#include "Platform.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <cwctype>

using nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Channel types and model levels
// ---------------------------------------------------------------------------

const char* ChannelTypeLabel(ChannelType type)
{
    switch (type)
    {
    case ChannelType::Analyse: return "Analyse";
    case ChannelType::Detente: return "Détente";
    case ChannelType::ConsolidationLore: return "Consolidation lore";
    case ChannelType::Code: return "Code";
    }
    return "?";
}

const char* ChannelTypeKey(ChannelType type)
{
    switch (type)
    {
    case ChannelType::Analyse: return "analyse";
    case ChannelType::Detente: return "detente";
    case ChannelType::ConsolidationLore: return "consolidation_lore";
    case ChannelType::Code: return "code";
    }
    return "detente";
}

bool ChannelTypeFromKey(const std::string& key, ChannelType& out)
{
    for (ChannelType t : {ChannelType::Analyse, ChannelType::Detente, ChannelType::ConsolidationLore, ChannelType::Code})
    {
        if (key == ChannelTypeKey(t))
        {
            out = t;
            return true;
        }
    }
    return false;
}

const char* ModelLevelLabel(ModelLevel level)
{
    switch (level)
    {
    case ModelLevel::Leger: return "Léger";
    case ModelLevel::Normal: return "Normal";
    case ModelLevel::Fort: return "Fort";
    }
    return "?";
}

const char* ModelLevelKey(ModelLevel level)
{
    switch (level)
    {
    case ModelLevel::Leger: return "leger";
    case ModelLevel::Normal: return "normal";
    case ModelLevel::Fort: return "fort";
    }
    return "normal";
}

bool ModelLevelFromKey(const std::string& key, ModelLevel& out)
{
    if (key == "standard") // name used before "Normal"
    {
        out = ModelLevel::Normal;
        return true;
    }
    for (ModelLevel l : {ModelLevel::Leger, ModelLevel::Normal, ModelLevel::Fort})
    {
        if (key == ModelLevelKey(l))
        {
            out = l;
            return true;
        }
    }
    return false;
}

ModelLevel DefaultLevelFor(ChannelType type)
{
    switch (type)
    {
    case ChannelType::Detente: return ModelLevel::Leger;
    case ChannelType::Analyse: return ModelLevel::Fort;
    case ChannelType::ConsolidationLore: return ModelLevel::Fort;
    case ChannelType::Code: return ModelLevel::Normal;
    }
    return ModelLevel::Normal;
}

namespace
{
    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    json ReadJson(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return json::parse(in);
    }

    std::set<std::wstring> MemoryTokens(const std::string& text)
    {
        static const std::set<std::wstring> stop = {
            L"a", L"au", L"aux", L"ce", L"ces", L"d", L"dans", L"de", L"des", L"du", L"elle", L"en", L"est", L"et",
            L"être", L"il", L"l", L"la", L"le", L"les", L"lui", L"on", L"par", L"pour", L"qu", L"que", L"qui",
            L"sa", L"ses", L"son", L"sur", L"un", L"une", L"utilisateur", L"utilisatrice"};
        std::set<std::wstring> out;
        std::wstring word;
        const std::wstring wide = Platform::Widen(text);
        auto flush = [&] {
            if (word.rfind(L"appel", 0) == 0)
                word = L"appel";
            else if (word.rfind(L"préfér", 0) == 0)
                word = L"préfér";
            if (word.size() >= 2 && !stop.contains(word))
                out.insert(word);
            word.clear();
        };
        for (wchar_t c : wide)
        {
            if (std::iswalnum(c))
                word.push_back(static_cast<wchar_t>(std::towlower(c)));
            else
                flush();
        }
        flush();
        return out;
    }

    bool EquivalentMemoryText(const std::string& a, const std::string& b)
    {
        const auto left = MemoryTokens(a);
        const auto right = MemoryTokens(b);
        if (left.empty() || right.empty())
            return false;
        if (left == right)
            return true;
        // Very short facts are easy to collide accidentally; require exact tokens.
        if (std::min(left.size(), right.size()) < 2)
            return false;
        size_t common = 0;
        for (const auto& token : left)
            common += right.contains(token) ? 1u : 0u;
        const double containment = static_cast<double>(common) / static_cast<double>(std::min(left.size(), right.size()));
        const double jaccard = static_cast<double>(common) / static_cast<double>(left.size() + right.size() - common);
        return containment >= 0.85 && jaccard >= 0.60;
    }

    int ScopeRank(const std::string& level)
    {
        return level == "toi" ? 3 : level == "subserver" ? 2 : 1;
    }
}

// ---------------------------------------------------------------------------
// Load / save
// ---------------------------------------------------------------------------

Store::Store(fs::path root) : m_root(std::move(root)) {}

bool Store::Load()
{
    bool ok = true;
    const fs::path workspace = m_root / "workspace.json";
    if (fs::exists(workspace))
    {
        try
        {
            const json doc = ReadJson(workspace);
            m_subservers.clear();
            for (const json& s : doc.value("subservers", json::array()))
            {
                Subserver sub;
                sub.id = s.at("id").get<std::string>();
                sub.name = s.value("name", "");
                sub.vaultPath = s.value("vaultPath", "");
                sub.lorePath = s.value("lorePath", "");
                sub.codePath = s.value("codePath", "");
                for (const json& c : s.value("channels", json::array()))
                {
                    Channel ch;
                    ch.id = c.at("id").get<std::string>();
                    ch.name = c.value("name", "");
                    if (!ChannelTypeFromKey(c.value("type", ""), ch.type))
                        ch.type = ChannelType::Detente;
                    ch.language = c.value("language", "Français");
                    if (!ModelLevelFromKey(c.value("level", ""), ch.level))
                        ch.level = DefaultLevelFor(ch.type);
                    // Keep objects alive: iterating items() of a temporary reads freed memory.
                    const json aiLevels = c.value("aiLevels", json::object());
                    for (const auto& [ai, key] : aiLevels.items())
                    {
                        ModelLevel l = ModelLevel::Normal;
                        if (key.is_string() && ModelLevelFromKey(key.get<std::string>(), l))
                            ch.aiLevels[ai] = l;
                    }
                    const json roles = c.value("roles", json::object());
                    for (const auto& [ai, r] : roles.items())
                        if (r.is_object())
                            ch.roles[ai] = {r.value("name", ""), r.value("instructions", ""), r.value("isLead", false)};
                    sub.channels.push_back(std::move(ch));
                }
                m_subservers.push_back(std::move(sub));
            }
        }
        catch (const std::exception& e)
        {
            m_lastError = std::string("Lecture de workspace.json impossible : ") + e.what();
            ok = false;
        }
    }

    try
    {
        if (fs::exists(m_root / "inbox.json"))
            for (const json& i : ReadJson(m_root / "inbox.json").value("items", json::array()))
                m_inbox.push_back({i.value("id", ""), i.value("kind", ""), i.value("subserverId", ""), i.value("channelId", ""),
                                   i.value("ai", ""), i.value("payload", ""), i.value("status", "attente"), i.value("answer", ""),
                                   i.value("createdAt", ""), i.value("decidedAt", ""), i.value("blocking", false)});
        if (fs::exists(m_root / "memory.json"))
            for (const json& m : ReadJson(m_root / "memory.json").value("notes", json::array()))
                m_memory.push_back({m.value("id", ""), m.value("level", ""), m.value("scopeId", ""), m.value("ai", ""),
                                    m.value("text", ""), m.value("createdAt", "")});
        if (fs::exists(m_root / "tasks.json"))
            for (const json& t : ReadJson(m_root / "tasks.json").value("tasks", json::array()))
                m_tasks.push_back({t.value("id", ""), t.value("channelId", ""), t.value("title", ""), t.value("assignee", ""),
                                   t.value("status", "a_faire"), t.value("createdBy", ""), t.value("updatedAt", "")});
    }
    catch (const std::exception& e)
    {
        m_lastError = std::string("Lecture des données impossible : ") + e.what();
        ok = false;
    }
    if (ok && ConsolidateMemoryDuplicates())
        SaveMemory();
    return ok;
}

bool Store::WriteFileAtomic(const fs::path& file, const std::string& text)
{
    std::string error;
    if (!Platform::WriteFileAtomic(file, text, error))
    {
        m_lastError = "Écriture impossible : " + Platform::Narrow(file.filename().wstring()) + " (" + error + ")";
        return false;
    }
    return true;
}

bool Store::SaveWorkspace()
{
    json subs = json::array();
    for (const Subserver& sub : m_subservers)
    {
        json channels = json::array();
        for (const Channel& ch : sub.channels)
        {
            json aiLevels = json::object();
            for (const auto& [ai, l] : ch.aiLevels)
                aiLevels[ai] = ModelLevelKey(l);
            json roles = json::object();
            for (const auto& [ai, r] : ch.roles)
                roles[ai] = {{"name", r.name}, {"instructions", r.instructions}, {"isLead", r.isLead}};
            channels.push_back({{"id", ch.id},
                                {"name", ch.name},
                                {"type", ChannelTypeKey(ch.type)},
                                {"language", ch.language},
                                {"level", ModelLevelKey(ch.level)},
                                {"aiLevels", aiLevels},
                                {"roles", roles}});
        }
        subs.push_back({{"id", sub.id},
                        {"name", sub.name},
                        {"vaultPath", sub.vaultPath},
                        {"lorePath", sub.lorePath},
                        {"codePath", sub.codePath},
                        {"channels", channels}});
    }
    return WriteFileAtomic(m_root / "workspace.json", json({{"version", 1}, {"subservers", subs}}).dump(2));
}

bool Store::SaveInbox()
{
    json items = json::array();
    for (const InboxItem& i : m_inbox)
        items.push_back({{"id", i.id}, {"kind", i.kind}, {"subserverId", i.subserverId}, {"channelId", i.channelId},
                         {"ai", i.ai}, {"payload", i.payload}, {"status", i.status}, {"answer", i.answer},
                         {"createdAt", i.createdAt}, {"decidedAt", i.decidedAt}, {"blocking", i.blocking}});
    return WriteFileAtomic(m_root / "inbox.json", json({{"version", 1}, {"items", items}}).dump(2));
}

bool Store::SaveMemory()
{
    json notes = json::array();
    for (const MemoryNote& m : m_memory)
        notes.push_back({{"id", m.id}, {"level", m.level}, {"scopeId", m.scopeId}, {"ai", m.ai}, {"text", m.text},
                         {"createdAt", m.createdAt}});
    return WriteFileAtomic(m_root / "memory.json", json({{"version", 1}, {"notes", notes}}).dump(2));
}

bool Store::SaveTasks()
{
    json tasks = json::array();
    for (const TaskItem& t : m_tasks)
        tasks.push_back({{"id", t.id}, {"channelId", t.channelId}, {"title", t.title}, {"assignee", t.assignee},
                         {"status", t.status}, {"createdBy", t.createdBy}, {"updatedAt", t.updatedAt}});
    return WriteFileAtomic(m_root / "tasks.json", json({{"version", 1}, {"tasks", tasks}}).dump(2));
}

// ---------------------------------------------------------------------------
// Sous-serveurs and salons
// ---------------------------------------------------------------------------

Subserver* Store::FindSubserver(const std::string& id)
{
    for (Subserver& sub : m_subservers)
        if (sub.id == id)
            return &sub;
    return nullptr;
}

Channel* Store::FindChannel(Subserver& subserver, const std::string& channelId)
{
    for (Channel& ch : subserver.channels)
        if (ch.id == channelId)
            return &ch;
    return nullptr;
}

Subserver* Store::CreateSubserver(const std::string& name, const std::string& vaultPath,
                                  const std::string& lorePath, const std::string& codePath)
{
    Subserver sub;
    sub.id = Platform::NewId();
    sub.name = name;
    sub.vaultPath = vaultPath;
    sub.lorePath = lorePath;
    sub.codePath = codePath;
    m_subservers.push_back(std::move(sub));
    if (!SaveWorkspace())
    {
        m_subservers.pop_back();
        return nullptr;
    }
    return &m_subservers.back();
}

bool Store::EnsureSelfImprovementSubserver(const std::string& codePath)
{
    constexpr const char* kId = "agentchats-self";
    if (FindSubserver(kId))
        return true;
    if (codePath.empty())
        return true; // Packaged without its source checkout: do not create a dead workspace.

    Subserver sub;
    sub.id = kId;
    sub.name = "Amélioration d’Agents Chat";
    sub.codePath = codePath;

    Channel discussion;
    discussion.id = "agentchats-feedback";
    discussion.name = "bugs-et-idées";
    discussion.type = ChannelType::Detente;
    discussion.language = "Français";
    discussion.level = ModelLevel::Leger;

    Channel implementation;
    implementation.id = "agentchats-code";
    implementation.name = "améliorations";
    implementation.type = ChannelType::Code;
    implementation.language = "Français";
    implementation.level = ModelLevel::Fort;
    implementation.roles["chatgpt"] = {"Chef d'équipe", kRolePresets[0].instructions, true};
    implementation.roles["claude"] = {"Architecte", kRolePresets[4].instructions, false};
    implementation.roles["gemini"] = {"Chercheur", kRolePresets[5].instructions, false};

    sub.channels.push_back(std::move(discussion));
    sub.channels.push_back(std::move(implementation));
    m_subservers.push_back(std::move(sub));
    if (!SaveWorkspace())
    {
        m_subservers.pop_back();
        return false;
    }
    return true;
}

bool Store::UpdateSources(Subserver& subserver, const std::string& vaultPath, const std::string& lorePath,
                          const std::string& codePath)
{
    const Subserver previous = subserver;
    subserver.vaultPath = vaultPath;
    subserver.lorePath = lorePath;
    subserver.codePath = codePath;
    if (!SaveWorkspace())
    {
        subserver = previous;
        return false;
    }
    return true;
}

Channel* Store::CreateChannel(Subserver& subserver, const std::string& name, ChannelType type,
                              const std::string& language)
{
    Channel ch;
    ch.id = Platform::NewId();
    ch.name = name;
    ch.type = type;
    ch.language = language;
    ch.level = DefaultLevelFor(type);
    // Product-wide division of labour. It remains editable per salon.
    ch.roles["chatgpt"] = {"Développeur", kRolePresets[1].instructions, type == ChannelType::Code};
    ch.roles["claude"] = {type == ChannelType::Code ? "Architecte" : "Chef d'équipe",
                           type == ChannelType::Code ? kRolePresets[4].instructions : kRolePresets[0].instructions,
                           type != ChannelType::Code};
    ch.roles["gemini"] = {"Chercheur", kRolePresets[5].instructions, false};
    subserver.channels.push_back(std::move(ch));
    if (!SaveWorkspace())
    {
        subserver.channels.pop_back();
        return nullptr;
    }
    return &subserver.channels.back();
}

namespace
{
    // Sends a folder to the Recycle Bin (recoverable), never a permanent delete.
    bool Recycle(const fs::path& dir)
    {
        std::error_code ec;
        if (!fs::exists(dir, ec))
            return true;
        std::wstring from = dir.wstring();
        from.push_back(L'\0'); // double-null terminated list
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        return SHFileOperationW(&op) == 0;
    }
}

bool Store::RenameSubserver(Subserver& subserver, const std::string& name)
{
    const std::string previous = subserver.name;
    subserver.name = name;
    if (!SaveWorkspace())
    {
        subserver.name = previous;
        return false;
    }
    return true;
}

bool Store::RenameChannel(Channel& channel, const std::string& name)
{
    const std::string previous = channel.name;
    channel.name = name;
    if (!SaveWorkspace())
    {
        channel.name = previous;
        return false;
    }
    return true;
}

bool Store::DeleteChannel(Subserver& subserver, const std::string& channelId)
{
    const auto previousChannels = subserver.channels;
    const auto previousTasks = m_tasks;
    const auto previousMemory = m_memory;
    const auto previousInbox = m_inbox;
    subserver.channels.erase(std::remove_if(subserver.channels.begin(), subserver.channels.end(),
                                            [&](const Channel& c) { return c.id == channelId; }),
                             subserver.channels.end());
    m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [&](const TaskItem& t) { return t.channelId == channelId; }), m_tasks.end());
    m_memory.erase(std::remove_if(m_memory.begin(), m_memory.end(),
                                  [&](const MemoryNote& m) { return m.level == "salon" && m.scopeId == channelId; }),
                   m_memory.end());
    for (InboxItem& i : m_inbox)
        if (i.channelId == channelId && i.status == "attente")
            i.status = "refuse";
    if (!SaveWorkspace() || !SaveTasks() || !SaveMemory() || !SaveInbox())
    {
        subserver.channels = previousChannels;
        m_tasks = previousTasks;
        m_memory = previousMemory;
        m_inbox = previousInbox;
        const std::string failure = m_lastError;
        SaveWorkspace(); SaveTasks(); SaveMemory(); SaveInbox();
        m_lastError = failure;
        return false;
    }
    m_messages.erase(subserver.id + "/" + channelId);
    m_nextSeq.erase(subserver.id + "/" + channelId);
    if (!Recycle(m_root / "subservers" / subserver.id / "channels" / channelId))
        m_lastError = "Salon supprimé, mais son dossier de conversations n'a pas pu être déplacé dans la corbeille.";
    return true;
}

bool Store::DeleteSubserver(const std::string& id)
{
    const auto previousSubservers = m_subservers;
    const auto previousTasks = m_tasks;
    const auto previousMemory = m_memory;
    const auto previousInbox = m_inbox;
    std::vector<std::string> channelIds;
    for (const Subserver& s : m_subservers)
        if (s.id == id)
            for (const Channel& c : s.channels)
                channelIds.push_back(c.id);
    m_subservers.erase(std::remove_if(m_subservers.begin(), m_subservers.end(), [&](const Subserver& s) { return s.id == id; }),
                       m_subservers.end());
    auto inSub = [&](const std::string& channelId) {
        return std::find(channelIds.begin(), channelIds.end(), channelId) != channelIds.end();
    };
    m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [&](const TaskItem& t) { return inSub(t.channelId); }), m_tasks.end());
    m_memory.erase(std::remove_if(m_memory.begin(), m_memory.end(), [&](const MemoryNote& m) {
                       return (m.level == "subserver" && m.scopeId == id) || (m.level == "salon" && inSub(m.scopeId));
                   }),
                   m_memory.end());
    for (InboxItem& i : m_inbox)
        if (i.subserverId == id && i.status == "attente")
            i.status = "refuse";
    if (!SaveWorkspace() || !SaveTasks() || !SaveMemory() || !SaveInbox())
    {
        m_subservers = previousSubservers;
        m_tasks = previousTasks;
        m_memory = previousMemory;
        m_inbox = previousInbox;
        const std::string failure = m_lastError;
        SaveWorkspace(); SaveTasks(); SaveMemory(); SaveInbox();
        m_lastError = failure;
        return false;
    }
    for (const std::string& c : channelIds)
    {
        m_messages.erase(id + "/" + c);
        m_nextSeq.erase(id + "/" + c);
    }
    if (!Recycle(m_root / "subservers" / id))
        m_lastError = "Sous-serveur supprimé, mais son dossier de conversations n'a pas pu être déplacé dans la corbeille.";
    return true;
}

bool Store::SetChannelLanguage(Channel& channel, const std::string& language)
{
    const std::string previous = channel.language;
    channel.language = language;
    if (!SaveWorkspace())
    {
        channel.language = previous;
        return false;
    }
    return true;
}

bool Store::SetChannelLevel(Channel& channel, ModelLevel level)
{
    const ModelLevel previous = channel.level;
    channel.level = level;
    if (!SaveWorkspace())
    {
        channel.level = previous;
        return false;
    }
    return true;
}

bool Store::SetAiLevel(Channel& channel, const std::string& ai, const ModelLevel* level)
{
    const auto previous = channel.aiLevels;
    if (level)
        channel.aiLevels[ai] = *level;
    else
        channel.aiLevels.erase(ai);
    if (!SaveWorkspace())
    {
        channel.aiLevels = previous;
        return false;
    }
    return true;
}

bool Store::SetRole(Channel& channel, const std::string& ai, const TeamRole* role)
{
    const auto previous = channel.roles;
    if (!role)
    {
        channel.roles.erase(ai);
    }
    else
    {
        if (role->isLead)
            for (auto it = channel.roles.begin(); it != channel.roles.end();)
                it = (it->first != ai && it->second.isLead) ? channel.roles.erase(it) : std::next(it);
        channel.roles[ai] = *role;
    }
    if (!SaveWorkspace())
    {
        channel.roles = previous;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Transcripts
// ---------------------------------------------------------------------------

std::string Store::CacheKey(const Subserver& subserver, const Channel& channel)
{
    return subserver.id + "/" + channel.id;
}

fs::path Store::TranscriptPath(const Subserver& subserver, const Channel& channel) const
{
    return m_root / "subservers" / subserver.id / "channels" / channel.id / "transcript.jsonl";
}

const std::vector<Message>& Store::Messages(const Subserver& subserver, const Channel& channel)
{
    const std::string key = CacheKey(subserver, channel);
    auto it = m_messages.find(key);
    if (it != m_messages.end())
        return it->second;

    std::vector<Message> messages;
    long long lastSeq = 0;
    std::ifstream in(TranscriptPath(subserver, channel), std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        try
        {
            const json record = json::parse(line);
            lastSeq = std::max(lastSeq, record.value("seq", 0LL));
            if (record.value("type", "") != "message")
                continue;
            const json& d = record.at("data");
            messages.push_back({d.value("id", ""), d.value("sender", ""), d.value("content", ""), d.value("timestamp", ""),
                                d.value("kind", ""), d.value("ref", "")});
        }
        catch (const std::exception&)
        {
            // A truncated last line (crash mid-write) is skipped, never fatal.
        }
    }
    m_nextSeq[key] = lastSeq + 1;
    return m_messages.emplace(key, std::move(messages)).first->second;
}

bool Store::AppendMessage(const Subserver& subserver, const Channel& channel, const std::string& sender,
                          const std::string& content, const std::string& kind, const std::string& ref)
{
    Messages(subserver, channel); // make sure history and sequence are loaded
    const std::string key = CacheKey(subserver, channel);

    Message msg{Platform::NewId(), sender, content, Platform::NowIsoUtc(), kind, ref};
    json data = {{"id", msg.id}, {"sender", msg.sender}, {"content", msg.content}, {"timestamp", msg.timestamp}};
    if (!kind.empty())
        data["kind"] = kind;
    if (!ref.empty())
        data["ref"] = ref;
    const json record = {{"seq", m_nextSeq[key]}, {"type", "message"}, {"data", data}};

    const fs::path path = TranscriptPath(subserver, channel);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << record.dump() << '\n';
    out.flush();
    if (!out)
    {
        m_lastError = "Écriture du transcript impossible : " + Platform::Narrow(path.wstring());
        return false;
    }

    ++m_nextSeq[key];
    m_messages[key].push_back(std::move(msg));
    return true;
}

// ---------------------------------------------------------------------------
// Boîte aux lettres
// ---------------------------------------------------------------------------

int Store::PendingInboxCount() const
{
    return static_cast<int>(std::count_if(m_inbox.begin(), m_inbox.end(), [](const InboxItem& i) { return i.status == "attente"; }));
}

int Store::PendingInboxCount(const std::string& subserverId) const
{
    return static_cast<int>(std::count_if(m_inbox.begin(), m_inbox.end(), [&](const InboxItem& i) {
        return i.status == "attente" && i.subserverId == subserverId;
    }));
}

const InboxItem* Store::AddInboxItem(InboxItem item)
{
    item.id = Platform::NewId();
    item.status = "attente";
    item.createdAt = Platform::NowIsoUtc();
    m_inbox.push_back(std::move(item));
    if (!SaveInbox())
    {
        m_inbox.pop_back();
        return nullptr;
    }
    return &m_inbox.back();
}

bool Store::DecideInboxItem(const std::string& id, const std::string& status, const std::string& answer)
{
    for (InboxItem& i : m_inbox)
    {
        if (i.id != id)
            continue;
        const InboxItem previous = i;
        i.status = status;
        i.answer = answer;
        i.decidedAt = Platform::NowIsoUtc();
        if (!SaveInbox())
        {
            i = previous;
            return false;
        }
        return true;
    }
    return false;
}

const InboxItem* Store::FindInboxItem(const std::string& id) const
{
    for (const InboxItem& i : m_inbox)
        if (i.id == id)
            return &i;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Mémoire
// ---------------------------------------------------------------------------

const MemoryNote* Store::AddMemory(const std::string& level, const std::string& scopeId, const std::string& ai,
                                   const std::string& text, bool* created, bool* promoted)
{
    if (created)
        *created = false;
    if (promoted)
        *promoted = false;
    for (MemoryNote& existing : m_memory)
    {
        if (!MemoryScopesOverlap(existing.level, existing.scopeId, level, scopeId) ||
            !EquivalentMemoryText(existing.text, text))
            continue;
        if (ScopeRank(level) > ScopeRank(existing.level))
        {
            const MemoryNote previous = existing;
            existing.level = level;
            existing.scopeId = scopeId;
            if (!SaveMemory())
            {
                existing = previous;
                return nullptr;
            }
            if (promoted)
                *promoted = true;
        }
        return &existing;
    }
    m_memory.push_back({Platform::NewId(), level, scopeId, ai, text, Platform::NowIsoUtc()});
    if (!SaveMemory())
    {
        m_memory.pop_back();
        return nullptr;
    }
    if (created)
        *created = true;
    return &m_memory.back();
}

bool Store::ConsolidateMemoryDuplicates()
{
    bool changed = false;
    for (size_t i = 0; i < m_memory.size(); ++i)
    {
        for (size_t j = i + 1; j < m_memory.size();)
        {
            MemoryNote& keep = m_memory[i];
            MemoryNote& candidate = m_memory[j];
            if (!MemoryScopesOverlap(keep.level, keep.scopeId, candidate.level, candidate.scopeId) ||
                !EquivalentMemoryText(keep.text, candidate.text))
            {
                ++j;
                continue;
            }
            if (ScopeRank(candidate.level) > ScopeRank(keep.level))
            {
                keep.level = candidate.level;
                keep.scopeId = candidate.scopeId;
            }
            m_memory.erase(m_memory.begin() + static_cast<std::ptrdiff_t>(j));
            changed = true;
        }
    }
    return changed;
}

bool Store::MemoryScopesOverlap(const std::string& leftLevel, const std::string& leftScope,
                                const std::string& rightLevel, const std::string& rightScope) const
{
    if (leftLevel == "toi" || rightLevel == "toi")
        return true;
    if (leftLevel == rightLevel)
        return leftScope == rightScope;

    const std::string& subserverId = leftLevel == "subserver" ? leftScope : rightScope;
    const std::string& channelId = leftLevel == "salon" ? leftScope : rightScope;
    if (subserverId.empty() || channelId.empty())
        return false;
    for (const Subserver& sub : m_subservers)
        if (sub.id == subserverId)
            return std::any_of(sub.channels.begin(), sub.channels.end(),
                               [&](const Channel& channel) { return channel.id == channelId; });
    return false;
}

bool Store::UpdateMemory(const std::string& id, const std::string& text)
{
    for (MemoryNote& m : m_memory)
    {
        if (m.id != id)
            continue;
        const std::string previous = m.text;
        m.text = text;
        if (!SaveMemory())
        {
            m.text = previous;
            return false;
        }
        return true;
    }
    return false;
}

bool Store::Forget(const std::string& id)
{
    const auto previous = m_memory;
    m_memory.erase(std::remove_if(m_memory.begin(), m_memory.end(), [&](const MemoryNote& m) { return m.id == id; }),
                   m_memory.end());
    if (!SaveMemory())
    {
        m_memory = previous;
        return false;
    }
    return true;
}

const MemoryNote* Store::FindMemory(const std::string& id) const
{
    for (const MemoryNote& m : m_memory)
        if (m.id == id)
            return &m;
    return nullptr;
}

std::vector<const MemoryNote*> Store::MemoryFor(const std::string& subserverId, const std::string& channelId) const
{
    std::vector<const MemoryNote*> out;
    for (const MemoryNote& m : m_memory)
        if (m.level == "toi" || (m.level == "subserver" && m.scopeId == subserverId) ||
            (m.level == "salon" && m.scopeId == channelId))
            out.push_back(&m);
    return out;
}

// ---------------------------------------------------------------------------
// Tâches
// ---------------------------------------------------------------------------

std::vector<TaskItem> Store::Tasks(const std::string& channelId) const
{
    std::vector<TaskItem> out;
    for (const TaskItem& t : m_tasks)
        if (t.channelId == channelId)
            out.push_back(t);
    return out;
}

const TaskItem* Store::AddTask(const std::string& channelId, const std::string& title, const std::string& assignee,
                               const std::string& createdBy)
{
    m_tasks.push_back({Platform::NewId(), channelId, title, assignee, "a_faire", createdBy, Platform::NowIsoUtc()});
    if (!SaveTasks())
    {
        m_tasks.pop_back();
        return nullptr;
    }
    return &m_tasks.back();
}

bool Store::SetTaskStatus(const std::string& channelId, const std::string& idOrTitle, const std::string& status)
{
    for (TaskItem& t : m_tasks)
    {
        if (t.channelId != channelId || (t.id != idOrTitle && Lower(t.title) != Lower(idOrTitle)))
            continue;
        const TaskItem previous = t;
        t.status = status;
        t.updatedAt = Platform::NowIsoUtc();
        if (!SaveTasks())
        {
            t = previous;
            return false;
        }
        return true;
    }
    return false;
}

bool Store::DeleteTask(const std::string& id)
{
    const auto previous = m_tasks;
    m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [&](const TaskItem& t) { return t.id == id; }), m_tasks.end());
    if (!SaveTasks())
    {
        m_tasks = previous;
        return false;
    }
    return true;
}
