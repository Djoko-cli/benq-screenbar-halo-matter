# Protocole radio BenQ ScreenBar Halo

## ETAT AU 23/09/2026 -- A LIRE AVANT TOUT LE RESTE

Le reste de ce document est un journal chronologique : il contient des
conclusions depuis REFUTEES. En cas de desaccord, ce bloc fait foi. Detail et
preuves : [AUDIT-2026-09-23.md](AUDIT-2026-09-23.md), scripts dans
`tools/audit/` (modele unifie : `indep_pll/t3.py`, trames attendues :
`synthese/chk.py`).

**Format de trame Halo 1 -- format BC5602 standard (type ShockBurst).**
Valide sur environ 66 trames sur 68, par trois decodeurs independants et deux
chaines de reception (CC2500 asynchrone, FIFO du BM5602) :

```
preambule 01010101 | adresse 63 FD F0 4F | PCF 9 bits | charge | CRC-16
```

- **Adresse sur l'air : `63 FD F0 4F`**, a ecrire **`4F F0 FD 63`** dans le
  BM5602. Le motif `8F F7 C1 3C` utilise jusqu'au 22/09 n'est que cette
  adresse vue avec deux bits de decalage : bon pour CORRELER en reception, faux
  pour emettre. La regle du preambule le confirme sans passer par le CRC : une
  adresse qui commence par 0 appelle le preambule 01010101, celui qu'emet la
  telecommande.
- **PCF de 9 bits** : longueur de charge (6 bits), PID (2 bits), NO_ACK (1 bit).
- **CRC-16/CCITT 0x1021, etat initial 0xFFFF**, sur adresse + PCF + charge --
  exactement le CRC materiel du BC5602. Les etats initiaux 0xDFBE et 0xF55A
  trouves dans la nuit ne sont que 0xFFFF avance de un ou deux bits : des
  artefacts du decalage. Le 0xEFDF du projet Halo 2 est le meme artefact, avec
  un bit de decalage.
- **Commande (telecommande -> lampe)** : longueur 2, NO_ACK=0, charge de deux
  octets, le plus souvent `C4 xx` (vus aussi : C5, 44, 85, C3 en tete). Sens des
  octets : INCONNU.
- **Accuse (lampe -> telecommande)** : longueur 0, meme PID que la commande
  (9 paires sur 9), NO_ACK=1. **L'accuse du Halo 1 est vide** : contrairement au
  Halo 2, on ne peut pas lire l'etat de la lampe en l'interrogeant.
- Canal 5 (2405 MHz), 125 kbps.

**23/09 : LA LAMPE OBEIT.** Emission au format standard (`txack`), charge
dynamique, CRC materiel, accuse automatique, canal 5, telecommande SANS piles :

| adresse | charge | accuses | effet sur la lampe |
|---|---|---|---|
| `3C C1 F7 8F` (ancienne, decalee) -- temoin | `C4 FE` | **0 / 10** | aucun |
| `4F F0 FD 63` (vraie, sur l'air `63 FD F0 4F`) | `C4 FE` | **10 / 10**, 1,6 ms chacun | luminosite du minimum au quasi-maximum |

Adresse, format, CRC et chaine d'emission sont valides de bout en bout sur la
vraie lampe. Premiere hypothese sur la charge, a verifier : `C4` = reglage de
luminosite, octet suivant = `1` + niveau sur 7 bits (`FE` -> 126 sur 127).

**Semantique de la charge -- premiers essais (telecommande sans piles).**
| charge | etat de depart | effet observe |
|---|---|---|
| `C4 FE` x1 | minimum | quasi-maximum (une seule trame suffit : niveau ABSOLU) |
| `C4 9C` x3 | maximum | legere baisse percue |
| `C4 80` x1 | maximum | « a peu pres la moitie ou plus bas » |

Hypothese la plus compatible : `C4` = luminosite, 2e octet = niveau sur 8 bits.
L'hypothese d'un niveau sur 7 bits est ecartee (`C4 80` aurait donne le
minimum). Observations a l'oeil, donc a confirmer par l'ecoute de la
telecommande (`ecoute 4FF0FD63 5`).

**Outils valides le 23/09** : `txack` (emission standard avec accuse, verdict
TX_DS/MAX_RT, reconfiguration apres chaque echec), `ecoute` (reception
passive qui n'accuse jamais, PCF et CRC decodes en logiciel ; valide sur banc,
canal 40 : C4 FE et C4 80 decodes neuf fois chacun), `prxack` (recepteur de
banc).

**Emission, avant le 23/09 : rien n'avait fonctionne, et on comprend pourquoi.** Les essais
`tx6` (un accuse mal forme) puis `txraw` (bonne trame bit pour bit, mais avec
l'adresse decalee, donc le mauvais preambule, et 1800 copies de meme PID que
la lampe ecarte comme doublons) ne pouvaient pas marcher. Prochain essai :
emission en mode materiel standard, charge dynamique et accuse automatique,
adresse `4F F0 FD 63` ; la puce indique alors d'elle-meme si la lampe a accuse
reception (TX_DS) ou non (MAX_RT). Plan detaille : section 4 de l'audit.

**Passages de ce document INFIRMES par l'audit** (a ne plus citer) :

| Passage | Ce qui est faux |
|---|---|
| « Structure de trame Halo 1, confirmee sans le CRC » (6 octets, 72 = 48+16+8) | La commande porte 2 octets, l'accuse aucun ; la deuxieme adresse est l'accuse de la lampe, pas une retransmission ; `7A FF` etait un faux positif. |
| « Deux familles de trames : commandes et accuses » (en-tete [longueur 4][compteur 2][type 2], etats 0xDFBE / 0xF55A, « 19 commandes exactes ») | En-tete a cheval sur le PCF et la charge ; un seul etat initial 0xFFFF ; environ 31 commandes sur 32 sont exactes avec le bon modele. |
| « Le verrou : obtenir une trame B exacte » | Faux : les trames etaient exactes, c'est notre decoupage qui etait decale. Le verrou est l'emission. |
| « Biais d'erreur : 100 % des 1 lus comme 0 » | Observe sur 7 cas, contre une reference elle-meme decalee. Non etabli. |
| Trim du quartz, valeurs analogiques, distance « elimines » | Chaque condition ne comptait que 7 a 9 trames, jugees avec un modele de CRC faux, et le bras « holtek 0 » tournait sans AGC (bogue B5). Seul un effet d'un facteur 3 ou plus est exclu. |
| Tout passage sur le PCF « d'un octet plein » | Le PCF fait 9 bits ; l'« octet plein » venait de l'adresse decalee. |
| GIO3 « en amont du correlateur », « voie RF close », « contradiction etablie » | Deja infirme plus bas dans ce document ; explique par l'adresse. |


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

### La contradiction, verrouillée par deux mesures contrôlées

Mesure `presence` du 2026-09-22 à 19h24, bande « très fort » (RSSI ≤ 49 dB),
trois cycles alternés repos / molette en main :

| Canal | Repos | Molette en main | |
|---|---:|---:|---|
| **5 — 2405 MHz** | **5** | **847** | **×169** |
| 46 — 2446 MHz | 0 | 0 | — |
| 75 — 2475 MHz | 9 | 14 | — |
| 80 — témoin BLE | 159 | 146 | plat |

Le témoin ne bouge pas, les deux autres canaux FCC non plus. **La télécommande
émet sur le canal 5, et le récepteur l'entend très fort.**

Deux minutes plus tôt, une minute entière campée sur ce même canal 5 à
125 kbps : **1,29 % de signal fort, zéro détection de préambule.** Environ onze
cents rafales sont passées devant le démodulateur sans qu'une seule soit
reconnue.

Les deux mesures ont leur propre contrôle et ont été faites dans la même séance.
**La contradiction est donc établie, pas supposée** : le signal est là, fort, sur
le bon canal, et un `BC5602` ne reconnaît pas le préambule d'un autre `BC5602`.

Tout ce qui pouvait être balayé l'a été : 84 canaux, 3 débits, 2 longueurs de
préambule, 3 largeurs d'adresse, 2 séquences d'initialisation. Les variables
restantes — **excursion de fréquence** et réglages de **modem** non documentés —
ne sont pas accessibles par balayage. Elles sont dans le firmware de la
télécommande, lisible par `SWD` sur `J5`.


## Le reset logiciel efface les reglages analogiques (2026-09-22)

Mesure `survie` : sur les 19 valeurs recommandees par Holtek, **15 sont
remises a leur valeur d'usine par un reset logiciel**. Or `resetRadio()`,
`configForLoopback()` et `sharedRadioConfig()` commencent tous par un reset.

Consequence : de l'ecriture de ces valeurs dans `begin()` jusqu'au 22 septembre,
**elles n'ont jamais ete actives pendant une ecoute**. Tous les balayages de
canaux, de debits, de largeurs d'adresse et de longueurs de preambule ont tourne
sur un modem aux valeurs d'usine. Exception : le balayage `modem`, qui ecrit son
registre apres la configuration.

Corrige : `registerConfigure()` est rejoue apres chaque reset, dans les deux
chemins de configuration, pilotable par `holtek 0|1`. Le temoin est imprime par
la mesure elle-meme : **18 sur 19** en place (le 19e est l'anomalie connue du
registre 0x2D de la banque 2, ecrit 0x18 et relu 0x58).

Premiere chasse avec les reglages actifs, canaux 3 a 7, 125 kbps, 30 s chacun :
canal 5 a 2433 signaux forts sur 145713 (16,7 pour mille contre 7 de moyenne sur
la bande, et 4,5 au repos) -- la telecommande est bien entendue -- mais **zero
transition sur GIO3**.

## Le fil GIO3 est valide electriquement (2026-09-22)

Commande `fil` : on force la pastille a sortir un niveau, selecteur par
selecteur, et on regarde si la broche resiste aux resistances internes de
l'ESP32. Les 16 selecteurs pilotent la broche a un niveau franc (0/20 ou 20/20
contre les DEUX tractions), et **le niveau change avec le selecteur**. Cela
valide la chaine entiere : ecriture SPI -> pastille -> fil -> lecture ESP32.

Ce test ne demande aucune source radio, contrairement a un comptage de fronts :
il distingue un fil debranche d'une absence de signal, ce qu'un zero de
transitions ne sait pas faire.

Reste a valider fonctionnellement : qu'un selecteur BOUGE quand une trame
arrive. Cela demande la balise sur la deuxieme carte, actuellement debranchee.
Les selecteurs 2, 4, 9 et 14 avaient ete vus actifs -- avec la balise allumee.

## Trois controles qui verrouillent la contradiction (2026-09-22 au soir)

**1. Le canal 5 porte bien la telecommande, pas le Wi-Fi.** Le canal 5 (2405 MHz)
tombe dans le Wi-Fi 1, large de 20 MHz (2401-2423). Un emetteur Wi-Fi depose
donc autant d'energie a 2420 qu'a 2405 ; la telecommande, large de 0,43 MHz
(dossier FCC), ne peut etre qu'a un des deux endroits. Commande `discrimine`,
canaux 5 / 20 / 78 echantillonnes en alternance, phases repos puis molette :

| canal | role | repos | molette | rapport |
|---|---|---|---|---|
| 5 = 2405 MHz | cible, dans le Wi-Fi 1 | 3,92 0/00 | 16,66 0/00 | **x4,25** |
| 20 = 2420 MHz | temoin Wi-Fi 1 | 0,96 0/00 | 0,97 0/00 | x1,01 |
| 78 = 2478 MHz | hors Wi-Fi | 1,68 0/00 | 1,84 0/00 | x1,09 |

Instrument valide d'abord contre la balise (source etroite connue sur le canal
5) : x21,68 sur le canal 5, x1,36 et x0,85 sur les deux autres.

**Premiere version de cette mesure : fausse.** Elle changeait de canal en
ecrivant seulement `RFCH` puis attendait 1,5 ms. Avec la balise sur le seul
canal 5, les trois canaux lisaient 992 pour mille -- y compris un canal a 73 MHz
de distance. La PLL ne retune pas en 1,5 ms : il faut une reconfiguration
complete a chaque visite.

**2. GIO3S=14 est en amont du correlateur.** Refait avec une adresse fausse d'un
octet, balise allumee : 624 fronts avec la bonne adresse, **624 fronts avec la
mauvaise**, pendant que les trames acceptees tombent de 210 a 13. La sortie
reflete donc la detection de preambule seule. Un zero de transitions signifie
« aucun preambule reconnu », et non « adresse inconnue ».

Rendement mesure : **3 fronts par trame reconnue** (624 fronts / 210 trames).
Une telecommande a ~20 trames/s devrait donc donner ~1200 fronts en 20 s.

**3. La forme du preambule est epuisee.** Le BC5602 deduit la polarite du
preambule du premier bit d'adresse emis, et l'adresse part a l'envers de l'ordre
du tableau : c'est le DERNIER octet du tableau qui sort en premier. Nos sondes
n'avaient donc jamais teste qu'une polarite. Commande `forme` : 2 polarites x 2
longueurs x 3 debits = 12 configurations, 20 s chacune, molette tournee,
reglages Holtek actifs. **Zero transition sur les douze**, temoin de trafic a
~20 pour mille contre 3,9 au repos.

**L'excursion de frequence n'est pas reglable** et elle est deja la bonne. Le
datasheet la fixe par le debit : fDEV=160 kHz a 125 et 250 kbps, 250 kHz a 500
kbps. Regle de Carson a 125 kbps : 2x160 + 125 = **445 kHz**, contre **430, 434
et 447 kHz** mesures dans le dossier FCC de la telecommande. C'est la meme
modulation au kilohertz pres. Les six bits bas de `CFO1`, malgre le nom du
registre, sont marques « reserved, must be kept unchanged ».

Etat de la contradiction : canal confirme, debit confirme par deux voies
independantes, excursion confirmee, preambule epuise, detecteur etalonne,
reglages analogiques actifs -- et toujours aucun preambule reconnu.

## La voie RF est close, et on sait pourquoi (2026-09-22, bilan)

Balayage complet des **84 canaux**, 125 kbps, reglages analogiques actifs,
detecteur etalonne, molette tournee sans arret : **zero transition**. Le canal le
plus bruyant est le 59 (2459 MHz), en plein Wi-Fi 11 -- pas la telecommande.

**Le mode direct en reception est mort, confirme avec une source forte.** Rejoue
avec la balise et les reglages actifs :

| configuration | OMST | RSSI |
|---|---|---|
| DIR_EN=0, entree par registre CE | 5 (RX) | 123 -> **31 dB** |
| DIR_EN=0, entree par strobe 0x8E | 5 (RX) | 118 -> **31 dB** |
| DIR_EN=1, entree par registre CE | 2 (Light Sleep) | 127 -> 120 dB |
| DIR_EN=1, OM 0x03 puis 0x07 | 4 (TX) | 127 -> 118 dB |

Le recepteur est parfaitement vivant en mode normal ; `DIR_EN=1` le rend sourd
par toutes les methodes d'entree. Le datasheet annonce pourtant « TX/RX data
from/to external MCU directly » (ligne 377) : l'implantation ne suit pas.

**Cette puce ne sait pas livrer de bits non decodes.** Les huit selecteurs GIO2
ne sortent rien, et le rendement de GIO3 le disait deja : **3 fronts par trame**
(624 fronts pour 210 trames), la ou un flux de bits a 125 kbps en donnerait un
demi-million. GIO3 est une impulsion d'evenement, pas un train de donnees.

**Consequence.** Il n'existe aucun moyen d'ecouter l'air sans connaitre
l'adresse a l'avance : le moteur de paquets est le seul chemin vers les donnees,
et il refuse d'ouvrir. La methode 1 (verrouillage en milieu de trame sur une
pseudo-adresse tiree du payload) ne peut pas davantage fonctionner, puisque le
detecteur de preambule ne s'arme jamais sur ce signal, meme en debut de trame.

Ce qui reste etabli et n'est plus a refaire :

- la telecommande emet une source **etroite sur 2405 MHz** (controle Wi-Fi) ;
- a **125 kbps, fDEV 160 kHz** (Carson vs dossier FCC, au kilohertz pres) ;
- notre recepteur **fonctionne** (31 dB sur la balise, 210 trames decodees) ;
- le detecteur est **en amont du correlateur** (624 fronts avec adresse fausse) ;
- canal, debit, excursion, polarite et longueur de preambule sont **epuises**.

**Suite : la lecture du firmware AT32F421 par SWD.** Elle donne l'adresse ET la
structure reelle des trames, c'est-a-dire tout ce qui manque. Brochage J5 : trou
1 = masse, trou 3 = 0,001 V (candidat SWCLK), les autres a 2,49 V. Resistances
serie de 220 a 470 ohms sur SWDIO/SWCLK, la cible tournant a 2,5 V.


## CORRECTION IMPORTANTE : GIO3S=14 n'est PAS en amont du correlateur

L'entree ci-dessus « GIO3S=14 est en amont du correlateur » est **fausse**, et
avec elle la conclusion « la voie RF est close ». Le controle etait confondu :
l'adresse dite « fausse » (`44 33 22 E2`) ne differe de la vraie (`44 33 22 E1`)
que de **deux bits**, et le correlateur du BC5602 tolere quelques bits d'erreur.
Il acceptait donc encore les trames, ce qui donnait l'illusion d'une sortie
independante de l'adresse.

Refait avec la sequence de reception du projet amont, balise allumee :

| adresse du recepteur | ecart | transitions GIO3 | trames |
|---|---|---|---|
| `44 33 22 E1` | aucun | 28 506 | 789 |
| `44 33 22 E2` | 2 bits, premier octet sur l'air | 1 806 | 50 |
| `11 22 33 E2` | totalement differente | **0** | **0** |
| `11 22 33 44` | totalement differente, polarite opposee | **0** | **0** |

**Consequence.** Tous les resultats nuls de la journee -- 84 canaux, 3 debits,
2 polarites, 2 longueurs de preambule -- signifient « on n'a pas la bonne
adresse », et non « le signal est indetectable ». La voie RF n'est pas close.

**Lecon de methode.** Un controle negatif doit etre VRAIMENT negatif. Choisir
une adresse fausse a deux bits de la vraie, c'etait tester la tolerance du
correlateur en croyant tester son existence.

## Le chemin de reception du projet amont (qui fonctionne)

Tire de `prepare_halo_receive()` dans Termina1/benq-screenbar-halo2-esphome.
Deux differences de fond avec le notre, commande `amont` :

1. **Aucun reset logiciel.** Son commentaire : « Literal Pico lifecycle: no
   software reset during normal initialization. Hidden packet/PID/RF state is
   allowed to continue from hardware POR. » Nos deux chemins commencaient par un
   reset, qui efface 15 des 19 valeurs recommandees. Sans reset, celles ecrites
   par `begin()` survivent : le temoin affiche **18 sur 19**.
2. **Reception passive** : CRC desactive (`PKT1=0x00`), auto-ACK desactive
   (`ENAA=0x00`), payload dynamique desactive, longueur statique de 13 octets.

Mesure : **789 trames en 15 s** sur la balise, charge utile exacte
`DE AD 55 0F A0 3C 01 02 03 04` suivie du CRC `C2 BA`. Et **28 506 transitions
GIO3, soit 36 par trame**, contre 3 par trame avec notre ancien chemin.

Parametres du projet amont, identiques a ce qu'on avait deduit par la mesure :
adresse `9C EA BB 86` (4 octets, **codee en dur**, c'est une Halo 2), canal 5,
`DM1 = 0x82` (125 kbps, adresse de 4 octets).

Il ecrit aussi `XO1` (banque 0, registre `0x38`) a `0x15` avant la calibration du
VCO, dans son chemin d'emission en mode direct -- le **trim du quartz**, que nous
n'avons jamais touche. Valeur par defaut apres reset : `0x10`.

## Pourquoi il faut un nRF52840, et pas seulement un ESP32 (2026-09-22)

**La methode qui a reellement trouve l'adresse du Halo 2**, c'est un **HackRF
One + Universal Radio Hacker**, le 2026-04-14, par kuzmin-no (SK2024 sur le
forum HA). Son README initial le dit, et deux captures d'ecran du depot montrent
la session : 2,405 GHz, 2 MSps, FSK, 16 echantillons par symbole. Le script
`find_halo2_address.py` n'arrive que 2,5 mois plus tard, presente comme un moyen
de se passer du SDR, et **aucun log n'a jamais ete publie prouvant qu'il marche**.

Le projet `Termina1/benq-screenbar-halo2-esphome` ne resout pas la decouverte
d'adresse : `RADIO_ADDRESS{0x9C,0xEA,0xBB,0x86}` est codee en dur et le README
renvoie l'utilisateur a « capturer son propre trafic ». **L'adresse est propre a
chaque paire lampe/telecommande** ; seule celle d'appairage (`E2 08 00 B0`) est
universelle. Verifie : les trois adresses connues donnent 0 trame sur notre
Halo 1, sur un chemin qui en decode pourtant 789 de la balise.

**La voie nRF52840** (`xf_bc5602.py`) : ecouter a **1 Mbps** un signal emis a
125 kbps sur-echantillonne chaque bit par **8**. On pointe alors le mot de
synchro sur le **preambule sur-echantillonne** -- `0xAA` a 125 kbps devient
`FF 00 FF 00` a 1 Mbps, motif universel, identique sur tous les exemplaires --
au lieu de l'adresse qu'on ignore. Le decodeur decime par 8, cherche la position
ou le CRC tombe juste, et lit l'adresse dans le flux :

```
Frame: address(32) + PCF(9) + payload(80) + CRC-16(16)
CRC-16/CCITT (poly 0x1021, init 0xFFFF) over address + PCF + payload
"address": bytes(_byte(bits, o - 41 + 8 * i) for i in range(4))
```

**Le BC5602 ne peut pas faire cela, c'est mesure.** Son detecteur de preambule
doit s'armer sur une alternance AU DEBIT CONFIGURE, avant le correlateur
d'adresse. Balise a 125 kbps, recepteur sur-echantillonnant, payload de 32
octets, CRC coupe :

| debit RX | facteur | mot de synchro | trames | temoin |
|---|---|---|---|---|
| 500 kbps | x4 | `F0 F0 F0 F0` | **0** | 1293/11999 |
| 500 kbps | x4 | `0F 0F 0F 0F` | **0** | 1186/12000 |
| 250 kbps | x2 | `CC CC CC CC` | **0** | 695/12000 |
| 250 kbps | x2 | `33 33 33 33` | **0** | 692/12000 |

Et le BC5602 plafonne a 500 kbps, soit x4 au mieux. L'ESP32-C6, lui, n'expose
aucune interface de PHY brute : sa radio ne fait que Wi-Fi, BLE et 802.15.4.

**Conclusion : une carte nRF52840 (Seeed XIAO, ~13 $) est le chemin le moins
cher vers l'adresse.** Verifier qu'elle porte une antenne ceramique et pas un
simple connecteur u.FL nu.

## Capture brute au CC2500, et premiere adresse de la Halo 1 (2026-09-22)

Le CC2500 donne ce que le BC5602 refuse : un MODE SERIE ou le moteur de paquets
est debranche. `PKTCTRL0.PKT_FORMAT=01` sort les bits demodules sur GDO0 avec
l'horloge de bit recuperee sur GDO2, et `MDMCFG2.SYNC_MODE=000` supprime toute
exigence de preambule et de mot de synchro. Le datasheet prevoit exactement cet
usage : « The MCU must then handle preamble and sync word insertion and
detection in software. »

Module : 24TRGC5-V4 (GC-02), CC2500 + RFX2402E (PA/LNA), quartz 26 MHz, u.FL.
PARTNUM 0x80, VERSION 0x03. Table de verite de l'etage d'entree MESUREE :
`PA_EN=0, RX_EN=1` donne 18 dB de plus que les trois autres combinaisons.

**Le pilote est bit-bange**, volontairement : l'ESP32-C6 n'a qu'un controleur
SPI generaliste, deja pris par le BM5602, et il ne se re-route pas ensuite.
Mesure a l'appui, le peripherique rendait un octet d'etat 0x00 la ou le
bit-bang rendait 0x0F sur les MEMES broches.

**Chaine validee de bout en bout sur la balise.** Une trame lue dans le flux
brut, par une puce a qui aucune adresse n'a ete donnee :

```
FF FF FF C0 2A AA | E1 22 33 44 | DE AD 55 0F A0 3C 01 02 03 04 | C2 BA
     repos          adresse            charge utile                CRC
```

**Ce qui a ete mesure sur la telecommande, et non plus deduit :**

- elle emet sur 2405 MHz : bandes fortes x8,76 quand la molette tourne, plancher
  immobile (commande `ccpres`) ;
- a **125 kbps** : duree d'un bit mesuree en mode asynchrone, pic a 8,08 us,
  contre 8,09 us pour la balise a 125 kbps connus (commande `ccbit`) ;
- sa porteuse est **bien centree** : FREQEST donne une masse a 0-31 kHz, contre
  0-15 kHz pour la balise (commande `ccoff`) ;
- elle est **forte** : pic a -19 dBm.

**Adresse trouvee : `8F F7 C1 3C` sur l'air, soit `3C C1 F7 8F` en ordre
d'ecriture.** Trouvee par recherche des sequences repetees dans 956 672 bits de
flux brut, sans aucune hypothese de CRC ni de structure. Quatre occurrences,
trois precedees d'un preambule, toutes suivies de charges utiles DIFFERENTES :

```
FC 00 55 | 8F F7 C1 3C | 53 13 11 6E 07 CF DF ...
FC 00 55 | 8F F7 C1 3C | 25 89 67 E2 20 BE 7F ...
7F 7C 00 05 | 8F F7 C1 3C | 06 B9 21 BD FF FE ...
77 B6 01 55 | 8F F7 C1 3C | 06 B9 21 BF FF FE ...
```

Le preambule fait **un octet** (`55`), la ou notre balise en emet deux.

**ADRESSE CONFIRMEE** par le correlateur materiel du BM5602, avec son controle :

| adresse ecrite | duree | transitions GIO3 | trames |
|---|---|---|---|
| `3C C1 F7 8F` | 90 s | 200 | **7** |
| `3C C1 F7 8E` (un bit d'ecart) | 40 s | 0 | **0** |
| `8F F7 C1 3C` (ordre inverse) | 30 s | 0 | **0** |

Et corroboration croisee entre deux radios et deux chaines d'analyse
independantes : le prefixe de charge utile `06 B9 21 B*` apparait a la fois dans
le flux brut du CC2500 (passes 21 et 25 : `06 B9 21 BD`, `06 B9 21 BF`) et dans
une trame decodee par le moteur de paquets du BM5602 (`06 B9 21 BB`).

Sept trames en 90 s reste peu : les erreurs binaires en rejettent la plupart.
C'est desormais une question de rapport signal sur bruit, plus de protocole. C'est le juge le plus dur dont on dispose -- il decode 789 trames de la
balise et rigoureusement aucune avec une adresse fausse.

**Aucun modele de CRC ne valide ces trames** : 84 combinaisons de polynome,
d'etat initial et de sens de bits, sur sept points de depart. Les trames portent
donc des erreurs binaires, ce qui explique aussi leur rarete.

**Methodes essayees et ECARTEES, chacune par un controle sur la balise :**
l'ancrage sur l'alternance du preambule (sort surtout les `0x55` de la charge
utile), le consensus sur regions actives (37 bits unanimes sur la balise aussi,
donc sans valeur), et toute capture DECLENCHEE sur le RSSI -- lire le RSSI coute
190 us quand preambule et adresse n'en durent que 384 : l'adresse est passee
avant qu'on echantillonne. Seules la chasse par CRC et la recherche de
sequences repetees ont survecu a leur controle.

## Structure de trame Halo 1, confirmee sans le CRC (2026-09-22, nuit)

L'analyse des captures brutes de 32 octets du BM5602 donne une confirmation
INDEPENDANTE du CRC : l'adresse de la retransmission suivante se trouve
systematiquement au bit **72 ou 73** apres celle de la trame en cours, deux fois
avec **zero bit faux**. Or 72 = 48 + 16 + 8 :

```
| adresse 32 b | charge utile 48 b | CRC 16 b | preambule 8 b | adresse suivante...
```

Soit six octets de charge utile, deux de CRC, un octet de preambule. Cela
recoupe exactement la seule trame dont le CRC a valide
(`06 B9 21 BB 98 FF` + `7A FF`) et confirme que le Halo 1 utilise six octets la
ou le Halo 2 en utilise dix.

**Hypotheses ecartees par la mesure :**

- *Ecart de debit.* Balayage fin du CC2500, DRATE_M de 46 a 72, soit 119,8 a
  130,1 kbit/s par pas de 0,32 % : les detections d'adresse sont reparties
  uniformement sur toute la plage, **sans pic**, et aucun CRC ne valide nulle
  part. Un ecart de debit aurait donne un maximum franc.
- *Distance.* Les deux modules sont a 25 cm l'un de l'autre : la telecommande
  etait deja proche du BM5602.
- *Vote majoritaire.* Sans objet en l'etat : les rafales ne livrent qu'une ou
  deux copies, jamais les trois qu'un vote demande.

**Anomalie a reprendre en priorite.** La meme commande donne
`06 B9 21 BB 98 FF` + `7A FF` (CRC VALIDE) dans une lecture de 13 octets, et
`06 B9 21 BB FF 3F` + `7F 9B` dans une lecture de 32. Les quatre premiers
octets concordent, les suivants non. **Changer RXPW0 change le contenu recu**,
ce qui ne devrait pas arriver et explique probablement le faible rendement.
Reprendre avec `RXPW0 = 8`, la valeur qui a produit la seule trame valide.

**Le chien de garde des interruptions** se declenchait sur les captures : 32768
bits a 125 kbit/s font 260 ms d'interruptions masquees pour un seuil de 300.
Toutes les captures passent desormais par `ccSampleBits`, qui masque par
tranches de 8192 bits.

## Deux familles de trames : commandes et accuses de reception (2026-09-23)

Le premier octet de la charge utile se range en deux familles, et dans chacune
les bits 3-2 forment un **compteur de sequence sur deux bits** :

```
famille A : 02 06 0A 0E          = 0000 PP 10
famille B : 21 25 29 2D, puis 89 = 0010 PP 01
```

Dans les captures de 32 octets, une trame B est **toujours** suivie, 72 bits
plus loin, d'une trame A portant **le meme compteur** : 21->02, 25->06, 29->0A,
2D->0E. Ce sont des paires question-reponse, dans l'ordre B puis A.

Interpretation retenue, par trois indices concordants :

- **l'ordre** : B precede A ;
- **le contenu** : dans les trames A, les octets 2-3 ne dependent que du
  compteur (`06` -> `B9 21`, `0A` -> `78 AC`, `02` -> `F9 A5`, `0E` -> `38 28`),
  comme un accuse ; dans les trames B, l'octet 2 vaut toujours `89` et l'octet
  3 varie a chaque capture, comme une valeur de molette ;
- **la qualite de reception** : les trames A se recoivent nettement mieux --
  la seule trame au CRC valide de la soiree est une A -- ce qui designe deux
  emetteurs differents.

Donc **B = commande de la telecommande, A = accuse de la lampe**. C'est
l'INVERSE de la convention du Halo 2 (« odd PID frames are lamp replies »).

**Consequence mesuree.** La trame rejouee `06 B9 21 BB 98 FF` (verifiee bit
pour bit par le CC2500, CRC `7A FF`) n'a produit aucune reaction de la lampe,
mise au minimum pour l'occasion : c'etait un accuse de reception, pas une
commande.

**Reparation par le biais : sans resultat.** Les erreurs etant a 100 % des 1
lus comme 0, on a tente de reparer 49 captures en remettant a 1 jusqu'a trois
zeros. Trois trames passent le CRC, mais chacune au maximum de corrections, sans
repetition, et le hasard en predit environ quatre sur ce volume : faux
positifs.

**Correction d'une conclusion anterieure.** Le rejet de toutes les trames par
le CRC materiel ne tenait pas a un modele different : pendant cette seance,
aucune trame ne passait non plus le CRC logiciel. Sur la balise, le CRC
materiel produit exactement notre modele, et on l'utilise desormais en
emission.

**Hypotheses de reception eliminees par la mesure** (trame A de reference ou
comptage de trames, en alternance quand c'etait possible) : longueur de
lecture, distance et saturation, trim du quartz (calibre : environ 3 kHz par
cran, 87 kHz de plage), valeurs analogiques par defaut contre recommandees.

**Ce qui bloque maintenant** : obtenir une trame B exacte. Nos radios decodent
proprement nos propres emissions et assez bien celles de la lampe ; c'est le
signal de la telecommande qu'elles digerent mal.

## Premiere lecture de la charge (ecoute de la telecommande, 23/09)

Capture `logs/ecoute-tele2.log` (outil `ecoute 4FF0FD63 5`), gestes : minimum,
maximum, un cran bas, eteindre/rallumer, temperature, autres boutons (sans
pauses). Lecture provisoire, a confirmer une par une par `txack` :

| charge | observe | lecture provisoire |
|---|---|---|
| `C4 xx` | xx balaie `4C` (minimum tenu) a `FE` (maximum tenu) avec la molette | luminosite, lampe allumee |
| `44 xx` / `C4 xx` alternes, meme xx | pendant eteindre/rallumer | bit 7 du 1er octet = marche/arret ? |
| `C2 xx` | xx balaie `00` a `64` (0 a 100) | temperature de couleur en pourcentage ? |
| `83 xx`, `C3 xx` | meme xx que `C2` | variantes de `C2` (marche/arret ?) |
| `FF 00`, `FE 00`, `FD 00` | en debut et entre les groupes de gestes | reveil / etat ? |
| `E0 01`, `E0 02`, `FA A8`, `83 35`, `85 A7`, `91 00`, `89 58`, `89 E0` | « autres boutons » | inconnus |

**Bouton de switch de lampe** (capture `logs/btn-switch.log`, appuis repetes ;
la lampe cycle avant -> arriere -> les deux -> avant). Sequence decodee :
`C3 35`, `C2 35`, `C2 35` (apres un reveil `FF 00 FD 00 FF 00`), `83 35`,
`C3 35` -- chaque etat emis 3 fois. L'ordre C3 -> C2 -> 83 -> C3 est bien le
cycle les deux -> avant -> arriere -> les deux. Le 2e octet (`35` = 53) ne
bouge pas : c'est la temperature de couleur courante, renvoyee avec le mode.

| 1er octet | bits | lampe |
|---|---|---|
| `C2` | `1100 0010` | avant seule |
| `83` | `1000 0011` | arriere seule |
| `C3` | `1100 0011` | les deux |

Lecture du 1er octet comme champ de bits, **a confirmer par emission** :
bit 7 = marche, bit 6 = lampe avant, bit 0 = lampe arriere, bit 1 = le 2e
octet est la temperature, bit 2 = le 2e octet est la luminosite. Elle explique
toutes les valeurs deja vues : `C4` (luminosite, avant), `C5` (luminosite, les
deux), `85` (luminosite, arriere), `44` (eteinte, avant), `C2/83/C3`
(temperature + mode). La telecommande envoie donc des ETATS ABSOLUS et non des
bascules -- d'ou le renvoi sans risque du meme etat (`C2 35` deux fois).

**Bouton A et bouton favori** (captures `logs/btn-A.log`, `btn-A2.log`,
`btn-A3.log` ; l'ecoute horodate desormais chaque trame a la milliseconde).
Chronologie de `btn-A3.log` (reveil par le bouton favori, puis bouton A) :

| t (ms) | charge | lecture |
|---|---|---|
| 2532-2940 | `FF 00` `FF 00` `FE 00` `FF 00` `FD 00` `FF 00` | reveil |
| 3043 | `FA A8`, NO_ACK=1 | seule trame sans demande d'accuse |
| 3143 | `83 35` | mode arriere seule, temperature 53 |
| 3244-3345 | `85 A7` x2 | luminosite A7, arriere seule |
| 3446-3548 | `91 00` x2 | bit 4, arriere seule |
| 4048-4250 | `89 58`, `89 E0` x2 | bit 3, arriere seule |
| 7588 | `A1 01` | bouton A, compteur 1 |
| 10892-11304 | reveil | |
| 13202-13405 | `A1 02` x2 | bouton A, compteur 2 |
| 16445-16824 | reveil | |
| 17042-17244 | `A1 03` x3 | bouton A, compteur 3 |

Le bloc 3143-4250 (favori) rejoue un etat complet : mode, luminosite, puis
deux reglages encore inconnus (bits 3 et 4). C'est exactement la liste des
« autres boutons » de la premiere capture. Le bouton A emet `E0 nn` quand la
lampe avant seule est active, `A1 nn` en arriere seule, `60 01` une fois
(bit 7 a zero) : le haut et le bas de l'octet suivent le mode de lampe, le
bit 5 designe le bouton A. Le 2e octet compte les appuis successifs sur A
(01, 02, 03... jusqu'a 05 vu) et repart a 01 apres une pause ou une autre
commande. Constat de l'utilisateur sur `btn-A3.log` : apres le favori, la
lampe est bien passee en **arriere seule** (confirme la lecture de `83`,
predite avant son retour) ; **deux appuis brefs** sur A, **rien de visible**
-- mais trois trames `A1 01/02/03`. Le 1 pour 1 appui/trame n'est donc pas
acquis : la telecommande pourrait emettre A d'elle-meme (mode automatique
entretenu ?). A trancher par un appui unique suivi d'un long silence.

Lecture de travail du 1er octet (a confirmer par emission) :

| bit | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| sens | marche | avant | bouton A | reglage ? | reglage ? | luminosite | temperature | arriere |

`FF`, `FE`, `FD`, `FA` (tous les bits hauts a 1) sortent de ce schema :
trames de service (reveil, annonce).

**Appui unique sur A** (`logs/btn-A4.log`, lampe en avant seule, puis 21 s
sans toucher la telecommande) : un seul evenement `E0 01` x3 a 9,8 s, puis
un reveil sans commande a 13,1 s, puis silence. La telecommande n'emet donc
pas A d'elle-meme ; les trames en trop des essais precedents etaient des
doubles detections du bouton tactile. Effet observe : la lampe avant
**baisse puis remonte**, la temperature semble bouger aussi (incertain).
Lecture : A = bascule du mode automatique (capteur), 2e octet = numero
d'appui pour que la lampe ignore les repetitions.

**Verification sur toutes les captures** (tous les `logs/*.log`, 18 valeurs
distinctes de 1er octet) : `05 42 43 44 60 83 85 89 91 A1 C2 C3 C4 E0` ont
toutes exactement un bit de reglage parmi les bits 1 a 5, et au moins une
lampe (bit 6 ou bit 0) ; les seules exceptions sont `FA FD FE FF`. Le bouton
marche/arret renvoie la derniere trame d'etat avec le bit 7 inverse, dans
chaque mode : `C4 ED`/`44 ED` (avant, apres la molette), `85 A7`/`05 A7`
(arriere), `C3 35`/`43 35` (les deux), `C2 35`/`42 35` (avant).
Le favori a ete rejoue deux fois dans `ecoute-tele2.log` : `83 35`, `85 A7`,
`91 00`, puis `89 58` ou `89 00`, puis `89 E0`.

### Semantique CONFIRMEE par emission (23/09, telecommande sans piles)

Outil : `txack 4FF0FD63 5 <charge> 3 300` (trois trames, 300 ms d'ecart,
comme la telecommande). Logs `logs/tx-sem-*.log`. Chaque ligne : 3 accuses
sur 3, puis observation de l'utilisateur, prediction ecrite AVANT.

| test | charge | prediction | observe |
|---|---|---|---|
| 1 | `C3 35` x1 | les deux lampes | **rien** (voir plus bas) |
| 1 bis | `C3 35` x3 | les deux lampes | les deux lampes |
| 2 | `83 35` | arriere seule | arriere seule |
| 3 | `C2 35` | avant seule | avant seule |
| 4a | `C2 00` | un extreme de temperature | **le plus froid** |
| 4b | `C2 64` | l'autre extreme | **le plus chaud** |
| 5a | `42 64` | extinction | eteinte |
| 5b | `C3 35` | rallumage, les deux, temperature moyenne | les trois a la fois |
| 6a | `E1 01` (jamais vu, construit) | A : baisse puis remonte, les deux restent | conforme |
| 6b | `E1 01` renvoye deux fois | rien (numero deja traite) | rien, deux fois |
| 6c | `E1 02` | nouvelle reaction | baisse puis remonte |

Acquis :
- 1er octet = champ de bits : b7 marche, b6 avant, b0 arriere, b5 bouton A,
  b2 luminosite, b1 temperature (b3, b4 : favori, non testes). Une valeur
  jamais emise par la telecommande (`E1`) est comprise : la lecture en champ
  de bits est la bonne, pas une table de codes.
- Les trames d'etat sont ABSOLUES : mode, marche et temperature s'imposent
  quel que soit l'etat precedent, et plusieurs en une trame (test 5b).
- Temperature : `00` = le plus froid, `64` (100) = le plus chaud.
- Bouton A : evenement, 2e octet = numero d'appui ; un numero deja traite est
  ignore (6b). Effet visible identique a chaque nouveau numero (baisse puis
  remonte) : bascule ou relance du reglage automatique, non tranche.
- **Une trame unique n'a pas suffi** (test 1), trois oui. Hypotheses : la
  premiere trame reveille le microcontroleur de la lampe, ou la lampe
  ecarte une trame dont le PID egale celui de la derniere recue. A etudier ;
  en attendant, emettre chaque commande trois fois comme la telecommande.

Luminosite avec les deux lampes (`C5`, bits marche + avant + luminosite +
arriere) :

| test | charge | observe |
|---|---|---|
| 7 | `C5 60` | baisse legere, les deux lampes restent allumees |
| 7b | `C5 4C` | minimum de la molette |
| 7c/7d | `C5 20`, puis `4C`/`20` alternes toutes les 4 s | aucun changement visible |

- La lampe **plafonne sous `4C`** : c'est son minimum reel, pas seulement
  celui de la molette. Plage utile `4C`-`FE`.
- Perception (utilisateur) : la courbe 0 -> 100 parait logarithmique ; avec
  les deux lampes allumees, chacune eclaire moins que seule (puissance
  partagee). Pour Matter : conversion non lineaire du niveau vers `4C`-`FE`.

Favori et bits 3 / 4 :

| test | charge | observe |
|---|---|---|
| 8 | salve du favori `83 35`, `85 A7`, `91 00`, (`89 E0` incertain) | arriere seule, luminosite plus forte : le favori se rejoue sans la telecommande |
| 9a | `C3 35`, 4 s, `C9 58` | passage aux deux lampes, puis rien |
| 9b/9c | `C9 E0` / `C9 00` alternes toutes les 4 s | rien (deux fois, lampe regardee) |
| 10a/10b | `D1 01`, `D1 00`, `D1 64` toutes les 6 s | rien (deux fois, lampe regardee) |

Bits 3 et 4 : **aucun effet visible**, deux lampes allumees, valeurs opposees.
Reglages internes (cible ou etat du mode automatique ? memoire du favori ?),
sans utilite pour la commande depuis Matter. Non poursuivi.

### Appairage Halo 1 (23/09, format de trame desormais correct)

Manipulation : maintenir **favori + switch de lampe ~5 s** ; toutes les LED
de la telecommande clignotent. Appui long sur favori = enregistrer le preset
**dans la telecommande** (LED en retour ; la lampe ne recoit que les trames
d'etat du rappel).

| capture | adresse, canal | pendant la manip |
|---|---|---|
| `logs/pair-1.log` | `63 FD F0 4F`, 5 (lien normal) | rien ; avant et apres, trafic normal |
| `logs/pair-2.log` | `E2 08 00 B0`, 5 (appairage Halo 2) | 0 trame, 0 rejet en 40 s |

- L'appairage de la Halo 1 ne passe ni par le lien normal, ni par l'adresse
  d'appairage de la Halo 2 sur le canal 5 : autre adresse et/ou autre canal.
  Seule voie restante : balayage au CC2500 (energie par canal, puis capture
  brute). Non necessaire pour piloter la lampe ; laisse en option.
- **L'adresse n'a pas change** : juste apres la manip, la molette emet sur
  `63 FD F0 4F` (`85 A6` -> `85 FE`, pair-1.log, 36-40 s).
- La trame de service `FA xx` (NO_ACK) valait `A8` partout avant, `F8` juste
  apres la remise des piles et la manip (bits 6 et 4) : niveau de pile ou
  drapeau, non tranche.

**Adresse d'appairage Halo 1 TROUVEE** (23/09, CC2500, `logs/trig-2-appairage.log`,
analyse `tools/pairing/ana.py`) :
- Balayage d'energie (`ccscan`) : rien de net hors du canal 5 ; puis capture
  brute sur 2405 MHz gardee sur detection de porteuse (`cctrig`, seuil
  RELATIF +14 dB : en absolu, meme +7 dB laissait 18 % de porteuse au repos).
- Temoin (molette, `trig-1-molette.log`) : l'analyse aveugle, sans connaitre
  l'adresse, sort des trames au CRC juste `63 FD F0 4F` / `C4 D3`, `C4 CB`.
- Pendant la manip favori + switch : trames standard, **125 kbps, canal 5,
  adresse sur l'air `59 01 00 B0`** (a ecrire `B0 00 01 59`), longueur 2,
  PID 0, NO_ACK 0, CRC juste. Charge en cycle, une par salve toutes les
  ~200 ms : `5A 5A` -> `F5 C3` -> `CF 49` -> ... Chaque charge part jusqu'a 3
  fois a ~1,85 ms d'ecart : ce sont des RETRANSMISSIONS faute d'accuse -- la
  lampe, pas en mode appairage, ne repond pas.
- Parente avec la Halo 2 : adresse d'appairage `E2 08 00 B0`, meme fin `00 B0`.
- `F5 C3 CF 49` (identifiant de la telecommande ?) n'a aucun lien simple avec
  `63 FD F0 4F` (XOR, inversions, complement, CRC-16 essayes). L'adresse de
  lien vient peut-etre de la lampe, dans son accuse, pendant un vrai appairage.
- Les « trames » `FFFF0000` longueur 0 vues a 1000 kbps sont des artefacts de
  sur-echantillonnage (flux constant), a ignorer.

**Appairage COMPLET capture** (23/09, procedure du manuel : lampe debranchee,
switch + favori 5 s, capteur couvert, USB rebranche dans les 15 s ; BM5602 sur
`59 01 00 B0` = `logs/pair-4-bm.log`, CC2500 brut = `logs/pair-4-cc.log`,
liste `tools/pairing/frames.py logs/pair-4-cc.log 125 99.84`) :

| t (s) | trafic |
|---|---|
| 0-2,2 et 5,7-8,0 | lien normal `63 FD F0 4F` : `FF 00`/`FE 00`/`FD 00` toutes les ~49 ms (manip en cours), puis `85 E2` x3 |
| 8,5-14,6 | balise `59 01 00 B0` : `5A 5A`, `F5 C3`, `CF 49` toutes les 200 ms, 2-3 essais chacune, PID fige : pas d'accuse |
| 14,76 | **premier accuse de la lampe** (USB rebranche) : longueur 0, PID 1, bit NO_ACK a 1 |
| 14,8-17,7 | balise emise UNE fois par charge, PID qui avance : chaque trame est accusee (accuse vide, ~0,83 ms apres) |
| 17,75 | fin : la telecommande s'arrete (appairage reussi) |

- **L'accuse de la lampe est VIDE** : la lampe n'attribue rien. Elle apprend
  l'identite de la telecommande dans la balise (coherent avec le manuel : une
  telecommande, plusieurs lampes). L'adresse de lien `63 FD F0 4F` est donc
  une fonction de `5A 5A / F5 C3 / CF 49` (ou de l'identifiant interne de la
  telecommande) -- fonction non identifiee.
- Consequence pratique : l'ESP32 peut RE-APPAIRER la lampe a l'adresse
  connue en rejouant la balise (`59 01 00 B0`, canal 5, charges dans l'ordre,
  accuse demande) pendant la fenetre d'appairage de la lampe.
- **L'adresse survit a l'appairage** (`logs/pair-5-verif.log`) : juste apres,
  la molette emet 121 trames sur `63 FD F0 4F` et le PID avance a chaque
  trame (emission unique, donc accusee) ; la lampe obeit. Voyants eteints a
  la fin de la manip : appairage reussi. Piste appairage CLOSE.
