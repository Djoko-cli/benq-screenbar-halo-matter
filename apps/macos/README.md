# Halo Compagnon (macOS)

App SwiftUI native qui supervise le pont ESP32-C6 de la BenQ ScreenBar Halo 1
par le port USB, selon le protocole machine de
[`docs/PROTOCOLE-JSON.md`](../../docs/PROTOCOLE-JSON.md) (v1). Le même code de
protocole servira plus tard à l'app iOS, par UDP sur Thread (section 10).

> **État.** Le mode machine arrive avec le firmware 0.4.0, écrit en parallèle ;
> l'app décode déjà sans erreur les lignes que produit son formateur dans les
> tests hôte (`tools/test_halo1.sh`), mais rien n'a encore été essayé avec la
> carte : voir « À vérifier au banc ». Face à un firmware plus ancien, l'app le
> détecte (`Commande inconnue : "id=1"`) et reste en console seule. Tout se
> voit en **mode démo**, sans matériel.

## Les quatre écrans

| Écran | Contenu |
|---|---|
| **Tableau de bord** | Consigne et état cru côte à côte (écarts et champs à livrer en orange), phase du pilote, tranches actives ; liaison avec la lampe (accusés, livraisons, abandons) ; module BM5602 (mode, jauges délais / surdité / relances sans guérison, bandeau **EN PANNE**) ; voyant animé d'après `status_led.h` (et `led test`) ; Thread et Matter (rôle, RSSI du parent, SRP, fabriques, code manuel et QR si la carte n'est pas en service) ; abonnements ; santé du lien série (lignes abîmées, fragments, trous de `n`, JSON perdus côté carte, dernière ligne rejetée) ; versions, identité, capacités ; démarrage (`boot`, cause, `up_s`) et système. |
| **Trames en direct** | Événements `rx`, `tx`, `livraison`, `relance` (et `module`, `intent`, `abonnement`, `thread`, `led`, `reponse`...) avec leur sens décodé (« Luminosité A5 (niveau 180) · les deux · allumée », « MAX_RT (sans accusé) en 11476 µs »...). Filtres par type, sans les accusés de la lampe, échecs seulement, recherche ; détail et JSON de chaque trame. CRC faux en gris, lignes anciennes (avant le `hello`) en italique. Menu Flux : `json trames 0/1`, `json log 0/1`, `lampe ecoute 0/1`. |
| **Graphiques** | Swift Charts, calculés comme la section 8 le dit : différences de blocs `compteurs` par fenêtre de 10 s ou 1 min, nouveau segment sur différence négative, `raz` ou redémarrage. Taux de perte TX et son complément, consignes abandonnées (et marqueurs des `livraison` `abandon`), CRC faux par minute et en part des trames (seuils du déluge), refus en réception (`rearm_hors_rx` avec le seuil de surdité, `tx.fifo`, `garde.refus`), relances empilées par cause (marqueurs `relance`, `module`), santé Matter (abonnés actifs, RSSI du parent, changements de rôle). `lampe stats raz` avec confirmation. |
| **Commandes et console** | Allumer, éteindre, lampes (`lampe mode`, `lampe avant/arriere on/off`), bouton A, `lampe sync` ; curseur de luminosité en niveau Matter avec la correspondance gamma de la carte (`gamma_c`) tracée (niveau → brut 4C..FE) ; curseur de température du plus froid au plus chaud (mireds, Kelvin nominaux, valeur brute) ; valeurs brutes (`lampe lum`, `lampe temp`) ; commandes récentes et leur sort (acceptée, livrée, abandonnée, sans réponse...). Console brute : chaque ligne part avec un `id`, le texte reçu entre `reponse debut` et `fin` lui est rattaché, `reponse` et `livraison` y sont rendues lisibles. |

## Construire, tester, lancer

Prérequis : macOS 15 ou plus, Xcode 16 ou plus (développé avec Xcode 27),
[XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`).
Aucune dépendance tierce. Le projet Xcode est généré : seul `project.yml` est suivi.

```sh
cd apps/macos
xcodegen generate
xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' build
xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' test
```

Swift 6 (concurrence stricte complète), avertissements traités comme des
erreurs : la compilation se fait sans aucun avertissement.

Lancer : ouvrir `Halo Compagnon.app` (dans `DerivedData/.../Build/Products/Debug/`),
ou `xed .` puis ⌘R. Arguments de lancement utiles :

```sh
open "…/Halo Compagnon.app" --args -demo -ecran trames   # démo directe, écran choisi
```

(`-ecran` : `tableau`, `trames`, `graphiques`, `commandes`.)

L'app **n'ouvre jamais un port seule** : il faut choisir une source dans la
barre latérale (le port Espressif, VID 303A, est proposé en premier) ou le
mode démo (⇧⌘D).

## Langues : français et anglais

L'app parle français (langue de développement : les clés des catalogues sont
les textes français) et anglais. **Réglages** (⌘,) › **Langue** : « Langue du
système » (par défaut), *English* ou *Français*. Le choix est gardé dans les
préférences de l'app (`langue`).

- **À chaud** : le contenu des fenêtres change tout de suite, sans relancer ni
  perdre l'écran, les filtres ou la console. Les vues (`Text("...")`) lisent
  la locale de l'environnement ; les textes calculés (sens décodé, libellés,
  notes, erreurs, menus de l'app) passent par `Localisation` (framework), qui
  est observable : une vue qui en a lu un se redessine. Le sens décodé du
  journal des trames est recalculé (la recherche suit), le bandeau d'alerte et
  les marqueurs des courbes aussi.
- **Au prochain lancement** : ce que macOS dessine lui-même (menus
  Halo Compagnon, Édition, Fenêtre, boîtes du système), qui suit
  `AppleLanguages` de l'app ; le choix l'écrit. Réglages Système (Langue et
  région › Applications) écrit au même endroit : la valeur qui s'y trouvait
  avant le premier choix *English*/*Français* est gardée
  (`AppleLanguagesAvantChoix`) et « Langue du système » la rend (ou retire
  `AppleLanguages` s'il n'y en avait pas). Les Réglages le disent.
- **Gardent leur langue jusqu'au texte suivant** (les Réglages le disent
  aussi) : les lignes déjà écrites dans la console (c'est un journal), la
  raison d'une reconnexion ou d'une erreur de port dans la barre latérale, la
  dernière ligne rejetée et l'erreur de saisie de la console. Ces textes
  sont faits au moment de l'événement, en partie de textes du système
  (`strerror`, erreurs de décodage).
- **Langue du système** : la première des langues préférées que l'app sait
  servir (`en-GB` donne l'anglais) ; aucune (allemand...) : le français, comme
  AppKit, qui retombe sur la langue de développement.
- **Formats** (heures, nombres, octets, dates relatives) : la langue choisie
  avec la région de l'utilisateur, comme macOS pour une langue choisie app par
  app (anglais en France : `en_FR`, 24 h, virgule décimale, « kB » et non
  « ko »). Les quantités ont leurs séparateurs de milliers ; les identifiants
  (`id`, numéros de paquet, versions), les kelvins, les microsecondes et
  l'hexa restent bruts, sur tous les écrans.
- **Termes techniques intacts** dans les deux langues : champs et valeurs JSON
  (`lum`, `temp`, `raz`...), commandes de la CLI (`lampe stats raz`,
  `json trames 0`), hexa, unités. Seules exceptions, les valeurs à liste
  fermée aux noms français (`ValeurFirmware`) : cause du démarrage (`reset` :
  `mise_sous_tension` → *power-on*...), `build`, sort du bouton A d'un
  `intent`, origine, mode et verdicts des reprises d'abonnements ; une valeur
  inconnue reste brute. Lexique anglais : consigne → *target*, état
  cru → *believed state*, livraison → *delivery*, accusé → *ack*, relance du
  module → *module restart*, redémarrage de la carte → *reboot* (comme la
  commande `reboot`), désappairage → *unpairing*, voyant → *status
  LED*, tranche → *slice*, bail → *lease*, EN PANNE → *DOWN*. Casse : phrase
  en français ; en anglais, *Title Case* pour les titres (écrans, cartes,
  sections, menus, boutons, alertes), casse de phrase pour le texte courant,
  les libellés de ligne, les cases à cocher et les pastilles.

Catalogues (String Catalogs) : `HaloProtocole/Localizable.xcstrings`
(framework : sens décodé, libellés, erreurs, notes de session, avec pluriels),
`HaloCompagnon/Ressources/Localizable.xcstrings` (app) et
`HaloCompagnon/Ressources/Titres.xcstrings` (titre de section dont le texte
français sert déjà de libellé, avec une autre casse anglaise). Les clés sont
extraites par le compilateur (`SWIFT_EMIT_LOC_STRINGS`) : Xcode les ajoute en
construisant ; en ligne de commande, après un changement de texte :

```sh
I=<DerivedData>/Build/Intermediates.noindex/HaloCompagnon.build/Debug
xcrun xcstringstool sync HaloProtocole/Localizable.xcstrings \
    --stringsdata $I/HaloProtocole.build/Objects-normal/arm64/*.stringsdata
xcrun xcstringstool sync HaloCompagnon/Ressources/*.xcstrings \
    --stringsdata $I/HaloCompagnon.build/Objects-normal/arm64/*.stringsdata
```

puis traduire les nouvelles clés. Les tests (`LocalisationTests`) vérifient
que chaque clé a son anglais (pluriels complets, mêmes valeurs interpolées),
qu'aucune n'est périmée et que le code et les catalogues vont ensemble. Tester
toute l'app dans l'autre langue : `xcodebuild ... test -testLanguage en -testRegion US`.

## Mode démo

La source « Mode démo » remplace le port série par une carte simulée
(`HaloCompagnon/Demo/`). Elle rejoue `HaloCompagnon/Ressources/demo-halo.jsonl`,
une chronologie de 195 s construite à partir des exemples de la section 12 :

| t | Événement |
|---|---|
| 0 s | instantané de connexion de 12.1, mot pour mot (`hello`, `config`, `etat`, `compteurs`, `reseau`) |
| 6 s | Apple Home règle la luminosité (`intent`, niveau 127), 3 paquets accusés, `livraison`, voyant vert |
| 14 s | **molette de la télécommande** : trame de service, 26 trames `lum` et leurs accusés, 2 CRC faux |
| 22 s, 27 s | molette de température, bouton A (3 copies du même appui) |
| 33 s | Matter : luminosité et température, deux tranches livrées |
| 40-58 s | **lampe débranchée** : commande Matter en échec (5 MAX_RT par tour, 3 tours, `livraison` `abandon` `injoignable`, voyant rouge) ; vos commandes échouent aussi pendant cette fenêtre |
| 64-66 s | un log IDF coupe une ligne machine (ligne abîmée + fragment), deux lignes perdues (trou de `n`) |
| 74-84 s | la puce devient sourde (réarmements hors RX), **relance du module** `sourde` |
| 96-105 s | le parent Thread disparaît (`thread` `detached`, abonnement terminé, voyant orange), puis revient |
| 116-150 s | délais TX à chaque commande, trois relances sans guérison, **`module` `panne`** (EN PANNE, rouge fixe) |
| 176 s, 182 s | `module` `retabli`, puis Apple Home éteint la lampe |

Les blocs périodiques ne sont écrits dans le fichier que lorsqu'ils changent :
la carte simulée les ré-émet à la cadence de la session (`json periode`,
`json compteurs`, `json reseau`, `hb` si l'état est coupé). Elle répond aux
lignes de l'app comme le firmware 0.4.0 le ferait : `json 1` (écho et invite
en mode humain avant), instantané, `json ping`, bail de 30 s, `json etat`,
`json hello`, commandes `lampe` asynchrones (`reponse` `accepte`/`differe`/
`usage`/`refuse`, 3 `tx`, `livraison` portant les `id`, voyant), commandes
historiques (`reponse debut`, texte, `reponse fin`), `led test`,
`lampe stats raz`, `reboot`. Les commandes de l'app surchargent la consigne et
les compteurs jusqu'au prochain changement venu du fichier.

À la fin de la chronologie, la carte « redémarre » : le flux se ferme comme
une ré-énumération USB, l'app rouvre après 300 ms et voit un nouveau `boot`
(états vidés, nouveau segment de courbes), et la démo repart.

Format du fichier (JSON-lines, ASCII) : une ligne machine telle que la carte
l'émet entre RS et LF (objet avec `v`), ou `{"texte": "...", "ms": N}` (ligne
de texte humain), ou `{"demo": "<directive>", "ms": N, ...}` (`lampe_debranchee`,
`lampe_rebranchee`, `ligne_coupee`, `saut_n`, `entete`). Le fichier se
régénère avec `python3 Outils/generer_demo.py` ; le script vérifie que les
premières lignes égalent les exemples de 12.1 et que ses trames `brut`
égalent celles de 12.3 (portage de `encodeAir` et du CRC).

## Connexion à la carte (section 3)

- Port `/dev/cu.*` (jamais `/dev/tty.*`) ouvert en `O_RDWR | O_NOCTTY | O_NONBLOCK`,
  puis `ioctl(TIOCEXCL)` : `pio device monitor` ne peut pas s'y attacher en même temps.
- **DTR et RTS à 0 dans un seul `ioctl(TIOCMSET)`** juste après l'ouverture,
  et plus jamais touchés : jamais l'état RTS=1, DTR=0 qui redémarre le C6.
  `HUPCL` retiré (la fermeture ne touche pas aux lignes), `cfmakeraw`, 8N1,
  `CLOCAL | CREAD`, 115200. Code : `HaloCompagnon/Serie/PortSerie.swift`.
- À l'ouverture : tout ce qui précède le premier LF est jeté, puis `0x15 0x0A`
  et `id=<n> json 1`. Sans `hello` en 2 s : trois renvois, puis un essai toutes
  les 30 s (« mode téléchargement ? »). `Commande inconnue : "id=1"` : ancien
  firmware, console seule (lignes sans `id`).
- La session s'établit au `hello` de cette tentative, et la file de commandes
  ne repart qu'à la `reponse fin` du `json 1` (une seule commande en vol, 6.5).
  Une réponse sans `hello` (hello perdu ou coupé par un log, `ok:false`,
  `cadence`) n'établit rien : `json 1` repart 2 s après. Réponse perdue après
  le `hello` : tenue pour perdue au bout de 3 s, la file repart.
- `json ping` après 10 s sans autre commande, ou au tiers du bail s'il est plus
  court (`json 1 bail 10` tapé dans la console) ; silence de 3 × max(période, 2 s)
  (hors commande de banc) : `json 1`, puis fermeture et réouverture sans `hello`
  sous 5 s. Avant tout `json 1` en cours de session (silence, bail échu,
  redémarrage), la commande en vol est marquée perdue ; au redémarrage, les
  livraisons attendues aussi.
- Ré-énumération (redémarrage, `reboot`, câble) : IOKit signale le départ et
  l'arrivée des ports (`IOServiceAddMatchingNotification` sur `IOSerialBSDClient`) ;
  l'app rouvre après 300 ms, puis 1 s, 2 s, 5 s, et retrouve la même carte par
  son numéro de série USB (sa MAC). Après 40 essais minutés (~3 min), elle
  cesse d'essayer à l'aveugle mais rouvre toujours au retour du port.
- **Libérer le port** (⇧⌘L, bouton ⏏) : `json 0`, puis fermeture une fois la
  file de sortie du tty vide (`TIOCOUTQ`, 300 ms au plus : le descripteur est
  `O_NONBLOCK` et la fermeture jetterait le reste), pas de réouverture avant
  « Reconnecter » : `pio run -t upload` peut flasher. « Déconnecter », le
  changement de source et la fin de l'app envoient aussi `json 0`, sauf pendant
  une commande de banc (la CLI ne lit plus).
- Port ouvert : l'app tient une activité `ProcessInfo` (pas d'App Nap), sinon
  le ping pourrait manquer le bail de 30 s fenêtre cachée.
- Une commande en vol à la fois, 20 lignes par seconde au plus (50 ms entre
  deux lignes, refus `cadence` de 6.5) ; sans `reponse` sous 3 s : « sans
  réponse », `json etat`, jamais de réémission. Un `debut` (même arrivé après
  ce verdict) fait une commande de banc : rien ne part, ni ping ni `json 1` de
  silence, jusqu'à sa `fin`. Curseurs : une commande toutes les 150 ms
  au plus pendant le glissement (les valeurs en file sont fusionnées), la
  valeur finale au relâchement.
- Console : 127 octets au plus préfixe compris (jugé avec le plus long `id`
  possible), ASCII imprimable, jamais de JSON ni de RS ; confirmation pour
  `reboot`, `decommission`, `erase`, `wifi`, `addr`, `chan`, `xo`, `debit`,
  `amble`, `aw`, `holtek`, `regcfg`, `lampe oublie`, `lampe adresse <x>`,
  `lampe stats raz`, `matter med|maxint|reprise auto`, `json cle nouvelle|efface` ;
  `json 0` refusé (passer par « Libérer le port ») ; la clé de `json cle` est
  masquée partout où elle pourrait s'afficher (console, journal, lignes
  rejetées, commandes récentes ; casse et espaces quelconques, toute suite de
  64 chiffres hexa) et n'entre jamais dans l'historique de saisie. Avant la
  réponse au `json 1`, une ligne tapée attend en file avec son `id`.

### Bac à sable : oui, avec `com.apple.security.device.serial`

L'app est sandboxée (`HaloCompagnon/HaloCompagnon.entitlements`) avec le droit
`com.apple.security.device.serial`, celui que la spécification prévoit (3.1),
et rien d'autre. Raisons : l'app n'a besoin que des
ports série et du registre IOKit (lecture, permise dans le bac à sable) ; le
droit série couvre `open`, `TIOCEXCL`, `TIOCMSET` et `termios` sur `/dev/cu.*` ;
rien d'autre sur le disque ni le réseau n'est ouvert ; et la même app pourra
être signée et distribuée sans changement. Si le banc montrait un refus du
bac à sable sur un `ioctl`, retirer `com.apple.security.app-sandbox` suffit
(aucun code ne dépend du bac à sable).

## Architecture

```
apps/macos/
├── project.yml                  projet XcodeGen (3 cibles, 1 schéma)
├── HaloProtocole/               framework sans AppKit ni SwiftUI : le protocole, indépendant du transport
│   ├── Tramage/                 RS + JSON + LF sur octets bruts (2.4), classement du texte (2.5)
│   ├── Messages/                un Codable par (t, bloc), énumérations tolérantes (cas inconnu), décodeur
│   ├── Commandes/               lignes id=<n> (2.6), règles de la console et liste blanche (6.4, 10.5), corrélation (6.2-6.5)
│   ├── Session/                 machine d'état de la session (3.3-3.6) : json 1, renvois, ancien firmware, ping, silence, redémarrage, continuité de n
│   ├── Etat/                    dernier instantané de chaque (t, bloc), datation par ms et l'ancre du hello
│   ├── Courbes/                 calculs de la section 8
│   ├── Correspondances/         niveau Matter <-> brut (gamma), mireds <-> temp (portage de halo1_map.cpp)
│   ├── Interpretation/          sens décodé et libellés
│   ├── Localisation/            langue en vigueur (observable), choix du réglage, locale des formats
│   ├── Localizable.xcstrings    textes du framework (français source, anglais)
│   └── Transport/               protocole Transport (ouvrir, envoyer, fermer, flux d'octets)
├── HaloProtocoleTests/          Swift Testing : tramage, décodage de chaque ligne d'exemple de la spec, couverture des clés (aucun champ perdu), corrélation, session, courbes, correspondances, fichier de démo, catalogues et sens décodé dans les deux langues
├── HaloCompagnon/               l'app
│   ├── Serie/                   PortSerie (POSIX, DTR/RTS), TransportSerie (DispatchSource), SurveillantUSB (IOKit)
│   ├── Demo/                    ScriptDemo, SimulateurDemo (acteur), TransportDemo
│   ├── Modele/                  Pont (@Observable, acteur principal) : relie transport, récepteur, moteur, état, journaux ; réglage de la langue
│   ├── Vues/                    les quatre écrans, leurs composants, les Réglages
│   └── Ressources/              demo-halo.jsonl, catalogues de textes (Localizable, Titres)
├── HaloCompagnonTests/          bout en bout sur la carte simulée (connexion, commande livrée, refus, chronologie entière accélérée, redémarrage, changement de source), langue de l'app
└── Outils/generer_demo.py       générateur de la chronologie de démo
```

La couche protocole est du code pur : `RecepteurLignes`, `MoteurSession` et
`Correlateur` sont des `struct` qui prennent le temps en argument et
renvoient des effets (envoyer, rouvrir, redémarrage détecté...). `Pont` les
alimente depuis le transport et exécute les effets. Un transport est un
`Transport` (`HaloProtocole/Transport/Transport.swift`) : l'app iOS ajoutera
un `TransportUDP` (datagramme = une ligne, enveloppe `H1` de 10.4) sans
toucher au reste ; `PolitiqueCommandes.autoriseeADistance` applique déjà la
liste blanche de 10.5. Pour la rendre multiplateforme, ajouter iOS aux
plateformes de la cible `HaloProtocole` (elle n'importe que Foundation).

## Choix d'interprétation

- **Lignes anciennes** : ce qui arrive avant le `hello` de la session est
  journalisé (en italique) mais ne met pas à jour l'état (3.1, étape 5).
- **Datation** : tout est daté par `ms`, rapporté à l'heure locale lors du
  dernier `hello` (écart signé sur 32 bits, juste à travers le retour à zéro de `millis()`).
- **RS plus tôt dans une ligne** (début d'une ligne machine sans son LF) :
  compté comme ligne abîmée, jamais affiché comme texte ; le texte qui le
  précède reste du texte.
- **Livraison** : elle couvre toutes les commandes acceptées dont l'`id` ne
  dépasse pas le plus grand de ses `ids` (fusion des consignes, `ids_perdus`).
- **Numéros d'`id`** : croissants sur toute la vie de l'app, jamais remis à 1
  à la reconnexion : la liste des id en attente de la carte survit à une
  reconnexion, et une `livraison` tardive ne doit pas tomber sur une commande
  neuve de même numéro.
- **Fin perdue** : un bloc `etat` ou `hb` reçu après un `reponse debut` prouve
  que la boucle de la carte tourne de nouveau (elle n'émet rien pendant une
  commande) : la commande est close « fin perdue » et la file repart.
- **Changement de source** (autre port, démo) : moteur, `boot`, états,
  journal des trames et courbes repartent de zéro (pas de faux redémarrage).
- **Courbes** : un écart de plus de max(30 s, 3 périodes `compteurs`) entre
  deux blocs (app suspendue, veille) ouvre un segment, comme un `raz` ; le
  seuil du déluge vaut `deluge_trames` × `deluge_pct` % CRC faux par
  `fenetre_ms` (540 par minute aux valeurs par défaut), comme `halo1_watch.cpp`.
- **Capacités** (`hello.caps`, 5.1) : test du voyant sans `led`, `json trames`
  sans `trames`, `json log` sans `log` et cartes Thread et Matter sans
  `matter` ne sont pas proposés ; sans `lampe_async`, l'écran Commandes
  prévient que chaque commande `lampe` bloque la carte.
- **Redémarrage** vu hors `hello` (`etat`, `hb`) : `json 1` renvoyé ; vu dans
  le `hello` d'une nouvelle connexion : états vidés seulement.
- **Champs obligatoires** : l'enveloppe (`v`, `t`, `n`) et, par message, ce
  qui porte le sens (`reponse` : `id`, `etape`, `ok`, `code` ; `etat.lampe` :
  `consigne`, `cru` ; `rx.type`, `tx.verdict`, `livraison.issue`...) ; le
  reste est facultatif et un `null` n'invalide rien.

## À vérifier au banc (côté app)

- **U1, U2** : 50 ouvertures-fermetures par l'app, `boot` inchangé et `up_s`
  continu (le tableau de bord les montre ; un redémarrage apparaît en marqueur).
- **U3** : `reboot` depuis la console (confirmation) : ré-énumération,
  reconnexion en moins de 5 s, `hello` avec `reset` `logiciel`.
- **U4** : `chiplog` actif : les lignes abîmées et les fragments montent dans
  « Santé du lien série », aucune valeur fausse affichée.
- **U5** : Mac en veille ou app suspendue 60 s : fragments classés comme tels
  au réveil (`NSWorkspace.didWakeNotification`), bail échu puis `json 1` renvoyé.
- Le bac à sable face aux `ioctl` de `PortSerie` (`TIOCEXCL`, `TIOCMSET`).
