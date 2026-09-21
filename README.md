# Agents Chat

Application locale de coordination multi-agents (C++ / Dear ImGui) où ChatGPT, Claude et Gemini
discutent avec toi et se coordonnent, en pilotant leurs versions « code »
(Codex, Claude Code, Gemini CLI) dans les salons Code.

Ce n'est pas un clone ni un extracteur de ces services : c'est une interface unifiée pour éviter de
jongler entre trois applications, en utilisant leurs points d'accès officiels déjà authentifiés.

## État actuel

- Sous-serveurs avec leurs sources (vault Obsidian, dossier lore, dossier de code).
- Salons Analyse, Détente, Consolidation lore, Code et Bugs GitHub.
- Fil de messages et saisie (Entrée envoie, Ctrl+Entrée va à la ligne).
- Transcript en ajout seul, un par salon.
- Boîte aux lettres : questions des IA, écritures de fichiers (avant/après), corrections du vault ou du lore,
  travaux de code, autorisations ponctuelles, demandes de compétences, actions GitHub et auto-amélioration.
  Rien de tout cela n'est exécuté sans ton accord.
- Liste des membres avec leur statut réel.
- Mémoire réellement commune : les faits sont visibles par toute l'équipe selon
  leur portée et les doublons équivalents sont fusionnés, même entre IA.
- Sous-serveur intégré « Amélioration d’Agents Chat » : les IA de son salon Code modifient l'application,
  la compilent et lancent ses tests, puis proposent de l'installer (seulement si les tests passent,
  avec retour arrière) ou d'ouvrir une pull request.
- Todo transversale par personne, réaffectation/statut, et PM local dédié avec chaque IA.
- Salons Bugs GitHub synchronisés avec les issues via `gh` (déjà connecté, aucun jeton stocké) :
  import en tâches « #N », création, commentaire, et fermeture seulement après ta vérification des tests.
- Mises à jour signées : une release n'est installée automatiquement que si elle est signée par la clé
  de publication intégrée ; la version remplacée est gardée. Voir [RELEASING.md](RELEASING.md).
- Langue et niveau de modèle (Léger / Normal / Fort) par salon ; niveau réglable par IA
  dans « Membres ».
- Options → Modèles : pour chaque IA et chaque niveau, le modèle et le niveau de réflexion.
- Statut de chaque IA et de son agent de code (en ligne, hors ligne jusqu'à…, non branchée).
  Clic droit : mettre en pause ou remettre en ligne.

Les connexions ChatGPT/Codex, Claude/Claude Code, Gemini/Gemini CLI et plusieurs
API compatibles sont prises en charge. Les travaux de code restent soumis à
l'accord de l'utilisatrice.

Le routage est **forfait d'abord** : Codex CLI, Claude Code et Gemini CLI sont prioritaires.
Une clé Gemini API configurée ne sert de repli que si Gemini CLI est indisponible ; l'application
ne remplace donc pas silencieusement un accès inclus dans un forfait par des appels facturés au token.

## Compiler

Prérequis : Visual Studio 2026 avec la charge « Développement Desktop en C++ »
(elle fournit CMake). Dear ImGui et nlohmann/json sont téléchargés
automatiquement par CMake au premier `configure`.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

L'exécutable est `build\Release\AgentChats.exe`.

Tests de sécurité (confinement des chemins, zones vault/lore, outils, signatures, GitHub) :

```powershell
build\Release\AgentChatsTests.exe
```

Le port Linux n'est pas encore compilable : le noyau métier est largement réutilisable,
mais l'hôte graphique et plusieurs services système sont Windows. Voir [PORTABILITY.md](PORTABILITY.md).

## Sécurité et secrets

Le dépôt ne contient aucune clé API, aucun jeton OAuth et aucune donnée personnelle.
Les clés configurées dans l'application sont stockées hors du dépôt, sous `%LOCALAPPDATA%`,
et chiffrées par Windows DPAPI pour le compte utilisateur courant. Les caches de mails,
d'agendas et de banque restent eux aussi locaux et chiffrés.

Ne publiez jamais le contenu du dossier `%LOCALAPPDATA%\AgentChats\`. Les fichiers de
configuration locale, caches, journaux, clés et certificats sont volontairement ignorés.

## Licence

Agents Chat est distribué sous la licence
[GNU Affero General Public License v3.0](LICENSE). Les versions modifiées distribuées
ou proposées aux utilisateurs au travers d'un réseau doivent conserver cette licence
et mettre leur code source correspondant à disposition.

Si `cmake` n'est pas dans le PATH, celui de Visual Studio est ici :
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

## Où sont les données

`%LOCALAPPDATA%\AgentChats\`

- `workspace.json` : sous-serveurs et salons.
- `subservers\<id>\channels\<id>\transcript.jsonl` : un événement par ligne, jamais réécrit.
- `settings.json` : modèles par niveau et statuts hors ligne.
- `imgui.ini` : disposition de la fenêtre.

Pour essayer l'application sur d'autres données sans toucher aux tiennes,
définis la variable d'environnement `AGENTCHATS_DATA` vers un autre dossier
avant de la lancer.

## Organisation du code

| Fichier | Rôle |
|---|---|
| `src/main.cpp` | Fenêtre Win32, DirectX 11, boucle d'affichage, application d'une mise à jour |
| `src/App.cpp` | Toute l'interface : colonnes, fil, boîte aux lettres, Options, actions des IA |
| `src/OptionsModules.cpp` | Liste des pages d'Options (mises à jour, connexions, modèles, mémoire, connecteurs, plugins, MCPs) |
| `src/Store.cpp` | `workspace.json`, transcripts, mémoire commune, tâches, boîte aux lettres |
| `src/Settings.cpp` | Modèles par niveau, présence, préférences |
| `src/Model.h` | Types de données : sous-serveur, salon, message, demande, tâche |
| `src/Conductor.cpp` | Tour de parole des IA, rôles, outils en texte |
| `src/Backends.cpp` | Appels aux CLI officielles (Claude Code, Codex, Gemini/Antigravity) et aux API |
| `src/CodeWorker.cpp` | Agents de code lancés dans le dossier du projet |
| `src/Tools.cpp` | Outils des IA, confinement des chemins, zones vault/lore |
| `src/ModelCatalog.cpp` | Catalogue des modèles et niveaux de réflexion |
| `src/GitHub.cpp` | Issues et pull requests via `gh` |
| `src/Updater.cpp` | Mises à jour signées, compilation locale testée, retour arrière |
| `src/ReleaseSignature.cpp`, `src/UpdateKeys.h` | Vérification ECDSA P-256 des releases, clés de confiance |
| `src/CloudSync.cpp` | Mail, agenda et banque en lecture seule, caches locaux |
| `src/Process.cpp`, `src/Http.cpp`, `src/Secrets.cpp`, `src/Platform.cpp` | Processus (job objects), WinHTTP, DPAPI, services Windows |
| `tests/SafetyTests.cpp` | Tests automatisés et diagnostics (`--connections`, `--github-read`, `--self-build`) |
| `tools/ReleaseTool.cpp` | Outil de la mainteneuse : clé de publication, signature, sauvegarde (jamais livré) |
