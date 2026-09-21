# Publier une version d'Agents Chat

Les copies installées n'acceptent une mise à jour automatique que si elle est **signée par la clé de
publication** dont la partie publique est intégrée dans `src/UpdateKeys.h`, pour la version exacte
annoncée par la release. L'empreinte `.sha256` reste publiée pour les humains, mais ne suffit plus :
un compte GitHub compromis ne peut pas faire installer un faux exécutable.

## Une seule fois : créer la clé

```powershell
build\Release\AgentChatsRelease.exe keygen
```

- La clé privée est enregistrée, chiffrée pour ton compte Windows (DPAPI), dans
  `%LOCALAPPDATA%\AgentChats-release\signing-key.dpapi`. Elle n'entre jamais dans le dépôt.
- Copie la clé publique affichée dans `src/UpdateKeys.h` (liste `keys`), puis commite.
- Fais **tout de suite** une sauvegarde, et range-la hors de ce PC :

```powershell
build\Release\AgentChatsRelease.exe backup E:\quelque-part\agents-chat-release.agkey
```

  Sans cette sauvegarde, perdre ce PC ou ce compte Windows signifie qu'aucune copie installée ne
  pourra plus recevoir de mise à jour automatique. Pour la réinstaller ailleurs :
  `AgentChatsRelease restore <fichier>`.

La première version qui embarque la clé (par ex. 0.1.1) doit être installée **à la main** une fois :
les copies plus anciennes n'ont aucune clé et refusent désormais toute mise à jour automatique.

## À chaque version

1. Monter la version dans `CMakeLists.txt` (`AGENTCHATS_VERSION` et `CPACK_PACKAGE_VERSION`).
2. Compiler et tester :

   ```powershell
   cmake --build build --config Release
   build\Release\AgentChatsTests.exe
   ```

3. Signer l'exécutable pour le tag exact de la release :

   ```powershell
   build\Release\AgentChatsRelease.exe sign build\Release\AgentChats.exe v0.1.2
   ```

   Cela produit `AgentChats.exe.sig` et `AgentChats.exe.sha256` à côté de l'exécutable.

4. Publier les trois fichiers sous le même tag :

   ```powershell
   gh release create v0.1.2 build\Release\AgentChats.exe build\Release\AgentChats.exe.sig build\Release\AgentChats.exe.sha256 --title "Agents Chat 0.1.2" --notes "…"
   ```

## Ce que vérifie l'application

- la release contient `AgentChats.exe` **et** `AgentChats.exe.sig` ;
- la signature (ECDSA P-256) porte sur `version=<tag>` et `sha256=<empreinte de l'exe téléchargé>` ;
- elle a été faite par une clé de `src/UpdateKeys.h` ;
- la version est plus récente que celle installée.

Sinon, le fichier téléchargé est supprimé et le motif est affiché dans Options → Mises à jour.

## Changer de clé

1. `keygen` sur une nouvelle clé (après avoir mis l'ancienne de côté), ajouter sa clé publique dans
   `UpdateKeys.h` **à côté** de l'ancienne ;
2. publier cette version signée avec **l'ancienne** clé ;
3. dans une version suivante, signée avec la nouvelle, retirer l'ancienne clé.

## Revenir en arrière

Chaque mise à jour garde la version remplacée dans `AgentChats.previous.exe`, à côté de l'exécutable.
Options → Mises à jour → « Revenir à la version précédente ».
