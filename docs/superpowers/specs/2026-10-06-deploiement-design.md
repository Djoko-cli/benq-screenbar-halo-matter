# Déploiement de Maillage Thread et de Halo Compagnon

Spec du 06/10/2026. Djoko a validé la conception le même jour. Une copie identique se trouve dans le dépôt du pont Halo (`docs/superpowers/specs/2026-10-06-deploiement-design.md`).

## 0. Contexte et décisions

Deux apps macOS, chacune dans son dépôt public :

| App | Dépôt | Aujourd'hui |
|---|---|---|
| Maillage Thread | `Djoko-cli/maillage-thread` | version 1.0, bac à sable, signature ad hoc, aucune version publiée |
| Halo Compagnon | `Djoko-cli/benq-screenbar-halo-matter`, dossier `apps/macos` | version 1.0, bac à sable, signature ad hoc, aucune version publiée |

Le démon système `halo-routes` (dépôt du pont Halo, `tools/macos/halo-routes`) garde la route du Mac vers le réseau Thread. Il sert aux deux apps : Halo Compagnon joint le pont en UDP sur Thread, Maillage Thread joint la sonde par « Réseau Thread ».

**Décisions de Djoko (06/10) :**

| Sujet | Décision |
|---|---|
| Apps | les deux, chacune avec ses versions publiées sur son dépôt |
| Moteur de mise à jour | **Sparkle 2**, ajouté par le gestionnaire de paquets Swift |
| Mise à jour | **entièrement automatique** : recherche, téléchargement, installation, relance |
| Authenticité | **signature Ed25519** de chaque `.dmg` ; la clé privée reste dans le trousseau de Djoko |
| Gatekeeper | **la signature Ed25519 le remplace pour les mises à jour** : une mise à jour installée par l'app ne repasse pas par Gatekeeper ; la première installation, depuis le navigateur, y passe |
| Démon | renommé **Thread Route** (`thread-route`, étiquette `fr.djoko.thread.route`) |
| Livraison du démon | **installé par l'app**, avec l'approbation de Djoko dans Réglages Système ; mis à jour avec l'app |
| Porteur du démon | **les deux apps savent l'installer, une seule le fait** ; une seule source, dans le dépôt du pont Halo |
| Premières versions | Maillage Thread **1.0.0**, Halo Compagnon **1.0.0** |
| Remise | le `.dmg` de chaque app sur le Bureau de Djoko |

**Contraintes qui restent :**
- les apps restent signées ad hoc : pas de certificat Developer ID, pas de notarisation, jamais de `Local.xcconfig`, aucun identifiant d'équipe dans un fichier commité ;
- elles restent dans le bac à sable ;
- aucune donnée réelle dans ce qui est publié : le contrôle d'anonymisation passe avant chaque publication.

## 1. Le moteur de mise à jour, dans chaque app

- **Sparkle 2**, par le gestionnaire de paquets Swift, à une version figée, déclaré dans `project.yml` (XcodeGen).
- **Le flux de versions** est un fichier `appcast.xml`, joint à chaque version publiée sur GitHub. L'app le lit à une adresse stable :
  `https://github.com/Djoko-cli/<dépôt>/releases/latest/download/appcast.xml`.
- **La clé publique Ed25519** (`SUPublicEDKey`) est inscrite dans l'`Info.plist` de chaque app. Les deux apps partagent une seule paire de clés. La clé privée est créée par l'outil de Sparkle (`generate_keys`), qui la range dans le trousseau de Djoko.
- **Le comportement :**
  - recherche au démarrage, puis toutes les 24 heures (`SUScheduledCheckInterval` = 86400) ;
  - téléchargement et préparation automatiques (`SUAutomaticallyUpdate`) ;
  - installation quand l'app se ferme, ou tout de suite par « Installer et relancer ».
- **L'interface :**
  - un menu « Rechercher les mises à jour… » ;
  - dans les Réglages, « Rechercher automatiquement » et « Installer automatiquement », cochés par défaut ;
  - en français et en anglais : les textes de l'app passent par le catalogue, ceux de Sparkle par ses propres traductions.
- **Le bac à sable est gardé.** Sparkle installe par son service d'installation (`SUEnableInstallerLauncherService`). Les apps ajoutent seulement les droits « mach-lookup » de Sparkle (`<identifiant>-spks` et `<identifiant>-spki`). Elles ont déjà le droit `network.client` : le service de téléchargement de Sparkle n'est pas nécessaire.
- **Les tests de l'app** ne démarrent jamais le moteur de mise à jour, et n'accèdent jamais au réseau.

## 2. Thread Route

- **Le renommage :**
  - le dossier `tools/macos/halo-routes` devient `tools/macos/thread-route` ;
  - le programme `halo-routes` devient `thread-route`, et l'étiquette `fr.djoko.halo.routes` devient `fr.djoko.thread.route`, avec le programme, le plist et le journal ;
  - le comportement du démon ne change pas, et ses tests restent verts.
- **La migration.** L'ancien démon `fr.djoko.halo.routes` est arrêté et retiré avant le nouveau, pour que les deux ne tournent jamais ensemble. Cela demande le mot de passe administrateur une fois, et c'est Djoko qui le fait.
- **L'installation par l'app :**
  - le démon est compilé dans le paquet de l'app (`Contents/Library/LaunchDaemons/` et son programme) ;
  - il s'installe par `SMAppService.daemon`, et Djoko l'approuve une fois dans Réglages Système ;
  - l'app propose « Installer Thread Route » et affiche son état : absent, à approuver, actif, installé par l'autre app ;
  - une mise à jour de l'app par Sparkle apporte la nouvelle version du démon.
- **Une seule installation pour les deux apps :** une app qui voit Thread Route déjà actif, installé par l'autre, ne l'installe pas à son tour. Le moyen de le voir depuis le bac à sable est à établir par l'essai ci-dessous.
- **Une seule source :** le code du démon vit dans le dépôt du pont Halo. Maillage Thread en garde une copie à l'identique, avec un script de synchronisation et un test qui vérifie que la copie correspond à sa source, à une révision notée.
- **L'essai d'abord.** Le plan commence par un essai jetable, qui répond à trois questions :
  1. `SMAppService.daemon` accepte-t-il un démon dans une app signée ad hoc et dans le bac à sable ?
  2. Une app peut-elle savoir, depuis le bac à sable, que le démon est déjà actif ?
  3. Le démon garde-t-il ses droits root et son comportement ?
  
  Si la réponse à la question 1 est non, Thread Route reste installé par `installer.sh`, renommé, et le README le dit. Djoko en est informé avant la suite.

## 3. La publication

Un script par dépôt, `outils/publier.sh X.Y.Z`. Pour le pont Halo, il se trouve dans `apps/macos/outils/` ; le plan en fixe la place.

1. **Les vérifications :** `main` est propre et à jour avec GitHub, la version n'existe pas encore, les tests passent.
2. **Les numéros :**
   - la version (`CFBundleShortVersionString`) vient de `MARKETING_VERSION` dans `project.yml` ;
   - le numéro de compilation (`CFBundleVersion`), que compare Sparkle, est le nombre de commits de `main`. Il ne fait que croître.
3. **La compilation** se fait en Release, avec la même signature ad hoc qu'aujourd'hui, Thread Route compris.
4. **Le `.dmg`,** créé par `hdiutil`, contient l'app et un raccourci vers Applications. Il s'appelle `Maillage-Thread-X.Y.Z.dmg` ou `Halo-Compagnon-X.Y.Z.dmg`.
5. **La signature :** `sign_update` de Sparkle signe le `.dmg` avec la clé du trousseau. Puis `appcast.xml` est produit, avec l'adresse du `.dmg` dans la version publiée, sa taille, sa signature, les deux numéros, la version minimale du système (macOS 26.0) et les notes de version.
6. **Le contrôle d'anonymisation** passe sur les notes, `appcast.xml` et les textes de l'app.
7. **La publication sur GitHub :** l'étiquette `vX.Y.Z`, puis `gh release create`, avec le `.dmg` et `appcast.xml`.
8. **La remise :** le `.dmg` est copié sur le Bureau.

**Les notes de version** viennent d'un fichier `NOTES-VERSIONS.md` par app, en français et en anglais, dont une section par version.

**Le README de chaque app** dit :
- comment l'installer depuis les versions publiées ;
- ce que demande Gatekeeper à la première ouverture ;
- ce que fait la mise à jour automatique ;
- comment installer Thread Route ;
- pour Maillage Thread, que le passeur se compile à part (`outils/passeur.sh`), avec la propre équipe de chacun.

## 4. La répétition, avant toute publication

Sur le Mac, sans GitHub :
- un petit serveur HTTP local sert un faux flux et une fausse version 1.0.1, signée avec la vraie clé ;
- une app 1.0.0, compilée avec ce flux local, trouve la mise à jour, vérifie la signature, l'installe et redémarre en 1.0.1 ;
- un `.dmg` mal signé est refusé ;
- Thread Route installé par une app est vu par l'autre.

La répétition doit avoir réussi avant toute version publiée sur GitHub.

## 5. Tests et vérification

- **Les suites existantes** restent vertes en français et en anglais : Maillage Thread, Halo Compagnon, tests Python, tests hôte du pont, tests de Thread Route.
- **Tests nouveaux :**
  - l'`Info.plist` porte l'adresse du flux, la clé publique et les réglages de Sparkle ;
  - le moteur n'est pas démarré sous les tests ;
  - le calcul des numéros de version ;
  - la production d'`appcast.xml` à partir de valeurs inventées ;
  - l'état de Thread Route, dans ses quatre cas ;
  - la copie du démon conforme à sa source.
- **Avec Djoko, en vrai :**
  - l'installation depuis le `.dmg` du Bureau et le passage de Gatekeeper ;
  - le menu « Rechercher les mises à jour… » ;
  - l'installation de Thread Route et son approbation ;
  - la migration depuis `halo-routes` ;
  - l'accès au trousseau de Halo Compagnon après une mise à jour.

## 6. Limites

- **La première installation** passe par Gatekeeper : il faut autoriser l'app dans Réglages Système, section Confidentialité et sécurité.
- **Halo Compagnon** redemandera sans doute l'accès à sa clé du trousseau après une mise à jour, car la signature ad hoc change à chaque compilation.
- **Le passeur de Maillage Thread n'est pas distribué.** C'est une app « conçue pour iPad », que chacun signe avec sa propre équipe.
- **Une copie de l'app déjà téléchargée par quelqu'un d'autre** ne se met à jour qu'à partir de la première version qui contient Sparkle.
