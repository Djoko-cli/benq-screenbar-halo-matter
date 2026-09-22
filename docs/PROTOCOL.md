# Protocole radio BenQ ScreenBar Halo

## Statut des informations

Tout ce document vient de la rétro-ingénierie du **ScreenBar Halo 2** par
[kuzmin-no](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration),
recoupée avec les dossiers FCC et le fil
[Benq Screenbar support](https://community.home-assistant.io/t/benq-screenbar-support/490864)
de la communauté Home Assistant.

| Élément | Halo 2 | Halo 1 (ce projet) |
|---|---|---|
| Transceiver RF | BC5602 | **BC5602 — confirmé** (FCC + teardown PCB) |
| Bande | 2405–2475 MHz | **2405–2475 MHz — confirmé** (FCC `JVPCR20CCTR`) |
| Modulation / débit | GFSK 125 kbps | Très probable (même puce, même bande) |
| Format de trame | ESB : préambule + adresse 4 o + PCF 9 bits + payload + CRC | Très probable (imposé par le BC5602) |
| Structure du payload | 10 octets, documentée ci-dessous | **À confirmer** |
| Octets de queue | `01 02` | **À confirmer** (`tail` pour les changer) |
| Bit « capteur » | ultrason | **À confirmer** (le Halo 1 n'a pas de capteur de présence) |

Autrement dit : la couche radio est acquise, la couche applicative est à
vérifier. Les commandes CLI `sniff`, `pair` et `send` sont là pour ça.

Le PCB du Halo 1 (relevé par `b4shful` sur le fil HA) : PSoC Cypress
**CY8C4125LQI-483** + transceiver **BC5602** + expandeur I²C **TCA9539PWR**.

## Couche radio

- Canal 1 : **2405 MHz** (`RFCH = 5`) — le seul observé en pratique
- Canal 2 : 2446 MHz (`RFCH = 46`)
- Canal 3 : 2475 MHz (`RFCH = 75`)
- Modulation GFSK, **125 kbps** (`DM1 = 0b10`)
- Adresse de 4 octets, **écrite dans l'ordre inverse** de l'ordre sur l'air
  (section *Bit ordering* du datasheet BC5602)

Format de trame, hérité de l'Enhanced ShockBurst :

```
préambule 0xAA │ adresse 4 octets │ PCF 9 bits │ payload 10 octets │ CRC
```

Le PCF (Packet Control Field) contient : longueur sur 5 bits (décalée de 3),
PID sur 2 bits, drapeau NO_ACK sur 1 bit.

**La lampe n'émet jamais spontanément.** Elle ne répond que dans le slot ACK
matériel qui suit une trame reçue. D'où la stratégie du firmware : interrogation
toutes les 5 s, et écoute passive de la télécommande entre deux interrogations.

### Auto-ACK et écoute

Le BC5602 gère l'auto-ACK en matériel. Deux conséquences :

- pour **piloter** la lampe, on active l'auto-ACK : la réponse de la lampe
  arrive dans la FIFO RX juste après l'émission ;
- pour **écouter** la télécommande, il faut le **désactiver** — sinon notre
  module acquitterait les trames en même temps que la lampe, et la
  télécommande cesserait de fonctionner.

Désactiver l'auto-ACK désactive aussi le CRC matériel et la longueur de payload
dynamique. Le PCF de 9 bits n'est alors plus retiré du flux, ce qui décale tous
les octets d'un bit : le firmware recale à la lecture
(`BC5602::shiftLeftOneBit`).

## Payload (10 octets)

| Octet | Contenu |
|---|---|
| 0 | Commande |
| 1 | Registre de contrôle (bits, voir plus bas) |
| 2 | Luminosité lampe avant, `0x01`–`0x64` (1–100 %) |
| 3 | Température de couleur, poids fort |
| 4 | Température de couleur, poids faible |
| 5 | Luminosité lampe arrière, `0x01`–`0x64` |
| 6 | Température de couleur arrière, poids fort (identique à l'avant) |
| 7 | Température de couleur arrière, poids faible |
| 8 | Octet de queue 0 — `0x01` sur le Halo 2 observé |
| 9 | Octet de queue 1 — `0x02` sur le Halo 2 observé |

La température de couleur est transmise **en Kelvin, en clair** :
2700 K = `0x0A8C`, 4000 K = `0x0FA0`, 6500 K = `0x1964`.

La lampe arrière n'a pas de température propre : les deux partagent la valeur.

### Registre de contrôle (octet 1)

| Bit | Rôle |
|---|---|
| 0 | Marche / arrêt général |
| 1 | Mode Auto |
| 2 | Favori |
| 3 | ┐ `0` = avant seule, `1` = arrière seule, `2` = les deux |
| 4 | ┘ |
| 5 | Capteur (ultrason sur le Halo 2) |
| 6–7 | Non utilisés |

### Commandes (octet 0)

| Valeur | Signification |
|---|---|
| `0x00` | La télécommande se réveille et contacte la lampe |
| `0x02` | Allumage / extinction général |
| `0x03` | Réglage luminosité + température de couleur |
| `0x04` | Demande de synchronisation d'état |
| `0x05` | La télécommande s'endort et en informe la lampe |
| `0x0A` | Mode appairage |

La lampe applique les changements **en fondu progressif**. Une trame `0x03`
n'est donc pas immédiatement reflétée : le firmware réinterroge avec `0x04`
toutes les 400 ms jusqu'à convergence (12 essais max, ~5 s).

## Appairage

Pendant l'appairage, télécommande et lampe communiquent sur une adresse fixe :
**`E2 08 00 B0`** sur l'air (soit `B0 00 08 E2` en ordre d'écriture registre).
La commande est toujours `0x0A`.

L'adresse de communication définitive semble transmise pendant cet échange sous
forme encodée — elle n'a pas été décodée. C'est pour cela qu'il faut la
retrouver par capture (voir ci-dessous).

La commande CLI `pair` met le module en écoute sur cette adresse : c'est le
meilleur moyen d'obtenir des trames Halo 1 exploitables **sans connaître
l'adresse de communication**, et donc de vérifier en premier lieu si la
structure de payload ci-dessus tient.

## Retrouver l'adresse de communication

### Méthode 1 — l'astuce du mot de synchro (commande `find`)

C'est la méthode du script `find_halo2_address.py`, généralisée.

On règle le récepteur sur une pseudo-adresse de 3 octets correspondant à une
séquence du **payload** dont on connaît la valeur, parce qu'on vient de la
régler à la télécommande. Les octets 5-6-7 conviennent : luminosité arrière +
température de couleur.

Exemple avec 10 % et 3925 K :

```
sur l'air        : 0A 0F 55
ordre d'écriture : 55 0F 0A
```

Le récepteur se verrouille donc **au milieu** d'une trame, puis continue
d'échantillonner. Les retransmissions automatiques font apparaître le début de
la trame suivante dans la même fenêtre de capture : préambule `0xAA` suivi de la
vraie adresse.

Le script d'origine lisait l'adresse à un offset fixe, calé sur le timing
inter-trames du Halo 2. Ici le firmware **balaie les 8 alignements de bits et
toute la fenêtre capturée**, puis compte les occurrences de chaque candidat : le
bon ressort par répétition, le bruit non. C'est ce qui rend la méthode
transposable au Halo 1, dont le timing n'a aucune raison d'être identique.

```
find            # 10 % arrière + 3925 K (valeurs par défaut)
find 25 4000    # autres valeurs : luminosité %, Kelvin
find x550f0a    # mot de synchro brut de 3 octets
```

### Méthode 2 — sniffer le bus SPI de la télécommande

La méthode qui ne peut pas échouer, suggérée par `b4shful` sur le fil HA.
Ouvre la télécommande, branche un analyseur logique sur `CSN` / `SCK` / `SDIO`
du BC5602 et capture. La commande `0x10` (`WRITE_PTX_ADDRESS`) est suivie des
4 octets d'adresse, en clair.

Bonus non négligeable : la même capture donne aussi les payloads réels du
Halo 1, donc la structure exacte de la trame — ce qui répond d'un coup à toutes
les cases « à confirmer » du tableau en haut de page.

### Méthode 3 — HackRF One + Universal Radio Hacker

Capture à 2405 MHz, démodulation GFSK à 125 kbps, décodage manuel de la trame.
C'est la méthode qui a servi à établir le tableau du payload sur le Halo 2.
Plus lourde à mettre en œuvre, mais elle ne demande pas d'ouvrir le matériel.

## Références

- [BC5602 datasheet v1.20](https://www.holtek.com/webapi/116711/BC5602v120.pdf)
- [Module BM5602-60-1](https://www.holtek.com/page/vg/BM5602-60-1)
- [kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration)
- [Fil Home Assistant « Benq Screenbar support »](https://community.home-assistant.io/t/benq-screenbar-support/490864)
- FCC : [`JVPCR20CCTR`](https://fccid.io/JVPCR20CCTR) (télécommande Halo 1),
  [`JVPCR20C`](https://fccid.io/JVPCR20C) (lampe Halo 1)

---

# Ce que la campagne de mesure du 21/09/2026 a établi

## Trame réelle (Halo 2, publiée par Termina1)

```
54  04 10 0C 0F 55 5B 0F 55 01 02  20 B9
↑   └──────── payload 10 octets ────┘  └CRC┘
PCF
```

La structure de payload documentée plus haut est donc **confirmée sur une trame
réelle** : commande `04`, contrôle `10`, luminosité avant `0C`, température
`0F 55` (3925 K), luminosité arrière `5B`, température répétée, queue `01 02`.

La trame fait **13 octets** en réception : `PCF(1) + payload(10) + CRC(2)`.
Le PCF occupe **un octet plein** — il n'y a **pas** de décalage d'un bit à la
lecture, contrairement à ce que supposait le portage initial.

## CRC — modèle vérifié

```
algorithme    : CRC-CCITT
polynôme      : 0x1021
état initial  : 0xEFDF avant les 4 octets d'adresse EN ORDRE SUR L'AIR
couverture    : adresse + PCF + payload (10 octets)
```

Validé sur trois vecteurs, état intermédiaire compris :

| PCF | Payload | CRC attendu |
|---|---|---|
| `54` | `04 10 0C 0F 55 5B 0F 55 01 02` | `20B9` |
| `50` | `02 11 0C 0F 55 5B 0F 55 01 02` | `E962` |
| `50` | `02 10 0C 0F 55 5B 0F 55 01 02` | `0241` |

Après les 4 octets d'adresse `86 BB EA 9C`, l'état vaut `0x5042`.

Implémenté dans `BenqHalo::frameCrc()` et `frameCrcFor()`.

## Contrainte sur l'adresse

Datasheet BC5602 v1.20 p.25, sous le diagramme de format de paquet :

> `Note: * MSB high 4-bit must be 0001xxxx or 1110xxxx`

Le premier octet de l'adresse **sur l'air** doit être de la forme `0x1X` ou
`0xEX`. L'adresse d'appairage `E2 08 00 B0` s'y conforme. À noter que l'adresse
`86 BB EA 9C` de Termina1 ne s'y conforme pas tout en fonctionnant : la portée
exacte de la règle reste incertaine.

## Configuration de réception correcte

Alignée sur l'implémentation ESPHome de Termina1, qui reçoit réellement :

```
DPL1 = 0x00, DPL2 = 0x00     payload statique
RXPW0 = 13                   PCF + payload + CRC
PKT1 = 0x00                  CRC matériel désactivé
ENAA = 0x00                  auto-ACK désactivé
IRQ1 = 0x40                  acquitter RX_DR — INDISPENSABLE
puis commande 0x8E           RX Mode Trigger
```

`RX_DR` se latche et **doit** être acquitté en y écrivant 1, sinon la puce
cesse de délivrer des trames.

## Variables éliminées par la mesure

Débit (les 3 valeurs existantes), canal (les 3 du dossier FCC), ordre des octets
d'adresse (les deux), longueur de préambule (1 et 2 octets), longueur d'adresse
(3 et 4 octets), et l'existence d'une sortie de bits démodulés sur `GIO2`
(8 sélecteurs balayés, aucune activité).

## Ce qui bloque

L'**adresse de communication** de la paire lampe/télécommande reste inconnue, et
le corrélateur du BC5602 ne peut rien capter sans elle. La méthode consistant à
se caler sur une séquence du payload comme pseudo-adresse n'a jamais accroché,
malgré un récepteur dont le fonctionnement est mesuré (mode RX confirmé par
`OMST`, RSSI avec 17 dB de dynamique, environnement RF propre).

Les deux seules voies restantes demandent du matériel :

1. **Analyseur logique** sur `CSN`/`SCK`/`SDIO` du BC5602 de la télécommande :
   la commande `0x10` y transporte l'adresse en clair, et la même capture donne
   la structure réelle du payload du Halo 1.
2. **Second MCU** pour monter le récepteur indépendant que Termina1 mentionne
   dans ses notes d'implémentation.

## Comportement de la télécommande (mesure)

Les commandes sont **tactiles** : un toucher émet **une impulsion**, maintenir le
doigt n'émet rien de plus. Seule la **molette** produit un flux continu tant qu'on
la tourne — c'est donc la seule source de trafic exploitable pour une capture à
fenêtre fixe.

## Mode direct du BC5602 — sélecteurs non documentés

Le bit `DIR_EN` (`CFG1` 0x00, bit 4) commute la puce en mode direct :
*« TX/RX data from/to external MCU directly »*. Le datasheet le mentionne **une
seule fois** et ne dit ni quelle broche porte les données, ni comment engager le
mode. Le guide d'application Holtek ne le mentionne pas du tout.

Le portage ESPHome de Termina1, qui fonctionne, révèle deux valeurs de sélecteur
que le datasheet range pourtant dans « Others: No function, input » :

| Registre | Valeur | Fonction réelle |
|---|---|---|
| `IO1` (0x06), `GIO2S` | `3` | `DIRECT_TXD` — donnée, MCU vers puce |
| `IO2` (0x07), `GIO3S` | `8` | `TBCLK_OUTPUT` — horloge bit, puce vers MCU |

Sa séquence d'armement : `IO2=0x08`, `IO1=0x58`, `CFG1=0x50` (**`AGC_EN` +
`DIR_EN`**), puis `OM=0x03`, 50 µs, `OM=0x07`. Les bits 2~0 d'`OM` sont déclarés
« Reserved, must be kept unchanged after power on » : ce sont en fait des bits de
commande cachés.

`GIO3` est la **broche 8** du module BM5602 — le « septième fil » de son montage.

Deux conséquences pour ce projet :

1. Notre configuration tournait avec **`AGC_EN` à 0** (`CFG1` relu à `0x00`).
2. Il n'a **jamais tenté la réception en mode direct** : son chemin RX remet
   `GIO2S=1` et repasse par le moteur de paquets. Le sélecteur `DIRECT_RXD`, s'il
   existe, est à chercher parmi les valeurs `GIO2S` restantes (2, 4, 6, 7).

Entrée en réception sans commande strobe, documentée celle-là (`ds.txt:717`) :

> If the device is set as a PRX device, it will enter the RX mode when the CE bit
> is set high by using register or using Strobe RX command.

## Paramètres radio confirmés

Relevés dans le portage ESPHome qui pilote réellement une lampe :

```
RADIO_CHANNEL = 5        ->  2405 MHz
write_reg(0x11, 0x82)    ->  DM1 : AW=10 (4 octets) + 010 (125 kbps)
```

Débit **125 kbps**, adresse de **4 octets**, canal **5**. Ce sont déjà les valeurs
par défaut de `sharedRadioConfig()`.

Défaut corrigé le 2026-09-21 : `AGC_EN` (`CFG1` bit 6) n'était jamais activé. Le
portage tiers écrit `CFG1 = 0x50` (`AGC_EN` + `DIR_EN`). Mesuré sur notre carte,
molette en rotation : plus fort signal reçu **85 dB sans AGC, 41 dB avec**. Comme
tout reset logiciel remet `CFG1` à `0x00`, le bit est réappliqué dans
`sharedRadioConfig()`, au même titre que le bit de préambule.

## L'émission exige `CE`

Mesuré le 2026-09-22 sur un banc à deux cartes : 2553 trames écrites dans la FIFO
d'émission, **zéro** `TX_DS`, `OMST` bloqué à `2` (Light Sleep), mode TX jamais
observé. La commande strobe `0x0E` ne suffit pas.

Le datasheet l'explique (`ds.txt:711`) :

> If the device is set as a PTX device and the CE bit is set high, it will stay in
> the Light Sleep mode when the TX FIFO is empty. **The PTX device will enter the
> TX mode automatically once the TX FIFO is not empty.**

L'émission n'est donc pas déclenchée par une commande mais par le **remplissage de
la FIFO**, à condition que `CE` (registre `0x15`, bit 0) soit à `1`. Sans lui, la
puce attend indéfiniment, FIFO pleine.

`CE` est désormais posé dans `configForLoopback()` et dans `prepareToTransfer()`.
Rappel : `CE` est **effacé par le matériel** à chaque fin de réception, donc il
doit être reposé à chaque tentative d'entrée en RX.

## `EN_DYN_ACK` : l'écriture FIFO refusée en silence

Le datasheet (`ds.txt:869`), dans la description de `DPL2` (registre `0x2B`) :

> Bit 0 **`EN_DYN_ACK`**: PTX "write TX FIFO with No-Auto-ACK" command enable

Tant que ce bit vaut `0`, la commande d'écriture FIFO **sans** auto-ACK est
**ignorée sans aucun signal d'erreur** : la FIFO reste vide, la puce n'a rien à
émettre et demeure en Light Sleep.

Mesuré par la commande `autotest`, quatre combinaisons sur silicium :

| `EN_DYN_ACK` | commande d'écriture | FIFO remplie ? |
|---|---|---|
| 0 | sans auto-ACK | **non** |
| 1 | sans auto-ACK | oui, et `TX_DS` tombe |
| 0 | avec auto-ACK | oui |
| 1 | avec auto-ACK | oui |

C'est ce défaut qui expliquait 2553 trames « émises » sans une seule transmission
réelle. La séquence d'émission correcte est donc : `DPL2` bit 0 à `1`, écriture de
la FIFO, `CE` à `1` — après quoi la puce part en TX **d'elle-même**, sans commande
strobe.

## L'accrochage sur le préambule est impossible — démontré

L'idée : puisque le préambule est connu (`AA` répété), donner au corrélateur une
adresse de 3 octets valant `AA AA X` pour qu'il se cale sur le préambule plus le
premier octet d'adresse, et livre les trois octets suivants — c'est-à-dire le
reste de l'adresse.

**Testée sur un émetteur dont l'adresse était connue d'avance**, le 2026-09-22 :

| | |
|---|---|
| balise | 4474 trames, **100 % confirmées par `TX_DS`** |
| préambule émis | 2 octets, vérifié effectif (`CFO1 = 0x4F`) |
| adresse | `E1 22 33 44` sur l'air, canal 5, 125 kbps |
| récepteur | validé la même heure : 421 trames reçues sur 421 |
| **résultat** | **0 accroche sur 512 configurations** |

Le corrélateur **ne peut pas se caler sur un motif contenant le préambule**,
vraisemblablement parce qu'il ne s'arme qu'après avoir détecté un préambule
valide. La méthode est close : ne pas la réessayer.

Corollaire méthodologique : ce banc à deux cartes permet de **valider une
technique de découverte sur une adresse connue** avant de la lancer contre la
lampe. Toute méthode future doit passer par là d'abord.

## Le débit de la télécommande n'est pas 125 kbps

Mesure des durées de rafale du 2026-09-22, canal 5, seuil 70 dB.

**Étalonnage** sur la balise, dont on connaît le débit (125 kbps) et la trame
(19 octets = 152 bits = 1216 µs théoriques) :

```
1620 rafales — dominante 800-1499 µs, moyenne 913 µs, plus longue 1755 µs
```

L'instrument lit court d'environ un quart (913 pour 1216) : inertie du registre
RSSI et seuil qui rogne les bords.

**Télécommande de la lampe**, molette en rotation :

```
817 rafales — dominante 400-799 µs, moyenne 531 µs, plus longue 785 µs
bande 800-1499 : ZERO
```

Aucune rafale dans la bande où la balise en plaçait 929, et une rafale maximale
de 785 µs contre 1755 µs. Corrigé du biais, les 531 µs mesurés valent environ
**708 µs réels** — soit 608 µs pour 19 octets à 250 kbps, contre 1216 µs à
125 kbps.

**125 kbps sur une trame de 19 octets est exclu par la mesure.** La durée seule ne
sépare pas formellement « 19 octets à 250 kbps » de « 11 octets à 125 kbps »,
la longueur du payload du Halo 1 étant inconnue — mais toutes les chasses menées
jusqu'ici écoutaient à 125 kbps, ce qui suffit à expliquer leurs zéros.

Le débit est désormais réglable et **persisté** (`debit 125|250|500`), au lieu
d'être codé en dur dans `sharedRadioConfig()`.

## La piste de l'appairage est close

Capture sur l'adresse d'appairage `E2 08 00 B0`, avec pour la première fois un
récepteur **validé** (421 trames sur 421 au même moment), au **débit mesuré**
(250 kbps) et sur le **canal mesuré** (5) : **zéro trame**, y compris en campant
sur le seul canal 5 avec trois fois plus de temps par combinaison.

L'explication n'est pas instrumentale. Observé le 2026-09-22 : la manip
d'appairage, qui se concluait la veille par un retour de la télécommande en deux
secondes, **expire désormais systématiquement au bout de dix**, et ce **les deux
ESP32 débranchés**. La télécommande continue par ailleurs de piloter la lampe.

Autrement dit : la paire est déjà liée, la manip n'a rien à renégocier, et **il
n'y a aucun échange d'appairage à capturer**. Ne pas relancer cette piste.

## Ce qui reste

L'adresse doit être lue là où elle est écrite en clair : sur le **bus SPI**
interne de la télécommande ou de la lampe, au démarrage, quand le
microcontrôleur la charge dans son BC5602 (commande « write PTX address »).

La lampe est sans doute la cible la plus simple : plus volumineuse, alimentée en
USB donc facile à redémarrer à volonté, et elle porte la même adresse que la
télécommande. Un ESP32 suffit à capturer ce bus.

## Écoute du bus SPI de la télécommande — point d'étape

La télécommande a été ouverte. Sa carte porte un **`BC5602` nu** (repère `U4`,
QFN-16), le quartz `Y1` à sa gauche et l'antenne sérigraphiée au-dessus — deux
repères physiques qui confirment l'orientation du boîtier.

Numérotation déduite des repères de coin sérigraphiés (`4/5`, `8/9`, `12/13`,
`16/1`), antenne en haut et quartz à gauche : bord haut `1-4` de droite à gauche,
bord gauche `5-8` de haut en bas, bord bas `9-12` de gauche à droite, bord droit
`13-16` de bas en haut.

**Les trois signaux sortent sur des pastilles de test**, vérifié au multimètre —
inutile de souder sur le QFN :

| Broche | Signal | Pastille |
|---:|---|---|
| 11 | `CSN` | la plus basse de la colonne de droite |
| 12 | `SCK` | celle du milieu, près de `C34` |
| 14 | `SDIO` | la plus haute |

Une masse est disponible sur une pastille au-dessus à gauche de la puce.

Niveaux au repos relevés par la commande `taptest`, pile en place : `CSN` tenue
**haute**, `SCK` tenue **basse**, `SDIO` haute. Ce sont les états d'un bus SPI
sain en mode 0, et ils ne peuvent pas provenir de lignes flottantes : les
pastilles sont donc les bonnes.

**Ce qui bloque est purement mécanique.** Un contact maintenu au ruban et à la
main ne survit pas à une capture : le journal se remplit de `FF`, `00`, `80`,
`C0`, `E0`, `F8`, `FC` — des suites de uns puis de zéros, signature d'un registre
à décalage cadencé par une ligne qui bascule au hasard. La capture filtre
désormais ce bruit, et n'annonce une écriture d'adresse que si `0x10` est suivi
d'au moins quatre octets non tous nuls.

Il faut des **pointes de test à ressort** pour maintenir les quatre contacts
pendant qu'on retire et remet la pile.

### Outillage de capture — cinq défauts corrigés

La première version de `sniffspi` ne pouvait pas fonctionner. Cinq défauts, tous
côté logiciel, trouvés grâce aux observations de terrain :

1. **Sourd entre deux transactions.** Une seule transaction armée, et un
   `Serial.flush()` de plusieurs millisecondes entre chacune. On attrapait la
   première d'une rafale et on dormait pendant tout le reste. Symptôme qui a mis
   sur la piste : « à l'insertion de la pile, le compteur monte de 1 » — alors
   qu'une initialisation compte des dizaines d'échanges.
2. **Bruit stocké au lieu d'être jeté.** Le tri ne se faisait qu'à l'affichage :
   le tampon se remplissait de parasites dans les premières secondes, et la
   rafale utile était perdue faute de place.
3. **Libération du périphérique avec des transactions encore armées**, d'où un
   `Load access fault` qui emportait la capture. Il faut vider la file d'abord —
   et afficher **avant** de démonter.
4. **Affichage tronqué à 20 octets** alors que les transactions vont jusqu'à 32 :
   le contenu discriminant était invisible.
5. **Test de contact trompeur.** Sans pile, toutes les lignes sont tirées vers la
   masse et ne suivent plus les résistances internes : elles passaient pour
   « pilotées ». Le discriminant est `CSN`, que le microcontrôleur maintient
   **haute** au repos.

À noter pour la suite : le pilote SPI esclave active des résistances de tirage
internes, donc **une broche débranchée se lit à 100 % haut**. Trois lignes à
100 % haut ne veulent pas dire « tout va bien » mais « rien ne touche ».

État final de l'outillage : capture à six transactions pré-armées, tri du bruit à
la volée, état des trois lignes et compteurs affichés une fois par seconde,
restitution complète sur 32 octets. **Il ne manque qu'un contact mécanique
fiable** — trois pastilles d'un millimètre ne se tiennent pas à la main.

## `J5` : le connecteur de programmation du microcontrôleur

Six trous traversants plaqués, repère `J5`, entre le contrôleur tactile `U2` et
la découpe. Ils acceptent une broche Dupont par simple friction — c'est le seul
point de la carte où le contact mécanique ne pose aucun problème.

Relevés au multimètre, télécommande alimentée, masse sur le trou 1 :

| Trou | Mesure | Interprétation |
|---:|---|---|
| 1 | continuité avec la masse | `GND` |
| 2, 4, 5, 6 | 2,49 V | tirés haut |
| **3** | **0,001 V** | **`SWCLK`** — tiré bas par sa résistance interne |

Aucun des cinq ne porte `CSN`, `SCK` ni `SDIO` : ce n'est pas un connecteur de
test radio. La signature — un seul trou bas pendant que les autres sont hauts —
est celle d'un port `SWD` : `SWDIO` et le reset se tiennent hauts, `SWCLK` bas.

**Conséquence sur les niveaux logiques** : la logique de la télécommande tourne
à **2,5 V**, pas 3,3 V, ce qui est cohérent avec des piles montées en parallèle
et remontées par un convertisseur. Le seuil de niveau haut de l'ESP32-C6 est
d'environ 2,48 V : les signaux arrivent donc dix millivolts au-dessus du seuil.
Ça fonctionne, mais sans marge — à garder en tête devant toute capture bruitée.

Une sonde `ST-Link V2` travaillant à 3,3 V devra passer par deux résistances
série de quelques centaines d'ohms sur `SWDIO` et `SWCLK`, pour ne pas faire
conduire les diodes de protection de la cible.

### Implémentation `SWD` en bit-banging

`src/swd.cpp` génère le protocole sans matériel dédié : reset de ligne, bascule
JTAG vers SWD, puis échanges de 8 bits de requête, 3 bits d'acquittement et
32 bits de donnée avec leurs cycles de retournement.

La commande `swd` **cherche elle-même quel trou est `SWDIO`** parmi plusieurs
broches de l'ESP32 reliées d'un coup aux trous inconnus — pas de recâblage entre
les essais. Elle ne s'arrête pas à l'`IDCODE` : elle demande ensuite la mise sous
tension du domaine de debug et vérifie l'acquittement matériel, ce qui distingue
une vraie liaison d'une lecture heureuse.

Réserve connue : Artery livre souvent ses microcontrôleurs avec la lecture de la
flash verrouillée. La liaison peut donc s'établir sans que le contenu soit
accessible.

## L'accrochage en milieu de trame EST possible — à condition d'une ancre

**Correction d'une conclusion erronée.** Une version précédente de cette section
affirmait l'inverse, « démontré » à l'appui. La démonstration ne valait rien.

Le détecteur de préambule s'arme sur une **suite alternée**. Une fenêtre de trois
octets prise dans le payload n'est donc accrochable que si l'octet qui la
**précède** ressemble à un préambule, c'est-à-dire vaut `0x55` ou `0xAA`.

Le script `find_addr.py` de kuzmin, dont l'intégration Home Assistant fonctionne,
le dit explicitement :

```python
HALO2_ADDRESS = [0x55, 0x0f, 0x0a]
# 0x55, 0x0f -> Color temperature 3925K
#               (reverse byte order; used as preambule and part of the sync word)
```

D'où sa consigne de régler la lampe sur **3925 K** exactement : c'est la valeur
dont l'octet de poids faible vaut `0x55`, et cet octet **sert de préambule**.

Le premier test de contrôle utilisait le payload de balise
`DE AD BE EF 01 02 03 04 05 06`, dont la plus longue suite alternée fait sept
bits — trop court pour armer le détecteur. **Aucune de ses huit fenêtres n'était
ancrée : l'expérience ne pouvait pas accrocher, quelle que soit la capacité réelle
de la puce.**

Refaite avec un payload portant une ancre, `DE AD 55 0F A0 3C 01 02 03 04` :

| Fenêtre | Octets | Ancrée | Trames |
|---|---|---|---:|
| 0-2 | `DE AD 55` | non | 0 |
| 1-3 | `AD 55 0F` | non | 0 |
| 2-4 | `55 0F A0` | non | 0 |
| **3-5** | **`0F A0 3C`** | **oui** | **158** |
| 4-6 | `A0 3C 01` | non | 0 |
| 5-7 | `3C 01 02` | non | 0 |
| 6-8 | `01 02 03` | non | 0 |
| 7-9 | `02 03 04` | non | 0 |

Un positif, sept négatifs — y compris pour les fenêtres qui **contiennent** le
`0x55` sans être précédées par lui. Et la FIFO rend `01 02 03 04 …`, exactement
les octets suivant la fenêtre accrochée.

**Le procédé de `find` est donc valide.** Ses échecs s'expliquent par deux causes
identifiées depuis : le **débit** — toutes les chasses tournaient à 125 kbps alors
que la mesure de durée de rafale exclut ce débit pour le Halo 1 — et l'**ancre**,
qui impose de régler la lampe sur une valeur dont un octet vaut `0x55` ou `0xAA`.

Valeurs d'ancrage utilisables : une température dont l'octet bas vaut `0x55`
(2645, 2901, 3157, 3413, 3669, **3925**, 4181, 4437 … K) ou `0xAA` (2730, 2986,
3242, 3498, 3754, 4010 … K) ; ou une **luminosité de 85 %**, qui vaut `0x55`.

À noter : l'accrochage sur le **préambule lui-même** reste impossible, et pour une
raison qui découle du même mécanisme — rien ne précède le préambule, il ne peut
donc pas être ancré.

## La chasse ancrée appliquée au Halo 1 — sans résultat

Le mécanisme d'ancrage est **validé sur le banc** (voir ci-dessus : un positif,
sept négatifs). Appliqué à la télécommande du Halo 1, il ne donne rien.

Mesures du 2026-09-22, télécommande remontée et molette tournée sans arrêt,
témoin de trafic positif à chaque fois (12 à 13 pour mille de signal fort,
pics à 19-20 dB) :

| Configuration | Candidats | Accroches |
|---|---:|---:|
| 32 températures d'ancrage × 101 luminosités | 4995 | 1 |
| lampe arrière éteinte, 32 températures | 3000 (94 passes) | 1 |
| température imposée à 3925 K, 101 luminosités | 5000 (50 passes) | 2 |

Un motif de 24 bits se retrouve par hasard environ une fois sur 16 millions de
positions, et il en défile des dizaines de millions par minute : **une à deux
accroches par run est le bruit attendu**, pas un indice. Leurs contenus sont
d'ailleurs illisibles et leurs luminosités dispersées.

Trois accroches antérieures, toutes annoncées à 3925 K, avaient semblé
corréler avec l'affichage de la télécommande. Le test concentré ci-dessus —
température imposée, 50 passages complets sur la luminosité — aurait produit
des dizaines d'accroches si cette corrélation avait été réelle. Elle ne l'était
pas.

**Ce que cela laisse ouvert** : la structure du payload du Halo 1 n'a jamais été
vérifiée, elle est supposée identique à celle du Halo 2. Si elle diffère, toute
l'approche par fenêtre ancrée s'effondre — et rien dans nos mesures ne permet de
trancher.

## Détecteurs sans adresse — ce qu'ils disent

Deux instruments ont été construits qui ne demandent aucune adresse, en
exploitant le fait que `GIO3` ne s'anime que si un **préambule a été détecté** :

- `debitgio` — balaie les trois débits. **Validé sur la balise** : 434 et 442
  transitions à son débit réel, zéro aux deux autres. Discrimination parfaite.
- `canalgio` — balaie les 84 canaux. **Validé sur la balise** : seul son canal
  ressort, accompagné de son **image** seize canaux plus haut (fréquence
  intermédiaire de 8 MHz), que le RSSI permet de distinguer du vrai canal.

Appliqués à la télécommande, avec trafic attesté : **aucune transition, à aucun
débit, sur aucun canal**, préambule de un comme de deux octets.

La puce n'accroche donc jamais le préambule de la télécommande, alors que sa
carte porte elle aussi un `BC5602`. Ce constat est solide et reste inexpliqué.

## Le débit, tranché par le rapport FCC

Le rapport de laboratoire de la télécommande (`scratchpad/ctr_test_report.txt`)
donne, pour les trois canaux 2405 / 2446 / 2475 MHz :

| Fréquence | Largeur à 20 dB | Largeur occupée à 99 % |
|---|---|---|
| 2405 MHz | 0,504 MHz | **0,430 MHz** |
| 2446 MHz | 0,508 MHz | **0,434 MHz** |
| 2475 MHz | 0,508 MHz | **0,447 MHz** |

Le datasheet donne les excursions appliquées par la puce (`ds.txt:198-200`) :
160 kHz à 125 et 250 kbps, 250 kHz à 500 kbps. Pour du GFSK, la largeur occupée
vaut approximativement `2 × fDEV + débit` :

| Débit | Largeur attendue | Verdict |
|---|---|---|
| **125 kbps** | **445 kHz** | **compatible** |
| 250 kbps | 570 kHz | exclu |
| 500 kbps | 1000 kHz | exclu |

**La télécommande émet à 125 kbps.** Cela contredit l'estimation par durée de
rafale, qui annonçait 250 kbps en supposant une trame de 19 octets.

### Conséquence : la trame du Halo 1 est courte

À 125 kbps, la durée de rafale corrigée (~708 µs) correspond à environ
**11 octets**, non 19. Préambule, adresse, PCF et CRC en consomment 8 ou 9 : il
ne reste que **2 à 4 octets de payload**, là où le Halo 2 en a dix.

**La structure de trame du Halo 1 n'est donc pas celle du Halo 2** — hypothèse
qui soutenait toutes les chasses par fenêtre de payload, et qui explique leur
échec.

## La contradiction ouverte

Deux blocs de faits qui ne peuvent pas être vrais ensemble si les deux puces
sont réglées de la même manière :

- la télécommande **émet** (la lampe lui répond) et porte un `BC5602` ;
- notre `BC5602` **ne détecte jamais son préambule**, après avoir balayé
  84 canaux × 3 débits × 2 longueurs de préambule × 3 largeurs d'adresse ×
  2 séquences d'initialisation, instrument validé sur la balise à chaque fois.

**Réserve** : ces balayages n'accordent que 0,7 à 3 s par canal, et le témoin
RSSI ne voyait jamais le canal 5 ressortir pendant ces runs — alors que la
mesure `presence` l'y voyait bondir d'un facteur sept le matin même. Une source
à ~1 % de rapport cyclique peut être ratée par un balayage.

**À faire en priorité à la reprise** : camper une minute entière sur le canal 5
plutôt que balayer.

```
debit 125
canalgio 60000 5 5
```

Cela tranche entre « la puce ne sait pas démoduler cette source » et « on n'y
était jamais au bon moment ». Les quarante balayages précédents ne pouvaient pas
séparer ces deux lectures.

Variables encore non testées si ce campement ne donne rien : l'**excursion de
fréquence** et les réglages de **modem** non documentés. Seul le firmware de la
télécommande, lisible par `SWD` sur `J5`, peut les livrer.
