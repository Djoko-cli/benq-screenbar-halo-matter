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
| EP4 "Halo auto" | On/Off Plug-in Unit | appui sur le bouton A (mode auto), revient seul a off apres 1 s |

- Une trame radio ne porte qu'une valeur : les deux lampes partagent la
  luminosite et la temperature, d'ou un seul curseur de chaque sur EP1.
- Allumer EP1 retrouve la derniere selection de lampes, comme le bouton marche
  de la telecommande. Eteindre EP2 puis EP3 eteint la lampe.
- EP4 est ignore quand la lampe est eteinte, et quand il arrive avec un ordre
  marche ou lampe (commande de piece, tuile regroupee) : dans Apple Home,
  afficher les accessoires en tuiles separees.
- Les noms se donnent dans l'app. Les Kelvin (~6500 a ~2700 K) sont nominaux,
  non mesures.
- Dans `src/config.h` : `HALO1_SELECTORS_AS_LIGHTS 0` expose EP2 et EP3 en
  prises (un "eteins les lumieres" de piece n'y touche plus),
  `HALO1_EXPOSE_AUTO 0` retire EP4.
- Rien n'est emis vers la lampe au demarrage : le noeud reprend l'etat sauve,
  et seul un ordre (Matter ou `lampe ...`) fait emettre.

> **Mise a jour depuis une version precedente** : la disposition des endpoints
> a change (avant : alimentation, lumiere avant, halo arriere, capteur, mode
> auto). Il faut remettre le noeud en service : retirer l'accessoire de chaque
> app, lancer `decommission` (ou appui long sur BOOT), puis l'ajouter a nouveau
> avec le code d'appairage (`matter`).

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

La cible par défaut est `esp32c6supermini`. Les autres se sélectionnent avec
`-e` : `esp32c3`, `esp32s3`, `esp32dev`.

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

### 1. Trouver l'adresse de communication

C'est **l'étape indispensable**, et elle est indépendante de Matter. Chaque
paire lampe/télécommande a une adresse radio propre, échangée sous forme
encodée pendant l'appairage BenQ. Sans elle, rien ne se passe.

À la télécommande, règle exactement :
- luminosité de la **lampe arrière** à **10 %**
- température de couleur à **3925 K**

Puis, dans le moniteur série :

```
find
```

Actionne un réglage toutes les 2–3 secondes pendant la minute de capture pour
que la télécommande émette. À la fin, le firmware liste les candidats classés
par nombre d'occurrences :

```
=== Recherche d'adresse : resultats ===
  17 occurrence(s)  adresse = 3A 91 04 C7   ->  addr 3A9104C7
   1 occurrence(s)  adresse = 08 00 12 FF   ->  addr 080012FF
```

Applique le plus fréquent, puis vérifie :

```
addr 3A9104C7
poll
```

`poll` doit afficher un état cohérent avec ce que tu vois sur la lampe.

Si la télécommande du Halo 1 ne permet pas de régler exactement 3925 K, utilise
d'autres valeurs : `find 25 4000` (luminosité arrière en %, température en K).
Le mot de synchro est recalculé tout seul.

Si `find` ne donne rien, voir [docs/PROTOCOL.md](docs/PROTOCOL.md#retrouver-ladresse-de-communication)
pour les deux autres méthodes (sniff du bus SPI de la télécommande, HackRF).

### 2. Appairer le nœud Matter

Le code d'appairage est affiché au démarrage, et `matter` le rappelle :

```
code manuel : 34970112332
```

Un contrôleur Matter est nécessaire — HomePod, Apple TV, Google Nest,
Echo, ou le module *Matter Server* de Home Assistant.

Un appui long (5 s) sur le bouton **BOOT**, ou la commande `decommission`,
retire toutes les fabriques pour ré-appairer de zéro.

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
| `info` | matériel, configuration radio, état de la lampe |
| `matter` | état Matter, code d'appairage |
| `poll` | interroge la lampe maintenant |
| `debug` | bascule les traces RF (trames émises / ACK / télécommande) |
| `regs` | dump des registres du BC5602 |
| `rfinit` | re-teste le module après correction du câblage, sans reflasher |
| `addr` / `addr 11223344` | affiche / définit l'adresse |
| `tail 0102` / `tail ffff` | octets de queue du payload ; `ffff` désactive le contrôle |
| `chan 5` | canal radio (5 = 2405 MHz, 46, 75) |
| `find` / `find 25 4000` / `find x550f0a` | recherche d'adresse |
| `pair` | écoute sur l'adresse d'appairage `E2 08 00 B0` |
| `sniff` / `normal` | mode sniffer / retour au mode normal |
| `send 0300320FA0320FA00102` | envoie un payload brut de 10 octets, affiche l'ACK |
| `erase` | efface la configuration radio |
| `wifi <ssid> <mdp>` | identifiants Wi-Fi (ESP32 classique uniquement) |
| `decommission` | retire toutes les fabriques Matter |
| `reboot` | redémarre |

## Ce qui reste à confirmer sur le Halo 1

La **couche radio** du Halo 1 est acquise : même BC5602, même bande
2405–2475 MHz (dossiers FCC `JVPCR20CCTR` / `JVPCR20C`, plus un teardown du PCB
sur le fil Home Assistant).

La **couche applicative** vient en revanche du Halo 2 et n'a jamais été
vérifiée sur la 1re génération :

- structure exacte du payload de 10 octets ;
- octets de queue (`01 02` sur le Halo 2 — `tail` permet de les changer,
  `tail ffff` désactive le contrôle) ;
- bit 5 du registre de contrôle, documenté « capteur ultrason » sur le Halo 2,
  alors que le Halo 1 n'a pas de détecteur de présence.

Si `poll` ne renvoie rien de cohérent une fois l'adresse trouvée, la marche à
suivre est dans cet ordre :

1. `tail ffff` pour lever le contrôle des octets de queue, puis `poll` à nouveau ;
2. `sniff` et manipule la télécommande : les trames brutes s'affichent, on peut
   comparer les octets qui bougent avec les réglages modifiés ;
3. `pair` pendant un appairage : les trames sont alors sur une adresse
   **connue**, donc capturables même sans avoir trouvé l'adresse de
   communication.

Le décodage se fait ensuite en ajustant `buildPayload()`, `validate()` et
`parseStatus()` dans [src/halo.cpp](src/halo.cpp). Rien de tout ça ne touche à
la couche Matter.

## Structure

```
platformio.ini            4 cibles ESP32, plateforme pioarduino, partitions huge_app
src/config.h              broches, minuteries, limites de la lampe
src/bc5602.{h,cpp}        pilote bas niveau du transceiver
src/halo.{h,cpp}          protocole BenQ + machine à états non bloquante
src/matter_bridge.{h,cpp} endpoints Matter, boite d'intentions, reflet de la consigne
src/net.{h,cpp}           Wi-Fi pour les cibles sans commissioning BLE
src/cli.{h,cpp}           console série de rétro-ingénierie
src/main.cpp              assemblage, LED d'état, bouton de decommissioning
docs/PROTOCOL.md          protocole radio, connu / à confirmer, méthodes de capture
docs/WIRING.md            câblage et pièges matériels
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
