# Déploiement de Maillage Thread et de Halo Compagnon

Spec du 06/10/2026. Djoko a validé la conception le même jour, puis l'a complétée le même jour : la signature des versions publiées par un certificat auto-signé stable ; la préparation d'une distribution large (section 7) ; le flux de versions dans le dépôt, avec des étiquettes propres à chaque app ; Thread Route installé par son installateur, après l'essai (section 2). Une copie identique se trouve dans le dépôt du pont Halo (`docs/superpowers/specs/2026-10-06-deploiement-design.md`).

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
| Signature du code | les versions publiées sont signées par **un certificat auto-signé stable**, `Djoko-cli Code Signing`, le même pour toutes les apps de Djoko ; les compilations de travail et les tests restent ad hoc |
| Gatekeeper | **la signature Ed25519 le remplace pour les mises à jour** : une mise à jour installée par l'app ne repasse pas par Gatekeeper ; la première installation, depuis le navigateur, y passe |
| Démon | renommé **Thread Route** (`thread-route`, étiquette `fr.djoko.thread.route`) |
| Livraison du démon | **installé par son installateur** (`installer.sh`), avec le mot de passe administrateur, qui fait aussi la migration depuis `halo-routes` ; les deux apps affichent son état et la commande |
| Source du démon | une seule, dans le dépôt du pont Halo ; Maillage Thread en garde une copie à l'identique |
| Flux de versions | **dans le dépôt** (`appcast.xml`), lu à son adresse brute sur `main` ; il garde toutes les versions publiées |
| Étiquettes | **propres à chaque app** : `maillage-vX.Y.Z` et `compagnon-vX.Y.Z` |
| Premières versions | Maillage Thread **1.0.0**, Halo Compagnon **1.0.0** |
| Remise | le `.dmg` de chaque app sur le Bureau de Djoko |

**Contraintes qui restent :**
- **le certificat de signature** est auto-signé, de nom neutre (`Djoko-cli Code Signing` : ni nom, ni adresse, ni équipe), valide 10 ans ; sa clé privée est dans le trousseau de Djoko, jamais dans un dépôt. Seule la compilation de publication (`publier.sh`) signe avec lui ; elle le trouve par son nom dans le trousseau, au moment de publier. Le dépôt peut écrire ce nom ; jamais une empreinte ni un identifiant d'équipe. Djoko le crée une fois, et en garde un `.p12` protégé en lieu sûr, avec la clé Ed25519 ;
- les compilations de travail et les tests restent signés ad hoc ; pas de certificat Developer ID, pas de notarisation (elle est préparée, désactivée : section 7), jamais de `Local.xcconfig`, aucun identifiant d'équipe dans un fichier commité ;
- elles restent dans le bac à sable ;
- aucune donnée réelle dans ce qui est publié : le contrôle d'anonymisation passe avant chaque publication.

## 1. Le moteur de mise à jour, dans chaque app

- **Sparkle 2**, par le gestionnaire de paquets Swift, à une version figée, déclaré dans `project.yml` (XcodeGen).
- **Le flux de versions** est un fichier `appcast.xml` du dépôt : `appcast.xml` à la racine de Maillage Thread, `apps/macos/appcast.xml` dans le pont Halo. L'app le lit à une adresse fixe, celle du fichier brut sur `main` :
  - `https://raw.githubusercontent.com/Djoko-cli/maillage-thread/main/appcast.xml` ;
  - `https://raw.githubusercontent.com/Djoko-cli/benq-screenbar-halo-matter/main/apps/macos/appcast.xml`.

  Il garde toutes les versions publiées, la plus récente en tête. Les `.dmg` restent dans les versions publiées sur GitHub : l'adresse de chacun, dans le flux, est celle de sa version publiée.
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
- **L'installation par son installateur** (décision de Djoko du 06/10, après l'essai ci-dessous) :
  - `installer.sh`, renommé, compile et teste le démon, puis le pose avec le mot de passe administrateur ; c'est Djoko qui le lance ;
  - les deux apps affichent son état, lu auprès de macOS depuis le bac à sable (`SMAppService.statusForLegacyPlist`) : actif, désactivé dans Réglages Système, absent, ou `halo-routes` encore installé ; et la commande d'installation ;
  - l'installateur arrête l'ancien démon (`halo-routes`) et attend, jusqu'à 25 secondes, que launchd l'ait déchargé avant de retirer ses fichiers ; si l'attente expire, il s'arrête sans rien retirer ni poser ; il en va de même du démon actuel avant son remplacement ;
  - une nouvelle version du démon s'installe en relançant `installer.sh` : la mise à jour d'une app ne le touche pas.
- **Une seule source :** le code du démon vit dans le dépôt du pont Halo. Maillage Thread en garde une copie à l'identique, avec un script de synchronisation et un test qui vérifie que la copie correspond à sa source, à une révision notée.
- **L'essai (06/10).** Un essai jetable a montré que, dans une app du bac à sable, macOS refuse tout de suite d'inscrire un démon qui n'y est pas (« SMAppService target executable must be sandboxed because the app is sandboxed »), que l'app soit signée ad hoc ou par un certificat auto-signé. Une app du bac à sable voit en revanche l'état d'un démon posé par un installateur. D'où le repli ci-dessus, que Djoko a validé.

## 3. La publication

Un script par dépôt, `outils/publier.sh X.Y.Z`. Pour le pont Halo, il se trouve dans `apps/macos/Outils/`.

1. **Les vérifications,** avant tout test : `main` est propre et à jour avec GitHub, l'étiquette de la version n'existe pas encore, ni la version dans le flux, qu'elle dépasse ; l'auteur et le committer Git sont `Djoko-cli`, à l'adresse noreply de GitHub ; le trousseau porte un seul certificat de ce nom, dont le sujet n'est que ce nom ; le contrôle d'anonymisation est sur le Mac. Puis les tests passent. Hors répétition, sans le contrôle d'anonymisation, rien n'est publié.
2. **Les numéros :**
   - la version (`CFBundleShortVersionString`) vient de `MARKETING_VERSION` dans `project.yml` ;
   - le numéro de compilation (`CFBundleVersion`), que compare Sparkle, est le nombre de commits de `main`. Il ne fait que croître.
3. **La compilation** se fait en Release. Puis l'app est signée par le certificat `Djoko-cli Code Signing`, son code imbriqué d'abord (Sparkle et ses services), avec le runtime renforcé ; sans notarisation, l'app lève la validation des bibliothèques (`com.apple.security.cs.disable-library-validation`) : un certificat auto-signé n'a pas d'équipe, et le runtime renforcé refuserait sinon de charger les cadres de l'app, qui s'arrêterait au lancement (constaté le 06/10) ; avec la notarisation (`NOTARISER=1`, qui suppose un Developer ID), ce droit est refusé ; aucune autre exception du runtime renforcé (`com.apple.security.cs.*`) n'est admise ; les binaires n'ont ni symboles de débogage ni chemin de source personnel (symboles retirés, chemins ramenés à des noms neutres, `dSYM` à part et jamais publié) ; `publier.sh` règle l'identité en un seul endroit, une variable. L'exigence de signature de l'app (`codesign -d -r-` : son identifiant et ce certificat) est alors la même d'une version à l'autre. Une étape de notarisation est écrite, désactivée par défaut (section 7).
4. **Le `.dmg`,** créé par `hdiutil`, contient l'app, un raccourci vers Applications et le texte de la licence MIT de Sparkle (`Sparkle-LICENSE.txt`). Avant `hdiutil`, tout son contenu, binaires compris, est refusé s'il porte le dossier personnel, `/Users/` ou le nom du compte macOS. Il s'appelle `Maillage-Thread-X.Y.Z.dmg` ou `Halo-Compagnon-X.Y.Z.dmg`.
5. **La signature :** `sign_update` de Sparkle signe le `.dmg` avec la clé du trousseau. Puis la version entre en tête du flux du dépôt, avec l'adresse du `.dmg` dans sa version publiée, sa taille, sa signature, les deux numéros, la version minimale du système (macOS 26.0 pour Maillage Thread, 15.0 pour Halo Compagnon) et les notes de version ; les versions d'avant restent.
6. **Le contrôle d'anonymisation** passe sur les notes, le flux, le message du commit du flux, les textes de l'app et le contenu du `.dmg`. Il est obligatoire : hors répétition, son absence est un refus.
7. **La publication sur GitHub :** juste avant, l'état est relu (`main` inchangée et à jour, `gh` connecté, `git push --dry-run` qui passe, version publiée absente : toute autre réponse de `gh release view` est un refus). Puis `gh release create --target <commit vérifié>`, avec le `.dmg`, crée l'étiquette de l'app (`maillage-vX.Y.Z` ou `compagnon-vX.Y.Z`) et la version publiée. Chaque geste fait est noté dans `gestes.txt`, dans le dossier des produits, et la reprise, si le script s'arrête en route, part de ce fichier, geste par geste. Puis le flux est commité sur `main` (un `git add` de ce seul fichier, un message en français sans accents terminé par la ligne Co-Authored-By) et poussé aussitôt.
8. **La remise :** le `.dmg` est copié sur le Bureau.

**Les notes de version** viennent d'un fichier `NOTES-VERSIONS.md` par app, en anglais puis en français, dont une section par version.

**Le README de chaque app** dit :
- comment l'installer depuis les versions publiées ;
- ce que demande Gatekeeper à la première ouverture ;
- ce que fait la mise à jour automatique ;
- comment installer Thread Route ;
- pour Maillage Thread, que le passeur se compile à part (`outils/passeur.sh`), avec la propre équipe de chacun.

## 4. La répétition, avant toute publication

Sur le Mac, sans GitHub :
- un petit serveur HTTP local sert le flux comme le ferait l'adresse brute du dépôt, et les `.dmg` comme les versions publiées ; il sert une version 1.0.1, signée avec la vraie clé et le vrai certificat ;
- une app 1.0.0, compilée avec ce flux local, trouve la mise à jour, vérifie la signature, l'installe et redémarre en 1.0.1 ;
- un `.dmg` mal signé est refusé ;
- Thread Route, posé par son installateur, est vu par les deux apps.

La répétition doit avoir réussi avant toute version publiée sur GitHub.

## 5. Tests et vérification

- **Les suites existantes** restent vertes en français et en anglais : Maillage Thread, Halo Compagnon, tests Python, tests hôte du pont, tests de Thread Route.
- **Tests nouveaux :**
  - l'`Info.plist` porte l'adresse du flux, la clé publique et les réglages de Sparkle ;
  - le moteur n'est pas démarré sous les tests ;
  - le calcul des numéros de version ;
  - la signature du code, dans l'ordre, et la notarisation, qui ne s'exécute pas par défaut et s'arrête proprement si on l'active sans profil ;
  - la production d'`appcast.xml` à partir de valeurs inventées, l'ajout d'une version à un flux existant, les étiquettes propres à l'app ;
  - l'état de Thread Route, dans ses quatre cas ;
  - la copie du démon conforme à sa source.
- **Avec Djoko, en vrai :**
  - l'installation depuis le `.dmg` du Bureau et le passage de Gatekeeper ;
  - le menu « Rechercher les mises à jour… » ;
  - l'installation de Thread Route par son installateur, et son état dans les deux apps ;
  - la migration depuis `halo-routes` ;
  - l'accès au trousseau de Halo Compagnon après une mise à jour.

## 6. Limites

- **La première installation** passe par Gatekeeper : il faut autoriser l'app dans Réglages Système, section Confidentialité et sécurité.
- **Halo Compagnon** redemandera sans doute l'accès à sa clé du trousseau au passage d'une compilation de travail, signée ad hoc, à une version publiée, signée par le certificat (et inversement). D'une version publiée à la suivante, l'exigence de signature ne change pas.
- **Le passeur de Maillage Thread n'est pas distribué.** C'est une app « conçue pour iPad », que chacun signe avec sa propre équipe.
- **Une copie de l'app déjà téléchargée par quelqu'un d'autre** ne se met à jour qu'à partir de la première version qui contient Sparkle.

## 7. Distribution large

Préparée, pas activée. Seule l'étape de notarisation de `publier.sh` est écrite, désactivée par défaut (`NOTARISER=1` l'active, avec `PROFIL_NOTARISATION`, le profil que `notarytool store-credentials` range dans le trousseau : jamais d'identifiant Apple, de Team ID ni de mot de passe dans un fichier commité). Elle signe et soumet le `.dmg` (`xcrun notarytool submit --wait`), l'agrafe (`xcrun stapler staple`) et l'évalue (`spctl --assess`), avant la signature Ed25519.

Ce qu'une distribution au-delà du Mac de Djoko demande :
- **Une app non notarisée** doit être autorisée dans Réglages Système, Confidentialité et sécurité, « Ouvrir quand même », puis le mot de passe : depuis macOS 15, le clic droit ne suffit plus.
- **Ed25519 ne protège pas la première installation :** un compte GitHub volé suffirait à remplacer le `.dmg` qu'installent les nouveaux venus.
- **Thread Route sur d'autres Macs** dépend probablement d'une signature Apple.
- **Le passeur n'est pas distribuable.**
- **La licence de Sparkle** (MIT) est respectée : le texte est livré dans le `.dmg`, et les README de chaque dépôt, en français et en anglais, ont une section « Crédits ».
- **Les Macs Intel** demandent un binaire universel, ou d'annoncer Apple Silicon seulement. La compilation de publication est déjà universelle (`arm64` et `x86_64`), mais elle n'a jamais été essayée sur un Mac Intel.
- **La marche suivante** est le programme développeur d'Apple : Developer ID et notarisation. La transition est transparente par Sparkle, puisque la clé Ed25519 ne change pas.
- **À prévoir :** un déploiement progressif, un canal bêta, les mises à jour delta, une mention de confidentialité (GitHub voit les adresses IP), et les tickets GitHub pour les signalements.
