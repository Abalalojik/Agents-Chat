# Cahier des charges — Agents Chat

Version de référence : 21 septembre 2026.

## 1. Finalité

Agents Chat doit devenir l'interface locale unique de l'utilisatrice pour travailler avec ChatGPT/Codex,
Claude/Claude Code et Gemini/Antigravity, sans jongler entre leurs trois applications et sans perdre les
avantages des abonnements existants. Il ne copie, n'aspire et n'émule pas ces services : il pilote leurs
interfaces officielles déjà installées et authentifiées.

Le produit coordonne les agents, conserve une mémoire commune, centralise conversations, tâches et
sources de travail, et ne confond jamais abonnement grand public/professionnel et API facturée au token.

## 2. Principes non négociables

1. **Forfaits d'abord.** Utiliser en priorité les CLI officielles authentifiées : Codex CLI, Claude Code,
   Gemini/Antigravity CLI. Une API payante n'est utilisée qu'après configuration et choix explicites.
2. **Aucun secret dans Git.** Jetons, clés, caches personnels, mails, agendas et données bancaires restent
   hors du dépôt public et sont chiffrés avec le coffre du système.
3. **Local d'abord.** Transcripts, tâches, mémoire et caches restent utilisables hors ligne. Les services
   distants ne reçoivent que ce qui est nécessaire à une requête.
4. **Mémoire réellement commune.** Une note équivalente apprise par deux IA est fusionnée ; sa portée
   (`toi`, sous-serveur, salon) est respectée et sa provenance reste visible.
5. **Actions sensibles approuvées.** Écriture de code, publication, banque et commandes système suivent
   une frontière d'autorisation explicite et sont journalisées.
5 bis. **Mises à jour signées.** Une mise à jour automatique n'est installée que si elle est signée (ECDSA P-256)
   par une clé de publication intégrée à l'application, pour sa version exacte ; la clé privée reste hors du dépôt.
   La version remplacée est conservée pour un retour arrière. Procédure : `RELEASING.md`.
6. **Linux est une cible produit obligatoire.** L'application ne doit pas enfermer l'utilisatrice sur Windows.
   Les formats sont multiplateformes et un build Linux ne sera déclaré disponible qu'après compilation et tests
   natifs, pas parce que le projet utilise CMake.

## 3. Utilisateurs et agents

- L'utilisatrice possède sa propre identité, sa todo globale et ses PM.
- Agents principaux : ChatGPT (programmation), Claude (organisation/analyse), Gemini (recherche).
- Agents de code : Codex, Claude Code, Gemini/Antigravity CLI.
- Agents optionnels par API : Mistral, DeepSeek, Grok et fournisseurs compatibles.
- Chaque agent expose son état réel : connecté, indisponible, en pause, limité ou en cours de travail.

## 4. Espaces de travail

Un sous-serveur représente un projet ou un univers et référence éventuellement :

- un vault documentaire ;
- un dossier de lore ;
- un dépôt de code.

Types de salons :

- **Analyse** : lecture et propositions de correction du vault ;
- **Détente** : conversation et lecture seule ;
- **Consolidation lore** : maintien du canon et propositions d'écriture ;
- **Code** : coordination et délégation aux agents de code ;
- **Bugs GitHub** : qualification, reproduction, tâche locale, issue GitHub, correction et non-régression.

Le sous-serveur intégré « Amélioration d’Agents Chat » sert à faire évoluer Agents Chat lui-même. Dans son salon
Code, les IA modifient le code (écritures et agents de code approuvés), puis l'outil `ameliorer` enchaîne, chaque
étape passant par la boîte aux lettres :

- `compiler_tester` : configure, compile `AgentChats` et `AgentChatsTests`, lance les tests ; le résultat (et la fin
  du journal en cas d'échec) revient dans le salon et l'IA demandeuse reprend la main ;
- `installer` : n'installe que la dernière version locale **dont les tests passent** ; la version remplacée est gardée
  pour le retour arrière ;
- `proposer_pr` : commit de toutes les modifications, branche `amelioration/<date>` (ou la branche de travail en
  cours), push, puis pull request via `gh` ; une PR existante pour la branche est simplement mise à jour.

## 5. Conversations privées et todo

- « Ma todo » agrège les tâches de l'utilisatrice de tous les salons.
- « PM / tâches » sur un agent affiche sa charge globale, ses statuts et leur salon d'origine.
- Une tâche possède : identifiant, titre, salon, assigné, créateur, statut et date de mise à jour.
- Statuts : à faire, en cours, fait, bloqué.
- Depuis la vue globale : créer, ouvrir le salon source, réaffecter, changer le statut, supprimer.
- Un PM avec une IA crée ou réutilise un salon local dédié où cette IA est l'interlocuteur principal.
- Les IA peuvent créer et mettre à jour les tâches via leur outil, avec résultat visible immédiatement.

## 6. GitHub et gestion des bugs

- Le dépôt est déterminé depuis `remote.origin.url` du dossier de code, jamais depuis un jeton enregistré
  dans le workspace.
- L'interface ouvre la liste des issues et la création d'une issue dans le navigateur.
- Fait : synchronisation via `gh` authentifié (aucun jeton stocké) : numéro d'issue stocké dans la tâche,
  import des états, création et commentaires proposés par les IA puis envoyés après accord, fermeture seulement
  après la case « tests vérifiés » cochée par l'utilisatrice.
- Une issue distante ne doit jamais être fermée automatiquement sur la seule affirmation d'une IA.

## 7. Mémoire et contexte économique

- Mémoire structurée commune persistante avec déduplication sémantique et promotion de portée.
- Contexte court préparé localement avant chaque appel afin de réduire les tokens.
- Fait : contexte court préparé dans l'application, sans modèle ni service : fil récent intact (16 000 caractères),
  jusqu'à 6 extraits plus anciens choisis par pertinence lexicale (BM25, accents repliés) pour la dernière demande,
  et mémoire réduite aux notes pertinentes puis récentes au-delà de 6 000 caractères. Jamais d'Ollama.
- Cible suivante : petit modèle embarqué pour résumer, si le classement lexical ne suffit plus.
- Salon d'entraînement séparé pour préparer, évaluer et versionner les jeux de données de fine-tuning.
- ChatGPT, Claude et Gemini peuvent proposer/corriger des exemples ; aucune donnée personnelle n'entre
  dans un jeu publiable sans validation et nettoyage.

## 8. Données personnelles connectées

### Mail et agenda

- Comptes personnels Microsoft/Hotmail et Google pris en charge par OAuth officiel.
- Synchronisation locale périodique des mails et agendas, puis recherche locale par les outils des IA.
- Lecture seule par défaut ; envoi, suppression ou modification exigent une fonction et une autorisation séparées.
- Ne pas forcer un tenant Microsoft professionnel pour un compte Hotmail personnel.

### Banque

- Connexion en lecture seule via un agrégateur réglementé/compatible, actuellement SimpleFIN lorsque disponible.
- Soldes et transactions sont mis en cache localement ; les outils ne révèlent pas les identifiants de compte.
- Agents Chat ne collecte ni mot de passe bancaire ni code de double authentification.
- Aucune initiation de paiement dans le périmètre actuel.

## 9. Planification locale

- Les synchronisations récurrentes utilisent le planificateur du système, pas un modèle qui tourne en cron.
- Windows : tâche planifiée locale. Linux cible : timer systemd utilisateur.
- Une exécution sans changement ne doit pas appeler de modèle ni produire de notification inutile.

## 10. Invitations et accès distant

- Cible : lien d'invitation à durée limitée permettant de brancher une instance/IA autorisée.
- Ne jamais exposer directement l'IP résidentielle ni ouvrir un port entrant sur la box.
- Utiliser un relais/tunnel authentifié (par exemple un service gcloud adapté), TLS, jetons à portée limitée,
  expiration, révocation et journal d'accès.
- Le lien d'invitation n'accorde jamais l'accès implicite aux fichiers, secrets ou comptes connectés.

## 11. Console de salon Code

- Besoin : console en bas des salons Code avec profils gcloud, CMD et PowerShell.
- État : **activée** après validation du modèle de sécurité par l'utilisatrice (21 septembre 2026) :
  - une commande = un processus, dans le dossier de code du sous-serveur (affiché), sans état conservé entre deux
    commandes ; profils CMD, PowerShell et gcloud (ce dernier : `gcloud …` uniquement, sans enchaînement ni redirection) ;
  - groupe de processus Windows : 3 Go, 10 minutes, bouton d'arrêt qui tue aussi les enfants ; jamais d'élévation ;
  - variables d'environnement à allure de secret retirées ; clés connues et formes de clés masquées à l'affichage
    et dans l'historique local (`console.jsonl` du salon), chaque ligne marquée « toi » ou IA ;
  - outil `console` des IA : carte dans la boîte aux lettres avec la commande exacte, sauf commande identique mot
    pour mot à la liste autorisée ; la sortie masquée revient à l'IA demandeuse. Les commandes de l'utilisatrice
    restent privées sauf « Partager avec le salon ».
- Exigences minimales : dossier de travail visible, liste de commandes autorisées ou confirmation, masquage des
  secrets, historique local, arrêt du processus, limites de ressources, distinction commande humaine/agent.
- Une console arbitraire héritant de toutes les variables et identifiants locaux est hors acceptation.

## 11 bis. Administration, rôles et extensions

L'administration reprend une navigation verticale inspirée des interfaces Discord/OpenAI, sans reprendre leur marque :

- Accès : Général, Membres, Groupes, Autorisations et rôles, Modèles, Identité et accès, Jetons d'accès, Trusted Access ;
- Extensions : Plugins, Connecteurs/Applications, **MCPs**, Skills, Marketplace ;
- Fonctionnalités : Agents, agents de code, mémoire locale et automatisations.

Un sous-serveur déclare un dossier principal, des dossiers supplémentaires et des exclusions. `Can Write` est une
capacité maximale du dossier ; elle ne devient effective que si le rôle de l'agent possède aussi `Can Write`.
Une exclusion `Can't access` masque et interdit le chemin ; `Can't read` permet d'en voir le nom mais jamais le contenu.

Les permissions sont attribuées aux rôles, façon Discord : Global Read, écriture de fichiers, gestion des tâches et
gestion GitHub. Global Read signifie tout l'ordinateur en lecture, mais son application exige une CLI locale authentifiée.
Un backend API et un invité restent techniquement bloqués même si leur rôle est mal configuré.

Chaque plugin/connector/skill possède un manifeste, une provenance, une version, des capacités demandées et un état
activé/désactivé. L'activation peut être limitée par sous-serveur et par rôle. Le marketplace vérifie signature/provenance,
affiche les permissions avant installation, et permet mise à jour, désactivation et désinstallation récupérable.

`MCPs` est un menu principal, jamais un sous-menu d'un connecteur. Chaque serveur affiche son nom, son transport
(`stdio`, HTTP ou SSE), sa commande ou URL, sa méthode d'authentification, la référence de son secret local, ses
sous-serveurs et rôles autorisés, son état de connexion et son dernier diagnostic. Les secrets eux-mêmes restent dans
le coffre du système et ne sont ni affichés après saisie, ni écrits dans le dépôt ou dans un manifeste exportable.

## 12. Portabilité Linux

Les formats de données et la majorité de la logique sont réutilisables. L'interface Win32/DirectX, le lancement
de processus, WinHTTP, DPAPI, la corbeille, l'ouverture du navigateur et `schtasks` doivent recevoir des
implémentations Linux. Le détail et l'ordre de migration figurent dans `PORTABILITY.md`.

Les secrets DPAPI ne sont pas importables : les comptes devront être reconnectés sur Linux. Les chemins Windows
seront remappés au premier lancement. Les transcripts et JSON ne doivent pas être perdus.

## 13. Critères d'acceptation généraux

Une fonctionnalité n'est terminée que si :

1. elle compile en Release sans avertissement nouveau significatif ;
2. les tests automatisés concernés passent ;
3. un essai à froid confirme le démarrage et le scénario utilisateur ;
4. les erreurs sont visibles et actionnables, sans secret dans les logs ;
5. la documentation publique et le cahier des charges restent cohérents ;
6. `git diff --check` passe et le dépôt public ne contient aucun secret ;
7. le commit est poussé sur GitHub avec une description compréhensible.

Pour une connexion d'agent, l'acceptation exige un vrai appel réussi, pas seulement la présence de l'exécutable.
Pour GitHub, elle exige une issue de test lue/créée via le compte authentifié. Pour Linux, elle exige un build et
un démarrage natifs.

## 14. État au 21 septembre 2026

Disponible ou implémenté : sous-serveurs/salons, orchestration multi-agent, agents de code avec approbation,
mémoire commune dédupliquée, boîte aux lettres, mail/agenda Microsoft et Google, cache bancaire en lecture seule,
planification locale Windows, todo de salon, todo globale par personne, PM locaux, type Bugs GitHub et liens vers
les issues, dépôt public AGPLv3.

Fait depuis : synchronisation `gh` des issues, mises à jour signées, auto-amélioration (compiler/tester, installer,
PR), contexte court local.

Test UI de bout en bout fait le 21 septembre 2026 (auto-amélioration : compilation, installation, retour arrière).

À achever/prouver : essai visuel de la console, relais d'invite, atelier de fine-tuning, et port Linux natif.
