#include "Tools.h"
#include "Platform.h"
#include "Secrets.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace Tools
{
    namespace
    {
        std::string Lower(std::string s)
        {
            // ASCII lower-case is enough for matching; accents compare as-is.
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::string ReadFileUtf8(const fs::path& p, size_t maxBytes)
        {
            std::ifstream in(p, std::ios::binary);
            std::string s;
            s.resize(maxBytes);
            in.read(s.data(), static_cast<std::streamsize>(maxBytes));
            s.resize(static_cast<size_t>(in.gcount()));
            return s;
        }

        std::string WithLineNumbers(const std::string& text, int firstLine, int lastLine)
        {
            firstLine = std::max(1, firstLine);
            if (lastLine > 0 && lastLine < firstLine)
                return {};
            std::istringstream input(text);
            std::ostringstream output;
            std::string line;
            int number = 1;
            while (std::getline(input, line))
            {
                if (number >= firstLine && (lastLine <= 0 || number <= lastLine))
                    output << number << ": " << line << '\n';
                if (lastLine > 0 && number >= lastLine)
                    break;
                ++number;
            }
            return output.str();
        }

        bool IsTextNote(const fs::path& p)
        {
            const std::wstring ext = p.extension().wstring();
            for (const wchar_t* allowed : {L".md", L".txt", L".json", L".jsonl", L".yaml", L".yml", L".toml",
                                           L".ini", L".cfg", L".xml", L".csv", L".cpp", L".c", L".h", L".hpp",
                                           L".cs", L".py", L".js", L".ts", L".tsx", L".jsx", L".java", L".rs",
                                           L".go", L".sh", L".ps1", L".bat", L".cmake", L".sql", L".html", L".css"})
                if (_wcsicmp(ext.c_str(), allowed) == 0)
                    return true;
            return p.filename() == L"CMakeLists.txt" || p.filename() == L"Dockerfile";
        }

        bool Hidden(const fs::path& relative)
        {
            for (const fs::path& part : relative)
            {
                const std::wstring s = part.wstring();
                if (!s.empty() && s[0] == L'.') // .obsidian, .git, .trash...
                    return true;
            }
            return false;
        }

        const fs::path* RootFor(const json& args, const Sources& src)
        {
            const std::string source = args.value("source", "vault");
            if (source == "lore")
                return src.lore.empty() ? nullptr : &src.lore;
            if (source == "principal")
                return src.main.empty() ? nullptr : &src.main;
            if (source.rfind("dossier_", 0) == 0)
            {
                try
                {
                    const size_t index = static_cast<size_t>(std::stoul(source.substr(8)));
                    return index < src.additional.size() ? &src.additional[index].path : nullptr;
                }
                catch (...) { return nullptr; }
            }
            return src.vault.empty() ? nullptr : &src.vault;
        }

        std::string ExclusionMode(const Sources& src, const fs::path& root, const fs::path& target)
        {
            std::error_code ec;
            const fs::path canonicalTarget = fs::weakly_canonical(target, ec);
            for (const Sources::Exclusion& rule : src.exclusions)
            {
                const fs::path candidate = rule.path.is_absolute() ? rule.path : root / rule.path;
                const fs::path canonicalRule = fs::weakly_canonical(candidate, ec);
                std::wstring base = canonicalRule.wstring();
                const std::wstring value = canonicalTarget.wstring();
                if (!base.empty() && base.back() != L'\\') base += L'\\';
                if (value == canonicalRule.wstring() || value.rfind(base, 0) == 0)
                    return rule.mode;
            }
            return {};
        }

        std::string Rel(const fs::path& root, const fs::path& p)
        {
            std::error_code ec;
            return Platform::Narrow(fs::relative(p, root, ec).generic_wstring());
        }
    }

    std::vector<Call> Extract(const std::string& reply, std::string& textWithoutCalls)
    {
        std::vector<Call> calls;
        textWithoutCalls.clear();
        size_t pos = 0;
        for (;;)
        {
            const size_t open = reply.find("```outil", pos);
            if (open == std::string::npos)
            {
                textWithoutCalls += reply.substr(pos);
                break;
            }
            const size_t bodyStart = reply.find('\n', open);
            const size_t close = bodyStart == std::string::npos ? std::string::npos : reply.find("```", bodyStart);
            if (close == std::string::npos)
            {
                textWithoutCalls += reply.substr(pos);
                break;
            }
            textWithoutCalls += reply.substr(pos, open - pos);
            const std::string body = reply.substr(bodyStart + 1, close - bodyStart - 1);
            try
            {
                const json j = json::parse(body);
                const auto add = [&](const json& one) {
                    if (one.is_object() && one.contains("nom") && one["nom"].is_string())
                        calls.push_back({one["nom"].get<std::string>(), one});
                };
                if (j.is_array())
                    for (const json& one : j)
                        add(one);
                else
                    add(j);
            }
            catch (...)
            {
                calls.push_back({"__invalide", json{{"texte", body}}});
            }
            pos = close + 3;
        }
        // Trim what is left around removed blocks.
        const auto first = textWithoutCalls.find_first_not_of(" \t\r\n");
        const auto last = textWithoutCalls.find_last_not_of(" \t\r\n");
        textWithoutCalls = first == std::string::npos ? std::string() : textWithoutCalls.substr(first, last - first + 1);
        return calls;
    }

    bool IsReadTool(const std::string& name)
    {
        return name == "lire_note" || name == "lire" || name == "chercher" || name == "lister" || name == "chercher_historique" ||
               name == "chercher_mails" || name == "lire_agenda" || name == "solde_comptes" ||
               name == "chercher_transactions" || name == "__invalide";
    }

    std::string Guide(ChannelType type, bool hasVault, bool hasLore, bool hasCode)
    {
        std::string g =
            "OUTILS. Tu n'as pas d'accès direct aux fichiers ni au terminal : pour agir, écris un bloc\n"
            "```outil\n{\"nom\": \"...\", ...}\n```\n"
            "(plusieurs blocs possibles). Les outils de lecture te renvoient leur résultat avant ta réponse finale ; "
            "les autres sont transmis à l'utilisatrice, qui les accepte ou non.\n"
            "- chercher_historique {\"requete\": \"mots\", \"portee\": \"salon\"|\"sous-serveur\"} : retrouver ce qui a été dit.\n"
            "- chercher_mails {\"requete\": \"mots\", \"non_lus\": false} : consulter le cache local des comptes mail connectés.\n"
            "- lire_agenda {\"requete\": \"\"} : consulter les événements synchronisés des agendas connectés.\n"
            "- solde_comptes {} : consulter les soldes bancaires synchronisés en lecture seule.\n"
            "- chercher_transactions {\"requete\": \"mots\", \"depenses_seulement\": false} : rechercher les opérations bancaires synchronisées.\n"
            "- retenir {\"niveau\": \"toi\"|\"sous-serveur\"|\"salon\", \"texte\": \"un fait\"} : mémoire commune. "
            "« toi » = ce qui concerne l'utilisatrice partout. Cette mémoire est partagée en temps réel par toute l'équipe : "
            "ne retiens jamais une information déjà présente dans MÉMOIRE COMMUNE ou qu'une autre IA vient de retenir.\n"
            "- te_demander {\"question\": \"...\"} : poser une question qui attend sa décision.\n"
            "- demander_autorisation {\"commande\": \"commande exacte\", \"raison\": \"pourquoi elle est nécessaire\"} : demander une autorisation ponctuelle pour le prochain travail de ton agent de code.\n"
            "- tache {\"action\": \"creer\"|\"statut\", \"titre\": \"...\", \"assigne\": \"claude\", \"statut\": \"a_faire\"|\"en_cours\"|\"fait\"|\"bloque\"} : tableau des tâches du salon.\n"
            "- demander_skill {\"nom\": \"...\", \"besoin\": \"...\"} : demander une nouvelle compétence (elle sera fabriquée dans l'Atelier si l'utilisatrice accepte).\n";
        if (type != ChannelType::Code && type != ChannelType::Bugs && (hasVault || hasLore))
        {
            g += "- lire_note {\"source\": \"vault\"|\"lore\", \"chemin\": \"dossier/Note.md\"}\n"
                 "- chercher {\"source\": \"vault\"|\"lore\", \"requete\": \"mots\"} : trouver les notes qui en parlent.\n"
                 "- lister {\"source\": \"vault\"|\"lore\", \"dossier\": \"\"} : voir les notes d'un dossier.\n";
        }
        if (hasCode)
        {
            g += "OUTILS NATIFS DU PROJET (immédiatement appelables, sans demander de compétence) :\n"
                 "- lister {\"source\": \"principal\", \"chemin\": \"\", \"recursif\": true} : arborescence avec taille ; .git et node_modules sont exclus.\n"
                 "- lire {\"source\": \"principal\", \"chemin\": \"src/fichier.cpp\", \"debut\": 1, \"fin\": 200} : texte avec numéros de ligne. `lire_note` reste un alias compatible.\n"
                 "- chercher {\"source\": \"principal\", \"motif\": \"TODO\", \"extensions\": [\".cpp\", \".h\"], \"regex\": false} : occurrences fichier:ligne:extrait. `requete` reste un alias de `motif`.\n";
            if (type == ChannelType::Code || type == ChannelType::Bugs)
                g += "- ecrire_fichier {\"source\": \"principal\", \"chemin\": \"src/fichier.cpp\", \"contenu\": \"...\"} : créer ou remplacer un fichier texte.\n"
                     "- remplacer_dans_fichier {\"source\": \"principal\", \"chemin\": \"src/fichier.cpp\", \"ancien\": \"texte exact unique\", \"nouveau\": \"...\"} : modification ciblée.\n"
                     "Les écritures exigent la permission Can Write et l'approbation de l'utilisatrice.\n";
        }
        if (type == ChannelType::Analyse && hasVault)
            g += "- proposer_correction {\"source\": \"vault\", \"chemin\": \"...\", \"ancien\": \"texte exact\", \"nouveau\": \"texte corrigé\"} : "
                 "une correction ciblée, montrée en avant/après.\n";
        if (type == ChannelType::ConsolidationLore && hasLore)
            g += "- proposer_correction {\"source\": \"lore\", \"chemin\": \"...\", \"ancien\": \"texte exact\", \"nouveau\": \"texte\"} : "
                 "modifier le lore (\"ancien\": \"\" pour créer une note).\n";
        if (type == ChannelType::Bugs)
            g += "- github {\"action\": \"creer_issue\", \"titre\": \"...\", \"corps\": \"reproduction, attendu, observé\"} | "
                 "{\"action\": \"commenter\", \"numero\": 12, \"texte\": \"...\"} | "
                 "{\"action\": \"proposer_fermeture\", \"numero\": 12, \"preuve\": \"tests qui passent, commit\"} : "
                 "tout est soumis à l'utilisatrice ; une issue n'est fermée qu'après sa vérification des tests. "
                 "Exige le rôle « Gérer les issues GitHub ». Les tâches « #N titre » du salon sont les issues synchronisées.\n";
        if (type == ChannelType::Code && hasCode)
            g += "- console {\"profil\": \"PowerShell\"|\"CMD\"|\"gcloud\", \"commande\": \"une seule commande\"} : "
                 "l'exécuter dans le dossier du projet (10 min max). Sauf commande déjà autorisée mot pour mot, "
                 "l'utilisatrice l'approuve d'abord ; la sortie (secrets masqués) te revient ici.\n";
        if ((type == ChannelType::Code || type == ChannelType::Bugs) && hasCode)
            g += "- travail_code {\"instructions\": \"consignes précises et complètes\"} : confier un travail à ton agent de code "
                 "(ton jumeau), dans le dossier du projet. Il ne démarre qu'avec l'accord de l'utilisatrice.\n";
        return g;
    }

    fs::path Confine(const fs::path& root, const std::string& relative)
    {
        if (root.empty() || relative.find('\0') != std::string::npos)
            return {};
        const fs::path rel = fs::path(Platform::Widen(relative)).lexically_normal();
        if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory())
            return {};
        for (const fs::path& part : rel)
            if (part == L"..")
                return {};
        if (Hidden(rel))
            return {};
        std::error_code ec;
        const fs::path base = fs::weakly_canonical(root, ec);
        const fs::path full = fs::weakly_canonical(base / rel, ec);
        std::wstring b = base.wstring();
        const std::wstring f = full.wstring();
        // Compare with a trailing separator: "C:\Vault2" must not pass as inside "C:\Vault".
        if (!b.empty() && b.back() != L'\\')
            b.push_back(L'\\');
        if (f.size() < b.size() || _wcsnicmp(f.c_str(), b.c_str(), b.size()) != 0)
            return {};
        return full;
    }

    fs::path ResolveWrite(const Sources& src, const std::string& source, const std::string& relative, std::string& error)
    {
        error.clear();
        const fs::path* root = nullptr;
        if (source == "principal")
            root = src.main.empty() ? nullptr : &src.main;
        else if (source.rfind("dossier_", 0) == 0)
        {
            try
            {
                const size_t index = static_cast<size_t>(std::stoul(source.substr(8)));
                if (index < src.additional.size() && src.additional[index].canWrite)
                    root = &src.additional[index].path;
                else
                    error = "Ce dossier supplémentaire n'est pas marqué Can Write.";
            }
            catch (...) { error = "Alias de dossier invalide."; }
        }
        else
            error = "L'écriture n'est permise que dans le dossier principal ou un dossier Can Write.";
        if (!root)
        {
            if (error.empty()) error = "Dossier d'écriture introuvable.";
            return {};
        }
        const fs::path target = Confine(*root, relative);
        if (target.empty())
        {
            error = "Chemin refusé : il sort du dossier autorisé ou vise un dossier caché.";
            return {};
        }
        if (!IsTextNote(target))
        {
            error = "Type de fichier refusé : seuls les fichiers texte et de code sont modifiables.";
            return {};
        }
        if (!ExclusionMode(src, *root, target).empty())
        {
            error = "Chemin refusé par une règle d'exclusion du sous-serveur.";
            return {};
        }
        return target;
    }

    namespace
    {
        bool IsUnder(const fs::path& root, const fs::path& file)
        {
            if (root.empty())
                return false;
            std::error_code ec;
            std::wstring r = fs::weakly_canonical(root, ec).wstring();
            const std::wstring f = fs::weakly_canonical(file, ec).wstring();
            if (!r.empty() && r.back() != L'\\')
                r.push_back(L'\\');
            return f.size() >= r.size() && _wcsnicmp(f.c_str(), r.c_str(), r.size()) == 0;
        }

        size_t Depth(const fs::path& p)
        {
            std::error_code ec;
            size_t n = 0;
            for (const auto& part : fs::weakly_canonical(p, ec))
                (void)part, ++n;
            return n;
        }
    }

    std::string ZoneOf(const fs::path& file, const fs::path& vaultRoot, const fs::path& loreRoot)
    {
        const bool inVault = IsUnder(vaultRoot, file);
        const bool inLore = IsUnder(loreRoot, file);
        if (inVault && inLore)
            return Depth(loreRoot) >= Depth(vaultRoot) ? "lore" : "vault";
        return inLore ? "lore" : inVault ? "vault" : "";
    }

    std::string RunRead(const Call& call, const Sources& src, ChannelType type)
    {
        const json& a = call.args;
        if (call.name == "__invalide")
            return "Bloc outil illisible (JSON invalide) : " + a.value("texte", std::string());

        if (call.name == "chercher_historique")
        {
            const std::string query = Lower(a.value("requete", ""));
            if (query.empty())
                return "Requête vide.";
            const bool wide = a.value("portee", "salon") == "sous-serveur";
            std::vector<std::string> channels = wide ? src.channelIds : std::vector<std::string>{src.channelId};
            std::string out;
            int hits = 0;
            for (const std::string& cid : channels)
            {
                const fs::path t = src.dataRoot / "subservers" / src.subserverId / "channels" / cid / "transcript.jsonl";
                std::ifstream in(t, std::ios::binary);
                std::string line;
                while (std::getline(in, line) && hits < 25)
                {
                    try
                    {
                        const json r = json::parse(line);
                        if (r.value("type", "") != "message")
                            continue;
                        const json& d = r["data"];
                        const std::string content = d.value("content", "");
                        if (Lower(content).find(query) == std::string::npos)
                            continue;
                        out += "[" + d.value("timestamp", "") + "] " + d.value("sender", "") + " : " +
                               (content.size() > 400 ? content.substr(0, 400) + "…" : content) + "\n";
                        ++hits;
                    }
                    catch (...)
                    {
                    }
                }
            }
            return hits ? "Messages trouvés (" + std::to_string(hits) + ") :\n" + out : "Aucun message ne contient « " + a.value("requete", "") + " ».";
        }

        if (call.name == "chercher_mails" || call.name == "lire_agenda")
        {
            const fs::path cacheFile = src.dataRoot / "cloud" / "cache.json";
            try
            {
                std::ifstream in(cacheFile, std::ios::binary);
                if (!in) return "Aucun compte mail/agenda n'est encore synchronisé.";
                json cache = json::parse(in);
                if (cache.contains("protected"))
                {
                    const std::string plain = Secrets::Unprotect(cache.value("protected", ""));
                    if (plain.empty()) return "Le cache mail/agenda ne peut pas être déchiffré pour ce compte Windows.";
                    cache = json::parse(plain);
                }
                const std::string query = Lower(a.value("requete", ""));
                std::string out = "Dernière synchronisation : " + cache.value("syncedAt", "inconnue") + "\n";
                int hits = 0;
                if (call.name == "chercher_mails")
                {
                    const bool unreadOnly = a.value("non_lus", false);
                    for (const json& m : cache.value("messages", json::array()))
                    {
                        if (unreadOnly && m.value("isRead", false)) continue;
                        const std::string subject = m.value("subject", "(sans objet)");
                        const std::string preview = m.value("bodyPreview", "");
                        std::string from;
                        if (m.contains("from") && m["from"].contains("emailAddress"))
                            from = m["from"]["emailAddress"].value("name", m["from"]["emailAddress"].value("address", ""));
                        if (!query.empty() && Lower(subject + " " + preview + " " + from).find(query) == std::string::npos) continue;
                        out += "[" + m.value("receivedDateTime", "") + "] " + (m.value("isRead", false) ? "" : "[NON LU] ") +
                               from + " — " + subject + "\n" + preview.substr(0, 500) + "\n\n";
                        if (++hits >= 30) break;
                    }
                }
                else
                {
                    for (const json& e : cache.value("events", json::array()))
                    {
                        const std::string subject = e.value("subject", "(sans titre)");
                        const std::string location = e.contains("location") ? e["location"].value("displayName", "") : "";
                        if (!query.empty() && Lower(subject + " " + location).find(query) == std::string::npos) continue;
                        const std::string start = e.contains("start") ? e["start"].value("dateTime", "") : "";
                        const std::string end = e.contains("end") ? e["end"].value("dateTime", "") : "";
                        out += start + " → " + end + " — " + subject + (location.empty() ? "" : " @ " + location) + "\n";
                        if (++hits >= 80) break;
                    }
                }
                return hits ? out : "Aucun élément correspondant dans le cache synchronisé.";
            }
            catch (const std::exception& e) { return std::string("Cache mail/agenda illisible : ") + e.what(); }
        }

        if (call.name == "solde_comptes" || call.name == "chercher_transactions")
        {
            try
            {
                std::ifstream in(src.dataRoot / "cloud" / "finance.json", std::ios::binary);
                if (!in) return "Aucun compte bancaire n'est encore synchronisé.";
                json cache = json::parse(in);
                if (cache.contains("protected"))
                {
                    const std::string plain = Secrets::Unprotect(cache.value("protected", ""));
                    if (plain.empty()) return "Le cache bancaire ne peut pas être déchiffré pour ce compte Windows.";
                    cache = json::parse(plain);
                }
                std::string out = "Dernière synchronisation : " + cache.value("syncedAt", "inconnue") + "\n";
                int hits = 0;
                const std::string query = Lower(a.value("requete", ""));
                const bool expensesOnly = a.value("depenses_seulement", false);
                for (const json& account : cache.value("accounts", json::array()))
                {
                    const std::string name = account.value("name", "Compte");
                    const std::string currency = account.value("currency", "EUR");
                    if (call.name == "solde_comptes")
                    {
                        out += name + " : " + account.value("balance", "?") + " " + currency;
                        if (account.contains("available-balance"))
                            out += " (disponible : " + account.value("available-balance", "?") + " " + currency + ")";
                        out += "\n";
                        ++hits;
                        continue;
                    }
                    for (const json& transaction : account.value("transactions", json::array()))
                    {
                        const std::string amount = transaction.value("amount", "0");
                        double numeric = 0.0;
                        try { numeric = std::stod(amount); } catch (...) {}
                        if (expensesOnly && numeric >= 0.0) continue;
                        const std::string description = transaction.value("description", "");
                        const std::string payee = transaction.value("payee", "");
                        const std::string memo = transaction.value("memo", "");
                        if (!query.empty() && Lower(description + " " + payee + " " + memo + " " + name).find(query) == std::string::npos)
                            continue;
                        std::string date;
                        const std::time_t epoch = static_cast<std::time_t>(transaction.value("posted", 0LL));
                        if (epoch > 0)
                        {
                            std::tm local{}; localtime_s(&local, &epoch);
                            std::ostringstream formatted; formatted << std::put_time(&local, "%Y-%m-%d"); date = formatted.str();
                        }
                        out += "[" + date + "] " + name + " — " + amount + " " + currency + " — " +
                               (!payee.empty() ? payee : description);
                        if (transaction.value("pending", false)) out += " [EN ATTENTE]";
                        if (!memo.empty()) out += " — " + memo;
                        out += "\n";
                        if (++hits >= 100) break;
                    }
                    if (hits >= 100) break;
                }
                return hits ? out : "Aucun élément correspondant dans le cache bancaire synchronisé.";
            }
            catch (const std::exception& e) { return std::string("Cache bancaire illisible : ") + e.what(); }
        }

        fs::path globalRoot;
        const fs::path* root = RootFor(a, src);
        const std::string source = a.value("source", "vault");
        if ((type == ChannelType::Code || type == ChannelType::Bugs) && source != "principal" &&
            source != "global" && source.rfind("dossier_", 0) != 0)
            return "Dans un salon d'ingénierie, utilise source=principal ou un dossier supplémentaire autorisé.";
        if (source == "global" && src.globalRead)
        {
            const std::string raw = call.name == "lister" ? a.value("chemin", a.value("dossier", std::string())) : a.value("chemin", "");
            const fs::path absolute = fs::path(Platform::Widen(raw));
            if (absolute.is_absolute())
            {
                globalRoot = absolute.root_path();
                root = &globalRoot;
            }
        }
        if (!root)
            return "Ce sous-serveur n'a pas de " + a.value("source", std::string("vault")) + ".";

        if (call.name == "lire_note" || call.name == "lire")
        {
            const fs::path requested = fs::path(Platform::Widen(a.value("chemin", "")));
            const fs::path p = source == "global" && requested.is_absolute() ? requested : Confine(*root, a.value("chemin", ""));
            std::error_code ec;
            if (p.empty() || !ExclusionMode(src, *root, p).empty() || !fs::is_regular_file(p, ec) || !IsTextNote(p))
                return "Note introuvable ou hors du périmètre : " + a.value("chemin", "");
            std::string text = ReadFileUtf8(p, 60000);
            if (text.size() == 60000)
                text += "\n[…note tronquée à 60 000 caractères]";
            if (call.name == "lire")
                text = WithLineNumbers(text, a.value("debut", 1), a.value("fin", 0));
            return "Contenu de « " + Rel(*root, p) + " » :\n" + text;
        }

        if (call.name == "lister")
        {
            const std::string relativeArg = a.value("chemin", a.value("dossier", std::string()));
            const fs::path requested = fs::path(Platform::Widen(relativeArg));
            const fs::path dir = source == "global" && requested.is_absolute() ? requested :
                                 (relativeArg.empty() ? *root : Confine(*root, relativeArg));
            std::error_code ec;
            if (dir.empty() || !fs::is_directory(dir, ec))
                return "Dossier introuvable : " + relativeArg;
            std::string out;
            int n = 0;
            const bool recursive = a.value("recursif", false);
            auto addEntry = [&](const fs::directory_entry& e) {
                const fs::path rel = fs::relative(e.path(), *root, ec);
                if (ExclusionMode(src, *root, e.path()) == "cant_access" || Hidden(rel) ||
                    (!e.is_directory() && !IsTextNote(e.path())))
                    return;
                out += (e.is_directory() ? "[dossier] " : "[fichier] ") + Platform::Narrow(rel.generic_wstring());
                if (e.is_regular_file())
                    out += " — " + std::to_string(e.file_size(ec)) + " octets";
                out += "\n";
                ++n;
            };
            if (recursive)
            {
                for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator() && n < 500; it.increment(ec))
                {
                    const fs::path rel = fs::relative(it->path(), *root, ec);
                    if (Hidden(rel) || ExclusionMode(src, *root, it->path()) == "cant_access")
                    {
                        if (it->is_directory()) it.disable_recursion_pending();
                        continue;
                    }
                    addEntry(*it);
                }
            }
            else
                for (const auto& e : fs::directory_iterator(dir, ec))
                    if (n < 500) addEntry(e);
            if (n >= 500) out += "[…liste tronquée]\n";
            return out.empty() ? "Dossier vide." : out;
        }

        if (call.name == "chercher")
        {
            const std::string rawQuery = a.value("motif", a.value("requete", std::string()));
            const std::string query = Lower(rawQuery);
            if (rawQuery.empty())
                return "Requête vide.";
            const bool regexMode = a.value("regex", false);
            std::regex expression;
            if (regexMode)
                try { expression = std::regex(rawQuery, std::regex::ECMAScript | std::regex::icase); }
                catch (const std::regex_error& e) { return std::string("Expression régulière invalide : ") + e.what(); }
            std::vector<std::string> extensions;
            if (a.contains("extensions") && a["extensions"].is_array())
                for (const json& ext : a["extensions"])
                    if (ext.is_string()) extensions.push_back(Lower(ext.get<std::string>()));
            std::string out;
            int hits = 0, files = 0;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(*root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator() && hits < 30 && files < 20000; it.increment(ec))
            {
                const fs::path rel = fs::relative(it->path(), *root, ec);
                const std::string excluded = ExclusionMode(src, *root, it->path());
                if (excluded == "cant_access" || Hidden(rel))
                {
                    if (it->is_directory())
                        it.disable_recursion_pending();
                    continue;
                }
                if (excluded == "cant_read")
                    continue;
                if (!it->is_regular_file() || !IsTextNote(it->path()))
                    continue;
                if (!extensions.empty())
                {
                    const std::string ext = Lower(Platform::Narrow(it->path().extension().wstring()));
                    if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
                        continue;
                }
                ++files;
                const std::string text = ReadFileUtf8(it->path(), 400000);
                const std::string lower = Lower(text);
                const std::string name = Platform::Narrow(rel.generic_wstring());
                std::istringstream lines(text);
                std::string line;
                int lineNumber = 0, perFile = 0;
                while (std::getline(lines, line) && perFile < 20 && hits < 100)
                {
                    ++lineNumber;
                    const bool matches = regexMode ? std::regex_search(line, expression) : Lower(line).find(query) != std::string::npos;
                    if (!matches) continue;
                    if (line.size() > 300) line.resize(300);
                    out += name + ":" + std::to_string(lineNumber) + ":" + line + "\n";
                    ++hits;
                    ++perFile;
                }
            }
            return hits ? out : "Rien trouvé pour « " + rawQuery + " ».";
        }

        return "Outil inconnu : " + call.name;
    }
}
