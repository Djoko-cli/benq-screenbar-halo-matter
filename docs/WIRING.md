# Câblage

## Module BM5602-60-1 → ESP32

Le BM5602 démarre en SPI **3 fils** (`SDIO` bidirectionnel). Le firmware bascule
le registre `IO1` en mode **4 fils** dès l'init : `GIO2` devient alors la sortie
de données, donc le MISO côté ESP32.

## Brochage du module BM5602-60-1

Le module **n'a aucun marquage sérigraphié** sur ses pastilles. Oriente-le
antenne en haut, texte `BM5602-60-1 V1.0` lisible : les 9 pastilles du bord
inférieur sont alors, **de gauche à droite** :

| # | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| | `VSS` | `VDD` | `GIO1` | `CSN` | `SCK` | `GIO2` | `SDIO` | `GIO3` | `GIO4` |
| | GND | 3V3 | — | CS | horloge | **MISO** | **MOSI** | — | — |

`GIO1`, `GIO3` et `GIO4` ne servent pas. Les deux pastilles isolées sur les
bords gauche et droit, près de l'antenne, sont des `VSS` supplémentaires.

Source : le schéma de câblage du
[projet Halo 2](https://github.com/kuzmin-no/BenQ_ScreenBar_HALO_2_HA_integration/blob/main/img/connection_diagramm.png).

| BM5602-60-1 | Rôle SPI | **C6 SuperMini** | ESP32-C3 | ESP32-S3 | ESP32 (WROOM) |
|---|---|---|---|---|---|
| `VDD`  | 3,3 V | 3V3 | 3V3 | 3V3 | 3V3 |
| `VSS`  | GND | GND | GND | GND | GND |
| `SCK`  | horloge | **IO18** | GPIO 4 | GPIO 12 | GPIO 18 |
| `GIO2` | MISO | **IO19** | GPIO 5 | GPIO 13 | GPIO 19 |
| `SDIO` | MOSI | **IO20** | GPIO 6 | GPIO 11 | GPIO 23 |
| `CSN`  | chip select | **IO14** | GPIO 7 | GPIO 10 | GPIO 5 |

### Pourquoi IO14 et IO18–20 sur le C6 SuperMini

Cette carte a peu de broches vraiment libres :

- **IO12 / IO13** portent l'USB natif — les utiliser coupe le port série ;
- **IO2, IO4, IO5, IO8, IO9, IO15** sont des broches de *strapping* : leur
  niveau est échantillonné au reset. Un module qui en pilote une pendant le
  démarrage peut empêcher la carte de booter ;
- **IO8** est une LED adressable WS2812, pas une LED simple ;
- **IO15** pilote la LED d'état, **IO9** est le bouton BOOT — le firmware s'en
  sert déjà.

Restent **IO14** et **IO18–IO20**, toutes sur le connecteur **extérieur
gauche**, dont l'ordre est `6 · 14 · 15 · 18 · 19 · 20 · 3V3 · GND · 5V`. Les
six fils, alimentation comprise, tiennent donc sur une seule rangée :

```
IO14 ── CSN
IO15    (LED d'état, on saute)
IO18 ── SCK
IO19 ── GIO2
IO20 ── SDIO
3V3  ── VDD
GND  ── VSS
```

IO21 et IO22 existent aussi, mais en **trous intérieurs** et non sur le bord
castellé : pénibles à souder, à éviter.

Les broches sont définies par `build_flags` dans [platformio.ini](../platformio.ini) —
change-les là plutôt que dans le code.

> **3,3 V uniquement.** Le BC5602 ne tolère pas le 5 V. L'ESP32 étant lui aussi
> en 3,3 V, aucun adaptateur de niveau n'est nécessaire.

## Points à ne pas négliger

**Découplage (optionnel).** Un 100 nF **plus** un 10 µF au plus près de `VDD`
est une bonne pratique peu coûteuse. Mais tout le projet a été mesuré **sans**
(0 perte au banc hors Thread, 23/09), et le module a vraisemblablement son
propre découplage : rien n'a jamais montré qu'il était nécessaire.

**Cohabitation 2,4 GHz.** L'ESP32 émet en Wi-Fi jusqu'à +20 dBm ; le BenQ
travaille à 2405 MHz, en plein sur le canal Wi-Fi 1. Deux précautions :

- éloigne physiquement le module de l'antenne de l'ESP32 (10–15 cm de nappe
  suffisent, le SPI à 1 MHz supporte très bien) ;
- mets ton point d'accès Wi-Fi sur le canal 11 (2462 MHz) si tu le peux.

Sans ça, la réception du BM5602 est désensibilisée à chaque émission Wi-Fi et
les trames de la télécommande passent à la trappe.

**Longueur du bus SPI.** 1 MHz par défaut (`RF_SPI_HZ` dans
[src/config.h](../src/config.h)). Inutile de monter plus haut : un payload fait
10 octets. Si tu utilises une nappe longue, descends plutôt à 500 kHz.

**Antenne.** Le BM5602-60-1 embarque une antenne imprimée. Ne la colle pas
contre une masse, un blindage ou un boîtier métallique.

## Vérification

Au démarrage, le moniteur série doit afficher :

```
=== BenQ ScreenBar Halo -> Matter ===
firmware 0.2.0
```

puis, avec `info` :

```
  BM5602        : detecte (version puce 0x......)
```

`ABSENT` signifie que la lecture SPI renvoie `0x000000` ou `0xFFFFFF` :
- `0xFFFFFF` → MISO n'est pas relié, ou relié ailleurs que sur `GIO2` ;
- `0x000000` → pas d'alimentation, `CSN` non relié, ou SCK/MOSI inversés.
