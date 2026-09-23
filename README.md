# BenQ ScreenBar Halo → Matter

Pilote une **BenQ ScreenBar Halo (1re génération)** depuis n'importe quelle app
domotique, en faisant passer un ESP32 pour sa télécommande 2,4 GHz.

L'ESP32 est un **nœud Matter** natif : pas de Homebridge, pas de broker MQTT.
Matter étant multi-admin, le même appareil peut être partagé entre plusieurs
écosystèmes à la fois — voir les réserves sur la certification plus bas.

## Ce qui est exposé

| Endpoint | Type Matter | Réglages |
|---|---|---|
| EP1 "Halo" | Color Temperature Light | marche/arret, luminosite, temperature 153-370 mireds |
| EP2 "Halo avant" | On/Off Light | lampe avant allumee (marche ET lampe avant) |
| EP3 "Halo arriere" | On/Off Light | lampe arriere allumee (marche ET lampe arriere) |
| ~~EP4 "Halo auto"~~ | On/Off Plug-in Unit | **desactive par defaut** (voir plus bas) : appui sur le bouton A (mode auto), revient seul a off apres 1 s (`matter impulsion <ms>`) ; un appui A sur la telecommande y fait la meme impulsion |

- Une trame radio ne porte qu'une valeur : les deux lampes partagent la
  luminosite et la temperature, d'ou un seul curseur de chaque sur EP1.
- Allumer EP1 retrouve la derniere selection de lampes, comme le bouton marche
  de la telecommande. Eteindre EP2 puis EP3 eteint la lampe.
- EP4, s'il est reactive, est ignore quand la lampe est eteinte, et quand il
  arrive avec un ordre marche ou lampe (commande de piece, tuile regroupee) :
  dans Apple Home, afficher les accessoires en tuiles separees.
- Les noms se donnent dans l'app. Les Kelvin (~6500 a ~2700 K) sont nominaux,
  non mesures.
- Dans `src/config.h` : `HALO1_SELECTORS_AS_LIGHTS 0` expose EP2 et EP3 en
  prises (un "eteins les lumieres" de piece n'y touche plus).
- Rien n'est emis vers la lampe au demarrage : le noeud reprend l'etat sauve,
  et seul un ordre (Matter ou `lampe ...`) fait emettre.

> **Passage a la 0.3.0 depuis EP1..EP4** (Halo, avant, arriere, auto) : flasher
> par-dessus le meme environnement (`pio run -e esp32c6thread -t upload` pour le
> noeud Thread d'Apple Home), sans `-t erase` ni `decommission` : l'appairage
> est garde, seul EP4 disparait.
>
> **Depuis l'ancienne disposition des endpoints** (alimentation, lumiere avant,
> halo arriere, capteur, mode auto) : il faut remettre le noeud en service.
> Retirer l'accessoire de chaque app, lancer `decommission` (ou appui long sur
> BOOT), puis l'ajouter a nouveau avec le code d'appairage (`matter`).

### EP4 "Halo auto" : desactive pour l'instant

Depuis la 0.3.0 (decision du 23/09), `HALO1_EXPOSE_AUTO` vaut 0 par defaut :
le bouton A n'est plus expose dans Matter. Le code reste, compile hors du
firmware : ni endpoint, ni miroir des A de la telecommande, ni reglage
d'impulsion (`matter impulsion` le dit, `matter` affiche « bouton A (EP4) :
desactive »). EP1 a EP3 gardent leurs numeros (EP4 etait cree en dernier).
Le bouton A reste accessible a la console : `lampe auto`.

Sur un noeud deja appaire, EP4 disparait de la liste des endpoints du noeud ;
la facon dont Apple Home retire la tuile « Halo auto » reste a verifier sur le
terrain.

Pour le **remettre**, ajouter `-DHALO1_EXPOSE_AUTO=1` aux `build_flags` de
l'environnement (par exemple `[env:esp32c6thread]` dans `platformio.ini`), ou
changer la valeur par defaut dans `src/config.h`, puis reflasher le meme
environnement (`pio run -e esp32c6thread -t upload`, sans effacement). EP4 revient
avec le meme numero, sa duree d'impulsion sauvee en NVS (`halo1/impulsion`) est
reprise, et l'app le montre comme un nouvel accessoire a ranger.

### Identite du noeud

Le cluster Basic Information (EP0) porte l'identite du produit, posee a chaque
demarrage avant `Matter.begin()` (valeurs dans `src/config.h`, macros
`MATTER_*`, surchargeables par `-D`) :

| Attribut | Valeur |
|---|---|
| VendorName | `Djoko-CLI` |
| ProductName | `Pont ScreenBar Halo` |
| NodeLabel | `Halo` (reecrit a chaque demarrage : un nom pose par un controleur dans cet attribut est remplace) |
| SerialNumber | `HALO1-` + l'adresse MAC d'usine en 12 chiffres hexa, unique par carte |
| HardwareVersion / HardwareVersionString | `1` / `ESP32-C6 SuperMini + BM5602` |
| SoftwareVersionString | `0.3.0-<commit>` (« Programme interne » dans Apple Home) |

- Le VID et le PID ne changent pas (`0xFFF1` / `0x8000`, certificat de test),
  ni le discriminateur et le code d'appairage : pas de remise en service. Une
  app peut mettre un moment a relire ces valeurs.
- `SoftwareVersionString` est la version du descripteur d'application
  (`esp_app_desc`), que `src/app_desc.c` remplace : sans lui, c'etait le commit
  du lib-builder d'Arduino (`6671d0b`). `FW_VERSION` se regle dans
  `platformio.ini` (`build_src_flags`) ; le commit vient de `tools/git_rev.py`,
  suivi de `-dirty` si un fichier suivi etait modifie a la compilation, ou si
  un fichier non suivi trainait dans `src/`, `include/` ou `lib/`.
- `matter` affiche ces valeurs telles que la pile les rapporte (lignes
  `identite` et `versions`), sauf le NodeLabel (valeur demandee, non relue),
  et signale toute valeur refusee. Le demarrage affiche
  `firmware 0.3.0-<commit>` et alerte si le descripteur lu dans l'image flashee
  differe. Sans carte, la meme version (`App version`) se lit avec :
  `pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32c6 image-info .pio/build/<env>/firmware.bin`.

## Matériel

| Élément | Rôle | Prix indicatif |
|---|---|---|
| **ESP32-C6 SuperMini** (ou C3 / S3) | MCU + Wi-Fi + Matter | ~5 € |
| **Module RF Holtek BM5602-60-1** | transceiver 2,4 GHz | ~3–4 $ |
| 100 nF + 10 µF | découplage de l'alim du module | — |

### Évite l'ESP32 classique

`CONFIG_ENABLE_CHIPOBLE` — la mise en service Matter par Bluetooth — **n'est pas
activé dans les bibliothèques précompilées d'Arduino pour l'ESP32 classique**
(la pile Bluedroid ne rentre pas). Il l'est sur C3, S3 et C6.

Concrètement :

| Cible | Mise en service | Taille firmware |
|---|---|---|
| **ESP32-C6 SuperMini** ✅ | BLE : le contrôleur fournit le Wi-Fi | 2,41 Mo |
| ESP32-C3 ✅ | BLE | 1,79 Mo |
| ESP32-S3 ✅ | BLE | 1,97 Mo |
| ESP32 classique ⚠️ | IP : Wi-Fi à donner avant (`wifi <ssid> <mdp>`) | 1,81 Mo |

L'ESP32 classique reste utilisable — la commande série `wifi` enregistre les
identifiants en NVS, sans recompilation — mais c'est une étape en plus.

### Pourquoi le BM5602 et pas un CC2500 ou un nRF24L01+

Le BenQ émet en **GFSK à 125 kbps**, avec un format de trame Enhanced
ShockBurst : préambule, adresse de 4 octets, PCF de 9 bits, CRC, et surtout
**auto-ACK matériel** — la lampe ne répond *que* dans le slot ACK.

| Puce | 125 kbps | Sync word 32 bits arbitraire | Auto-ACK ESB |
|---|---|---|---|
| nRF24L01+ / BK2425 | ❌ 250 k / 1 M / 2 M seulement | ✅ | ✅ |
| CC2500 | ✅ | ❌ le mode 32 bits répète le mot de 16 bits | ❌ à faire en logiciel |
| **BC5602 / BM5602-60-1** | ✅ 125 / 250 / 500 k | ✅ | ✅ |

Le nRF24 est éliminé d'office : il ne descend pas à 125 kbps. Le CC2500 y
arrive, mais il faudrait réimplémenter en logiciel le CRC nRF24 (calculé sur
adresse + PCF + payload, pas celui du CC2500), contourner la limite du sync
word, et produire l'ACK dans une fenêtre d'environ 130 µs. Faisable sur le
papier, très douloureux en pratique.

Le BC5602 est **exactement la puce qui se trouve dans la lampe et dans la
télécommande** (confirmé par les dossiers FCC et par un teardown du PCB). Tout
le protocole est géré en matériel.

### Où l'acheter

Le module est la principale friction du projet — ce n'est pas un composant de
grande distribution.

- [Best Modules Corp](https://www.bestmodulescorp.com/en/bm5602-60-1.html) —
  filiale de Holtek, ~3,10–4,28 $
- [Sourcengine](https://www.sourcengine.com/part-info/BM5602-60-1-145144816391)
- Distributeurs Holtek officiels
- [Fiche produit Holtek](https://www.holtek.com/page/vg/BM5602-60-1)

Prends-en deux : à ~4 $ pièce, ça évite de se demander si le module est mort
quand quelque chose ne marche pas.

## Câblage

Voir [docs/WIRING.md](docs/WIRING.md). En résumé, sur ESP32-C6 SuperMini
(la cible par défaut) :

| BM5602 | C6 SuperMini |
|---|---|
| `VDD` / `VSS` | 3V3 / GND |
| `SCK` | IO18 |
| `GIO2` | IO19 (MISO) |
| `SDIO` | IO20 (MOSI) |
| `CSN` | IO14 |

> `GIO2` sert de MISO : le firmware bascule le module en SPI 4 fils à l'init.
> Le module n'a aucun marquage : l'ordre de ses pastilles est dans
> [docs/WIRING.md](docs/WIRING.md#brochage-du-module-bm5602-60-1).

## Compilation

```bash
pio run -t upload -t monitor
```

La cible par défaut est `esp32c6supermini` (Matter sur Wi-Fi). Les autres se
sélectionnent avec `-e` : `esp32c6thread` (Matter sur Thread), `esp32c3`,
`esp32s3`, `esp32dev`.

> Un nœud appairé en Thread (Apple Home) se reflashe avec
> `pio run -e esp32c6thread -t upload -t monitor`. La commande sans `-e` y
> mettrait le build Wi-Fi : l'appairage reste en NVS, mais le nœud devient
> injoignable jusqu'au retour du build Thread.

Si la carte boucle au démarrage juste après le flash, c'est la mémoire flash du
clone qui n'aime pas le mode QIO : ajoute `board_build.flash_mode = dio` dans
l'environnement.

Deux contraintes de build, toutes deux déjà réglées dans `platformio.ini` :

- **La plateforme est le fork [pioarduino](https://github.com/pioarduino/platform-espressif32).**
  La bibliothèque Matter est livrée avec le core Arduino-ESP32 3.x (rien à
  installer via `lib_deps`), or la plateforme officielle de PlatformIO est
  restée au core 2.0.x qui n'a pas Matter du tout.
- **Partitions `huge_app.csv`** (3 Mo APP). La pile Matter pèse à elle seule
  ~1,4 Mo. Conséquence assumée : **pas d'OTA** sur une flash de 4 Mo.
  `min_spiffs.csv` laisserait la place à l'OTA mais ne tient que sur C3 et
  ESP32 classique, avec ~150 Ko de marge — ça déborde sur S3 et C6.

## Mise en service

### 1. Verifier le lien avec la lampe

Le firmware connait deja l'adresse de lien de la lampe : `63 FD F0 4F` sur
l'air, soit `4FF0FD63` dans l'ordre d'ecriture du BM5602, canal 5 (2405 MHz),
125 kbps. `lampe adresse` l'affiche. Rien a chercher.

Dans le moniteur serie, ecoute la telecommande pendant que tu la manipules
(30 s par defaut) :

```
ecoute 4FF0FD63 5
```

Chaque geste doit afficher des trames decodees au CRC juste (`C4 xx` a la
molette, par exemple). Puis `lampe` montre l'etat du pilote : consigne, etat
cru, champs a livrer, lien et radio. Une premiere commande, `lampe on` ou
`lampe lum A0`, doit finir sur une ligne `ok ... accuses`.

L'adresse de lien depend de la telecommande (elle vient de l'appairage BenQ).
Pour une autre paire lampe/telecommande, `lampe adresse XXXXXXXX` (ordre
d'ecriture) l'enregistre en NVS. Comment elle a ete trouvee :
[docs/PROTOCOL.md](docs/PROTOCOL.md).

### 2. Appairer le nœud Matter

Le code d'appairage est affiché au démarrage, et `matter` le rappelle :

```
code manuel : 34970112332
```

Un contrôleur Matter est nécessaire — HomePod, Apple TV, Google Nest,
Echo, ou le module *Matter Server* de Home Assistant.

Un appui long (5 s) sur le bouton **BOOT**, ou la commande `decommission`,
retire toutes les fabriques pour ré-appairer de zéro.

### 3. LED d'etat

Le voyant du produit est la LED RGB (WS2812) de la carte, sur IO8, dans les
builds `esp32c6thread` et `esp32c6supermini`. Intensite basse, puisqu'elle vit
sous le bureau : 24/255 au plus par canal, 8/255 pour la lueur blanche.

| LED | Signification |
|---|---|
| bleu clignotant (2 Hz) | pas encore mis en service : ajouter l'accessoire depuis l'app |
| orange lent (1 s allumee, 1 s eteinte) | mis en service, mais reseau absent (Thread perdu ; Wi-Fi pour `esp32c6supermini`) |
| eteinte, breve lueur blanche toutes les 10 s | tout va bien (signe de vie) ; une lueur aussi au retour du reseau |
| eclat vert (150 ms) | une consigne vient d'etre livree a la lampe (accusee) |
| rouge, 3 clignements | lampe injoignable (ou module radio perdu) : le pilote abandonne la consigne |
| arc-en-ciel | « Identifier » demande depuis Apple Home (cluster Identify), pendant toute l'identification |

Priorite : arc-en-ciel > rouge > vert > etat du reseau. Au banc, `led test`
joue chaque motif a tour de role (16 s) et `led` dit le motif en cours. Si le
vert et le rouge sont inverses, la WS2812 de la carte n'est pas en GRB :
`-DSTATUS_RGB_ORDER=LED_COLOR_ORDER_RGB` dans `platformio.ini`.

La petite LED d'IO15 reste en entree, donc eteinte quelle que soit sa
polarite : un seul voyant. En entree, elle ne gene pas non plus GDO2 du CC2500,
qui arrive sur IO15 quand la carte de capture est branchee. Le build diagnostic
ne fait que mettre la WS2812 au noir au demarrage : elle garde sa derniere
couleur a travers un reset ou un flash, et le bleu d'un build produit resterait
allume sur le banc. Parmi les autres cibles, seule `esp32dev` fait clignoter sa
LED simple (IO2) avec les memes motifs, sans la lueur ; sur les DevKit C3 et S3,
`PIN_STATUS_LED` (IO8, IO48) est la broche de leur WS2812, non declaree : pas de
voyant visible.

### Certification : ce qui marche et ce qui demande une étape en plus

Le firmware utilise les **certificats de test du SDK Matter** :
`VID 0xFFF1`, `PID 0x8000`. C'est le mode de développement normal, mais chaque
écosystème le traite différemment :

| Écosystème | Comportement |
|---|---|
| **Home Assistant** | accepte directement (Matter Server) |
| **Apple Home** | ajoute l'accessoire en affichant un avertissement « accessoire non certifié », qu'il suffit de confirmer |
| **Google Home** | refuse, sauf à créer un projet dans la [Google Home Developer Console](https://developers.home.google.com/matter/get-started) déclarant le même couple VID/PID de test |
| **Alexa** | non vérifié |

Faire disparaître complètement l'avertissement demanderait un VID attribué par
la CSA et une certification — hors de portée d'un projet perso.

## Commandes série

`help` liste tout.

| Commande | Effet |
|---|---|
| `info` | materiel, configuration radio, etat du pilote |
| `matter` | etat Matter, code d'appairage, identite du noeud et versions, compteurs du pont, demandes Identify |
| `matter impulsion [300..15000]` | duree de l'impulsion d'EP4 en ms, gardee en NVS (EP4 reactive seulement ; sinon un message le dit) |
| `matter reprise` | (Thread) relance tout de suite la reprise des abonnements sauves d'Apple Home |
| `matter reprise auto [0\|1]` | (Thread) relance seule apres un redemarrage : Thread + SRP prets depuis 10 s, pas avant 50 s (plus le plancher sauve), pour chaque abonne sauve sans abonnement actif ; session CASE d'abord (un echec ne coute rien a la pile), reprise ensuite ; puis 30 s, 60 s, 5 min apres chaque echec, et un coup d'oeil toutes les 5 min tant qu'un abonnement est actif (NVS) |
| `matter med [0\|1\|2]` | (Thread) type au prochain demarrage : 0 routeur, 1 MED des l'init (sans nouvelle attache), 2 MED apres `Matter.begin()` (ancien) (NVS) |
| `matter maxint [0\|10..3600]` | (Thread) plafond de l'intervalle max des abonnements neufs, 20 s par defaut, 0 = celui du controleur (NVS). Ne vaut qu'a partir du prochain abonnement neuf d'Apple : un abonnement repris garde son intervalle sauve |
| `lampe` | pilote Halo 1 : consigne, etat cru, champs a livrer, lien, radio |
| `lampe on` / `lampe off` | allumer / eteindre, memes regles que Matter |
| `lampe avant on\|off` / `lampe arriere on\|off` | une lampe (comme EP2 / EP3) |
| `lampe mode avant\|arriere\|deux` | lampes allumees (et allumage) |
| `lampe lum 4C..FE` / `lampe niveau 1..254` | luminosite brute (hexa) / niveau Matter |
| `lampe temp 0..100` / `lampe mired 153..370` | temperature : 0 froid, 100 chaud / en mireds |
| `lampe auto` | bouton A (refuse lampe eteinte) |
| `lampe sync` | renvoie tout ce qui est connu |
| `lampe trace 0\|1` / `lampe stats` | journal par evenement / compteurs |
| `lampe adresse [8 hexa]` | adresse de la lampe (ordre d'ecriture), en NVS |
| `lampe help` | toutes les commandes `lampe` (reglages et banc) |
| `led` | LED d'etat : motif en cours, couleur affichee |
| `led test` / `led stop` | joue chaque motif de la LED a tour de role (16 s), sans bloquer / l'arrete |
| `ecoute 4FF0FD63 5 [ms]` | ecoute passive de la telecommande, sans jamais accuser |
| `regs` | dump des registres du BC5602 |
| `rfinit` | re-teste le module apres correction du cablage, sans reflasher |
| `wifi <ssid> <mdp>` | identifiants Wi-Fi (ESP32 classique uniquement) |
| `decommission` | retire toutes les fabriques Matter |
| `reboot` | sauve l'etat de la lampe, puis redemarre |

Les commandes du Halo 2 (`poll`, `send`, `find`, `pair`, `sniff`, `tail`) sont
retirees : la lampe est un Halo 1, dont le protocole est different.

## Protocole du Halo 1

Le protocole est etabli, et verifie par emission sur la lampe : trame BC5602
standard (adresse de 4 octets, PCF de 9 bits, CRC-16 materiel), charge de deux
octets (drapeaux marche / lampes / selecteur, puis valeur), accuse vide. On ne
peut donc pas lire l'etat de la lampe : le pilote suit celui qu'il lui envoie
et ce qu'il entend de la telecommande. Detail, preuves et questions encore
ouvertes : [docs/PROTOCOL.md](docs/PROTOCOL.md) (bloc d'en-tete, puis les
sections Halo 1 a la fin).

## Structure

```
platformio.ini            4 cibles ESP32, plateforme pioarduino, partitions huge_app
src/config.h              broches, minuteries, limites de la lampe, identite Matter
src/fw_version.h          version du firmware (FW_VERSION + revision git)
src/app_desc.c            descripteur d'application : version rapportee par Matter
src/bc5602.{h,cpp}        pilote bas niveau du transceiver
src/halo.{h,cpp}          demarrage du module, outils de banc (couche Halo 2 neutralisee)
src/halo1_proto.{h,cpp}   protocole Halo 1 pur : trames, CRC, planification
src/halo1_map.{h,cpp}     correspondances Matter <-> lampe, regles d'intention
src/halo1_radio.{h,cpp}   sequences BC5602 prouvees, reconfiguration non bloquante
src/halo1_lamp.{h,cpp}    pilote : consigne, rafales accusees, suivi de la telecommande
src/cli_lampe.cpp         commandes 'lampe ...'
src/matter_bridge.{h,cpp} endpoints Matter, boite d'intentions, reflet de la consigne, Identify
src/status_led.{h,cpp}    LED d'etat : motifs et priorites (logique pure, testee sur l'hote)
src/net.{h,cpp}           Wi-Fi pour les cibles sans commissioning BLE
src/cli.{h,cpp}           console série de rétro-ingénierie
src/main.cpp              assemblage, bouton de decommissioning
docs/PROTOCOL.md          protocole radio, connu / à confirmer, méthodes de capture
docs/WIRING.md            câblage et pièges matériels
tools/test_halo1.sh       tests hote du protocole Halo 1 et de la LED d'etat, sans carte
tools/git_rev.py          revision git pour FW_GIT_REV (drapeau dynamique de PlatformIO)
```

## Crédits

Le protocole du Halo 2 a été rétro-conçu par
[kuzmin-no](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration)
(MicroPython, Raspberry Pi Pico W, sortie MQTT). Ce projet en est un portage
C++ pour ESP32 avec Matter natif, plus les outils nécessaires à la
transposition vers la 1re génération.

L'identification du BC5602 dans le Halo 1 revient à `hertzg` et le teardown du
PCB à `b4shful`, sur le
[fil Home Assistant](https://community.home-assistant.io/t/benq-screenbar-support/490864).
