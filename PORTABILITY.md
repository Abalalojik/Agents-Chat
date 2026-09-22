# Portabilité Linux

## Verdict

**Compilé et testé sous Linux** le 22 septembre 2026 (Ubuntu 24.04, g++ 13, dans Docker) : les 21 fichiers
compilent, le build CMake complet passe sans avertissement et tous les tests de `AgentChatsTests` passent.
Le build Windows reste identique (zéro avertissement, tests verts).

**Pas encore essayé sur un vrai bureau Linux** : la fenêtre SDL2/OpenGL, le trousseau (libsecret), la
corbeille (`gio trash`), l'ouverture des liens, le choix de dossier et les consoles de connexion des CLI
demandent une session graphique, absente du conteneur. Linux n'est déclaré disponible qu'après cet essai.

Vérifier depuis Windows, avec Docker Desktop lancé (dépôt monté en lecture seule) :

```powershell
powershell -File linux\check.ps1        # chaque fichier compilé seul : tableau OK / ÉCHEC
powershell -File linux\check.ps1 -Full  # vrai build CMake + Ninja, puis les tests
```

Choix faits pour Linux : libcurl (HTTP), trousseau Secret Service via libsecret (secrets), OpenSSL
(signatures, mêmes formats de clé et de signature que BCrypt), `fork`/`execve` avec groupe de processus
(annulation et délai tuent tout l'arbre ; **plafond mémoire pas encore appliqué**, un cgroup est prévu),
`gio trash` (corbeille), `xdg-open` (liens), `zenity` ou `kdialog` (choix de dossier), terminal du bureau (connexion
des CLI), timer systemd utilisateur (synchronisation), SDL2 + OpenGL 3 (fenêtre). La mise à jour se
remplace en place et démarre au lancement suivant.

## Réutilisable sans changement de format

- `workspace.json`, `tasks.json`, `memory.json`, `inbox.json` et les transcripts JSONL : formats texte indépendants du système.
- `Model.h` et l'essentiel de `Conductor.cpp` : modèles, orchestration, rôles et sélection des agents.
- La logique JSON de `Store.cpp`, `Settings.cpp`, `Tools.cpp` et `CloudSync.cpp`.
- Dear ImGui, nlohmann/json, les API OAuth/REST Microsoft, Google et SimpleFIN.
- Les CLI Codex, Claude et Gemini si leurs exécutables sont installés et authentifiés sur Linux.

Les chemins enregistrés dans les sous-serveurs restent toutefois des chemins Windows. Ils doivent être
remappés vers leurs équivalents Linux lors du premier démarrage, sans réécrire les transcripts.

## À remplacer derrière une couche plateforme

| Élément Windows actuel | Remplacement Linux attendu | Pourquoi |
|---|---|---|
| Win32 + DirectX 11 (`main.cpp`) | SDL2/GLFW + OpenGL ou Vulkan | fenêtre, événements et rendu sont entièrement Win32/D3D11 |
| `CreateProcessW` et Job Objects (`Process.cpp`) | `posix_spawn`/`fork+exec`, groupes de processus et signaux | lancement, annulation et plafond mémoire utilisent l'API Windows |
| WinHTTP (`Http.cpp`) | libcurl | toutes les requêtes HTTP passent par WinHTTP |
| DPAPI (`Secrets.cpp`) | Secret Service/libsecret, avec migration explicite | un secret DPAPI ne peut être déchiffré que par le compte Windows qui l'a créé |
| sélecteur COM et `%LOCALAPPDATA%` (`Platform.cpp`) | portail XDG/GTK et `$XDG_DATA_HOME` | dialogue de dossier et répertoire de données sont spécifiques à Windows |
| Corbeille via `SHFileOperationW` (`Store.cpp`) | `gio trash` ou trash spec freedesktop | la suppression récupérable dépend du Shell Windows |
| `ShellExecuteW` | `xdg-open`/portail desktop | ouverture du navigateur pour OAuth et GitHub |
| `schtasks.exe` | timer systemd utilisateur | le cron local est actuellement une tâche planifiée Windows |
| détection `.exe` et environnement PowerShell | noms Unix et shell explicite | découverte des CLI et commandes autorisées supposent Windows |

## Non importable tel quel

- Les valeurs chiffrées DPAPI dans `settings.json` et les jetons/caches chiffrés : reconnecter les comptes
  sous Linux est obligatoire. Copier ces blobs ne sert à rien et affaiblir leur protection pour les rendre
  portables serait une mauvaise solution.
- L'exécutable Windows et sa configuration ImGui de fenêtre : ils ne constituent pas un build Linux.
- Les chemins absolus `C:\...`/`E:\...` : ils n'ont pas de sens natif sous Linux.

## Ordre de port recommandé

1. Extraire des interfaces `Platform`, `Process`, `Http`, `Secrets` et `Desktop` avec implémentations Win32/Linux.
2. Construire d'abord `AgentChatsTests` sur Linux, sans interface graphique, pour valider stockage, mémoire,
   todo, orchestration et confinement des chemins.
3. Ajouter SDL2 + OpenGL pour Dear ImGui et remplacer les appels directs à `ShellExecuteW` dans `App.cpp`.
4. Migrer les chemins au premier lancement et demander une nouvelle connexion pour chaque secret DPAPI.
5. Valider réellement Codex, Claude et Gemini CLI sur Linux ; leur présence multiplateforme ne garantit pas
   que les arguments ou le flux de connexion soient identiques à ceux testés sous Windows.
