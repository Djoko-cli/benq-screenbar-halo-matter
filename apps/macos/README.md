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
  et `id=1 json 1`. Sans `hello` en 2 s : trois renvois, puis un essai toutes
  les 30 s (« mode téléchargement ? »). `Commande inconnue : "id=1"` : ancien
  firmware, console seule (lignes sans `id`).
- `json ping` après 10 s sans autre commande ; silence de 3 × max(période, 2 s)
  (hors commande de banc) : `json 1`, puis fermeture et réouverture sans `hello` sous 5 s.
- Ré-énumération (redémarrage, `reboot`, câble) : IOKit signale le départ et
  l'arrivée des ports (`IOServiceAddMatchingNotification` sur `IOSerialBSDClient`) ;
  l'app rouvre après 300 ms, puis 1 s, 2 s, 5 s, et retrouve la même carte par
  son numéro de série USB (sa MAC).
- **Libérer le port** (⇧⌘L, bouton ⏏) : `json 0`, fermeture, pas de réouverture
  avant « Reconnecter » : `pio run -t upload` peut flasher.
- Une commande en vol à la fois ; sans `reponse` sous 3 s : « sans réponse »,
  `json etat`, jamais de réémission. Curseurs : une commande toutes les 150 ms
  au plus pendant le glissement (les valeurs en file sont fusionnées), la
  valeur finale au relâchement.
- Console : 127 octets au plus préfixe compris (jugé avec le plus long `id`
  possible), ASCII imprimable, jamais de JSON ni de RS ; confirmation pour
  `reboot`, `decommission`, `erase`, `wifi`, `addr`, `chan`, `xo`, `debit`,
  `amble`, `aw`, `holtek`, `regcfg`, `lampe oublie`, `lampe adresse <x>`,
  `lampe stats raz`, `matter med|maxint|reprise auto`, `json cle nouvelle|efface` ;
  `json 0` refusé (passer par « Libérer le port ») ; la clé de `json cle` est
  masquée dans la console et le journal.

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
│   ├── Interpretation/          sens décodé et libellés français
│   └── Transport/               protocole Transport (ouvrir, envoyer, fermer, flux d'octets)
├── HaloProtocoleTests/          Swift Testing : tramage, décodage de chaque ligne d'exemple de la spec, corrélation, session, courbes, correspondances, fichier de démo
├── HaloCompagnon/               l'app
│   ├── Serie/                   PortSerie (POSIX, DTR/RTS), TransportSerie (DispatchSource), SurveillantUSB (IOKit)
│   ├── Demo/                    ScriptDemo, SimulateurDemo (acteur), TransportDemo
│   ├── Modele/                  Pont (@Observable, acteur principal) : relie transport, récepteur, moteur, état, journaux
│   ├── Vues/                    les quatre écrans et leurs composants
│   └── Ressources/demo-halo.jsonl
├── HaloCompagnonTests/          bout en bout sur la carte simulée (connexion, commande livrée, refus, chronologie entière accélérée, redémarrage)
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
