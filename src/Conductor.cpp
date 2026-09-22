#include "Conductor.h"
#include "Platform.h"
#include "Settings.h"
#include "ShortContext.h"
#include "Tools.h"

#include <algorithm>
#include <deque>
#include <set>

namespace
{
    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string Trim(const std::string& s)
    {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    const char* TypeInstructions(ChannelType type)
    {
        switch (type)
        {
        case ChannelType::Detente:
            return "C'est un salon DÉTENTE : conversation décontractée entre amis sur ses histoires, ses personnages, "
                   "sa journée, n'importe quoi. Pas de critique de style ni de corrections, sauf si elle en demande. "
                   "Réponses courtes et naturelles, avec de l'humour quand ça s'y prête.";
        case ChannelType::Analyse:
            return "C'est un salon ANALYSE : tu analyses ses écrits avec franchise (structure, personnages, rythme, style, "
                   "cohérence). Critique précise, argumentée, avec des exemples tirés du texte. Propose des corrections "
                   "ciblées avec l'outil quand c'est utile.";
        case ChannelType::ConsolidationLore:
            return "C'est un salon CONSOLIDATION LORE : tu aides à tenir le canon de son univers (fiches, chronologie, "
                   "règles). Repère les contradictions, complète ce qui manque, garde la cohérence. Tu peux proposer "
                   "des modifications du lore avec l'outil.";
        case ChannelType::Code:
            return "C'est un salon CODE : tu coordonnes un travail de programmation. Tu ne modifies aucun fichier "
                   "toi-même : tu planifies, tu découpes, tu confies le travail à ton agent de code avec l'outil "
                   "travail_code, puis tu vérifies ses résultats.";
        case ChannelType::Bugs:
            return "C'est un salon BUGS GITHUB : tu qualifies les anomalies, demandes une reproduction précise, "
                   "relies chaque correction à une issue GitHub et à une tâche locale, puis tu confies l'implémentation "
                   "à l'agent de code avec travail_code. Tu vérifies tests et non-régression avant de déclarer le bug résolu.";
        }
        return "";
    }
}

const char* AiDisplayName(const std::string& id)
{
    if (id == "chatgpt") return "ChatGPT";
    if (id == "claude") return "Claude";
    if (id == "gemini") return "Gemini";
    if (id == "mistral") return "Mistral";
    if (id == "deepseek") return "DeepSeek";
    if (id == "grok") return "Grok";
    if (id == "user") return "Toi";
    return "Système";
}

Conductor::~Conductor()
{
    Stop();
    if (m_thread.joinable())
        m_thread.join();
}

bool Conductor::Start(JobInput input)
{
    if (m_busy)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancel = false;
    m_busy = true;
    m_thread = std::thread([this, in = std::move(input)]() mutable { Run(std::move(in)); });
    return true;
}

void Conductor::Stop()
{
    m_cancel = true;
}

std::vector<ConductorEvent> Conductor::Drain()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConductorEvent> out;
    out.swap(m_events);
    return out;
}

void Conductor::Post(ConductorEvent ev)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(std::move(ev));
}

std::vector<std::string> Conductor::Mentions(const std::string& text, const std::vector<std::string>& candidates)
{
    const std::string l = Lower(text);
    std::vector<std::string> found;
    auto mentioned = [&](const std::string& word) {
        size_t at = l.find("@" + word);
        while (at != std::string::npos)
        {
            const size_t end = at + 1 + word.size();
            if (end >= l.size() || !(std::isalnum(static_cast<unsigned char>(l[end])) || l[end] == '_'))
                return true;
            at = l.find("@" + word, end);
        }
        return false;
    };
    if (mentioned("all") || mentioned("tous") || mentioned("toutes") || mentioned("everyone"))
        return candidates;
    // Keep the order in which they appear in the text.
    std::vector<std::pair<size_t, std::string>> ordered;
    for (const std::string& c : candidates)
        if (mentioned(c))
            ordered.push_back({l.find("@" + c), c});
    std::sort(ordered.begin(), ordered.end());
    for (const auto& [pos, c] : ordered)
        found.push_back(c);
    return found;
}

std::string Conductor::BuildSystemPrompt(const JobInput& in, const std::string& ai) const
{
    std::string others;
    for (const std::string& m : in.members)
        if (m != ai)
            others += (others.empty() ? "" : ", ") + std::string(AiDisplayName(m)) + " (@" + m + ")";
    const std::string user = in.userName.empty() ? "l'utilisatrice" : in.userName;

    std::string s = "Tu es " + std::string(AiDisplayName(ai)) + ", une IA qui participe à un salon de discussion de groupe "
                    "(façon messagerie) avec " + user + (others.empty() ? "" : " et d'autres IA : " + others) + ".\n";
    s += "Salon « #" + in.channelName + " » du sous-serveur « " + in.subserverName + " ».\n";
    s += TypeInstructions(in.type);
    s += "\n";
    const auto role = in.roleName.find(ai);
    if (role != in.roleName.end())
    {
        s += "\nTon rôle dans l'équipe : " + role->second + ".\n";
        const auto instr = in.roleInstructions.find(ai);
        if (instr != in.roleInstructions.end() && !instr->second.empty())
            s += instr->second + "\n";
        const auto permissions = in.roles.find(ai);
        if (permissions != in.roles.end())
        {
            s += "Autorisations du rôle : ";
            s += permissions->second.globalRead ? "Global Read (effectif seulement via une CLI locale), " : "lecture configurée, ";
            s += permissions->second.canWriteFiles ? "Can Write autorisé, " : "aucune écriture de fichier, ";
            s += permissions->second.manageTasks ? "gestion des tâches" : "pas de gestion des tâches";
            s += permissions->second.manageGithub ? ", gestion GitHub.\n" : ", pas de gestion GitHub.\n";
        }
    }
    if (!in.lead.empty() && in.lead != ai)
        s += "Le chef d'équipe est " + std::string(AiDisplayName(in.lead)) + " (@" + in.lead + ") : rends-lui compte en le mentionnant quand tu as fini ou que tu bloques.\n";
    if (in.lead == ai)
        s += "Tu es le chef d'équipe : découpe la demande, confie chaque partie à un coéquipier en le mentionnant avec une consigne précise, "
             "puis intègre leurs résultats et fais un bilan court sans mentionner personne.\n";
    s += "\nRéponds en " + in.language + ". Écris comme dans une messagerie : pas de titre, pas de signature, ne te présente pas, "
         "n'écris pas ton nom en tête de message.\n";
    s += "Pour passer la parole à une autre IA, mentionne-la avec @ (par exemple @claude), seulement si tu as besoin qu'elle agisse ou réponde. "
         "Ne répète pas ce que les autres ont déjà dit : complète, nuance ou contredis. "
         "Si tu n'as rien d'utile à ajouter, réponds exactement : [PASSE]\n\n";
    s += in.toolGuide;
    if (!in.context.empty())
        s += "\n" + in.context;
    return s;
}

std::string Conductor::BuildConversation(const JobInput& in, const std::vector<Message>& history, const std::string& ai)
{
    // Short context, prepared locally: the recent thread verbatim within a budget, plus the
    // older messages most relevant to the latest request. The full history stays searchable.
    constexpr size_t kRecentBudget = 16000, kRelevantBudget = 6000, kRelevantMax = 6, kExcerpt = 800;
    auto render = [&](const Message& m) {
        std::string who = m.sender == "user" ? (in.userName.empty() ? "L'utilisatrice" : in.userName)
                                             : std::string(AiDisplayName(m.sender));
        if (m.sender == "system")
            who = "[Système]";
        return who + " (" + Platform::LocalTimeOfDay(m.timestamp) + ") : " + m.content;
    };

    std::vector<std::string> recent;
    size_t used = 0, firstRecent = history.size();
    for (size_t i = history.size(); i-- > 0;)
    {
        std::string line = render(history[i]);
        if (used + line.size() > kRecentBudget && !recent.empty())
            break;
        used += line.size();
        recent.push_back(std::move(line));
        firstRecent = i;
    }

    std::string out = "Fil du salon (du plus ancien au plus récent) :\n\n";
    if (firstRecent > 0)
    {
        std::vector<std::string> older;
        for (size_t i = 0; i < firstRecent; ++i)
            older.push_back(history[i].content);
        const std::string query = history.empty() ? std::string() : history.back().content;
        std::vector<size_t> picked = ShortContext::Rank(older, query, kRelevantMax);
        std::sort(picked.begin(), picked.end());
        std::string excerpts;
        for (size_t index : picked)
        {
            std::string line = render(history[index]);
            if (line.size() > kExcerpt)
                line = line.substr(0, kExcerpt) + " […]";
            if (excerpts.size() + line.size() > kRelevantBudget)
                break;
            excerpts += line + "\n\n";
        }
        out += "[… " + std::to_string(firstRecent) + " messages plus anciens omis ; utilise chercher_historique au besoin]\n\n";
        if (!excerpts.empty())
            out += "Extraits plus anciens liés à la dernière demande :\n\n" + excerpts + "Suite récente du fil :\n\n";
    }
    for (auto it = recent.rbegin(); it != recent.rend(); ++it)
        out += *it + "\n\n";
    out += "C'est ton tour, " + std::string(AiDisplayName(ai)) + ".";
    return out;
}

void Conductor::Run(JobInput in)
{
    auto post = [&](ConductorEvent::Kind kind, const std::string& ai, const std::string& text, TurnResult r = {}) {
        Post({kind, in.subserverId, in.channelId, ai, text, std::move(r)});
    };

    const std::string userText = in.history.empty() ? std::string() : in.history.back().content;
    static const std::vector<std::string> kAll = {"chatgpt", "claude", "gemini", "mistral", "deepseek", "grok"};

    // Who speaks first.
    std::deque<std::string> queue;
    const std::vector<std::string> named = in.forcedSpeakers.empty() ? Mentions(userText, kAll) : in.forcedSpeakers;
    for (const std::string& n : named)
    {
        if (std::find(in.members.begin(), in.members.end(), n) != in.members.end())
            queue.push_back(n);
        else
            post(ConductorEvent::Kind::Notice, n, std::string(AiDisplayName(n)) + " n'est pas disponible dans ce salon pour le moment.");
    }
    if (named.empty())
        // No @mention means addressing the whole room. `members` is already
        // filtered by channel role and live/routable presence.
        queue.assign(in.members.begin(), in.members.end());
    if (queue.empty() && named.empty())
        post(ConductorEvent::Kind::Notice, "", "Aucune IA n'est disponible (voir « Membres » et Options → Connexions).");

    std::vector<Message> history = in.history;
    int turns = 0, attempts = 0;

    while (!queue.empty() && !m_cancel && turns < in.maxTurns && attempts < in.maxTurns * 3)
    {
        const std::string ai = queue.front();
        queue.pop_front();
        ++attempts;

        const auto modelIt = in.model.find(ai);
        TurnRequest req;
        req.kind = in.backend[ai];
        req.provider = in.provider[ai];
        req.apiKey = in.apiKey[ai];
        req.model = modelIt != in.model.end() ? modelIt->second.model : "";
        req.thinking = modelIt != in.model.end() ? modelIt->second.thinking : "auto";
        req.systemPrompt = BuildSystemPrompt(in, ai);
        req.workDir = in.workDir;

        std::string toolResults;
        std::string finalText;
        bool failed = false;
        for (int iteration = 0; iteration < 4 && !m_cancel; ++iteration)
        {
            post(ConductorEvent::Kind::TurnStart, ai, "");
            req.prompt = BuildConversation(in, history, ai);
            if (!toolResults.empty())
                req.prompt += "\n\nRésultats de tes outils (seule toi les vois) :\n" + toolResults +
                              "\nMaintenant, donne ta réponse pour le salon.";
            TurnResult r = RunTurn(req, [&](const std::string& chunk) { post(ConductorEvent::Kind::Chunk, ai, chunk); }, m_cancel);
            if (!r.ok)
            {
                post(ConductorEvent::Kind::TurnFailed, ai, r.error, r);
                failed = true;
                break;
            }
            std::string visible;
            const std::vector<Tools::Call> calls = Tools::Extract(r.text, visible);
            std::string reads;
            Tools::Sources effectiveSources = in.sources;
            const auto role = in.roles.find(ai);
            const auto backend = in.backend.find(ai);
            const bool localCli = backend != in.backend.end() &&
                                  (backend->second == BackendKind::ClaudeCli || backend->second == BackendKind::CodexCli ||
                                   backend->second == BackendKind::GeminiCli);
            effectiveSources.globalRead = localCli && role != in.roles.end() && role->second.globalRead;
            for (const Tools::Call& call : calls)
            {
                if (Tools::IsReadTool(call.name))
                {
                    post(ConductorEvent::Kind::Notice, ai, std::string(AiDisplayName(ai)) + " consulte : " + call.name +
                                                               " " + call.args.dump());
                    reads += "— " + call.name + " " + call.args.dump() + "\n" + Tools::RunRead(call, effectiveSources, in.type) + "\n\n";
                }
                else
                {
                    post(ConductorEvent::Kind::Action, ai, call.args.dump());
                    // The next speakers must see what was already done in this turn.
                    std::string note = std::string(AiDisplayName(ai)) + " a utilisé l'outil " + call.name;
                    if (call.name == "retenir")
                        note = std::string(AiDisplayName(ai)) + " a retenu : " + call.args.value("texte", std::string());
                    history.push_back({Platform::NewId(), "system", note, Platform::NowIsoUtc(), "", ""});
                }
            }
            if (!reads.empty() && iteration < 3)
            {
                toolResults += reads;
                continue;
            }
            finalText = Trim(visible);
            break;
        }
        if (failed || m_cancel)
            continue;

        if (finalText.empty() || finalText == "[PASSE]" || finalText.rfind("[PASSE]", 0) == 0)
        {
            post(ConductorEvent::Kind::TurnPassed, ai, "");
            continue;
        }
        post(ConductorEvent::Kind::TurnDone, ai, finalText);
        Message m{Platform::NewId(), ai, finalText, Platform::NowIsoUtc(), "", ""};
        history.push_back(m);
        ++turns;

        // Hand-offs: the lead delegates to everyone it names, anyone else to the first one.
        std::vector<std::string> next;
        for (const std::string& n : Mentions(finalText, in.members))
            if (n != ai)
                next.push_back(n);
        if (!next.empty())
        {
            if (ai != in.lead)
                next.resize(1);
            for (const std::string& n : next)
                if (std::find(queue.begin(), queue.end(), n) == queue.end())
                    queue.push_back(n);
        }
    }

    if (m_cancel)
        post(ConductorEvent::Kind::Notice, "", "Arrêté.");
    else if (turns >= in.maxTurns && !queue.empty())
        post(ConductorEvent::Kind::Notice, "", "Limite de tours atteinte pour ce message (" + std::to_string(in.maxTurns) + ").");
    post(ConductorEvent::Kind::JobDone, "", "");
    m_busy = false;
}
