# Brief de passation : boitier imprime 3D du module Halo (ESP32-C6 SuperMini + BM5602)

> Destinataire : une instance Claude (Opus) chargee de modeliser le boitier et de
> rediger la notice de soudure et d'assemblage. Redige le 23/09/2026 par l'instance
> qui a developpe le firmware. Interlocuteur : Majid (francophone, a l'aise en
> electronique ; il a longtemps prefere eviter la soudure, il accepte maintenant
> une soudure propre et definitive pour la version finale).

## 1. Contexte en trois lignes

- Le module pilote une lampe BenQ ScreenBar Halo (1re generation) par radio 2,4 GHz
  (canal 5 = 2405 MHz, via le transceiver Holtek BM5602-60-1) et l'expose dans
  Apple Home en **Matter sur Thread** (radio 802.15.4 de l'ESP32-C6, canal 25 = 2475 MHz).
- Le firmware est termine et valide sur la vraie lampe (depot : ce dossier ; build
  `esp32c6thread` dans `platformio.ini`). **Aucune broche ne doit changer** : elles
  sont figees dans les `build_flags`.
- Objectif de ta mission : un **petit boitier compact, visse sous le bureau, alimente
  en USB-C**, plus la **notice definitive de soudure et d'assemblage**, avec le
  BM5602 monte en « HAT » au-dessus de l'ESP32.

## 2. Les deux cartes

**ESP32-C6 SuperMini** (carte de Majid, 4 Mo de flash) :
- environ 22,5 x 18 mm (**a mesurer** : cote exacte, epaisseur PCB, hauteur des
  composants dessus et dessous) ;
- USB-C sur un petit cote, **antenne ceramique/PCB a l'autre petit cote** (a
  confirmer sur la carte reelle) ;
- boutons **BOOT (IO9)** et **RESET** pres de l'USB ; LED d'etat du firmware :
  la **WS2812 sur IO8** (la LED simple d'IO15 reste eteinte) ;
- connecteur exterieur gauche, pas de 2,54 mm, dans cet ordre :
  `6 · 14 · 15 · 18 · 19 · 20 · 3V3 · GND · 5V`.
  **IO21 et IO22 sont des trous interieurs** : ne pas les utiliser.

**BM5602-60-1** (module Holtek, antenne imprimee integree) :
- dimensions **a mesurer** (aucune fiche fiable) ;
- aucune serigraphie. Antenne en haut, texte `BM5602-60-1 V1.0` lisible : les
  9 pastilles du bord inferieur sont, de gauche a droite,
  `VSS · VDD · GIO1 · CSN · SCK · GIO2 · SDIO · GIO3 · GIO4`.
  Deux pastilles isolees pres de l'antenne, a gauche et a droite, sont des VSS
  supplementaires (non necessaires).
- **3,3 V uniquement** : ne jamais le relier au 5 V.

## 3. Brochage DEFINITIF (six fils, rien d'autre)

| BM5602 (pastille n°) | Signal | ESP32-C6 SuperMini |
|---|---|---|
| VSS (1) | masse | **GND** |
| VDD (2) | 3,3 V | **3V3** |
| CSN (4) | chip select | **IO14** |
| SCK (5) | horloge SPI | **IO18** |
| GIO2 (6) | MISO (le firmware passe le module en SPI 4 fils) | **IO19** |
| SDIO (7) | MOSI | **IO20** |

A **ne pas** cabler dans la version finale : GIO1, GIO3, GIO4 (GIO3 -> IO3 ne servait
qu'aux diagnostics), IO10 (second module du banc), les fils du CC2500. La WS2812
(IO8) et IO9 (BOOT) sont sur la carte : rien a souder, seulement a rendre accessibles
(section 5).

**Decouplage : OPTIONNEL.** Toutes les mesures du projet ont ete faites SANS
condensateur ajoute (0 perte au banc hors Thread) et le module a tres
probablement son propre decouplage. Un **10 uF + 100 nF** ceramiques au plus pres
des pastilles VDD/VSS reste une bonne pratique peu couteuse ; ne le presente pas
comme obligatoire, et ne lui attribue aucun effet non mesure.

## 4. Contraintes radio (les plus importantes du projet)

Deux radios 2,4 GHz cohabitent a quelques millimetres : le BM5602 (2405 MHz, doit
entendre les accuses faibles de la lampe et la telecommande) et l'ESP32-C6 (Thread
a +20 dBm, 2475 MHz). Sur le banc, des paquets vers la lampe se perdent par episodes
quand Thread est actif ; la cause n'est pas prouvee, **mais l'eloignement des
antennes est la seule parade materielle**. Regles :

1. **L'antenne imprimee du BM5602 ne doit survoler NI l'antenne du C6, NI un plan
   de masse, NI un composant.** Elle doit deborder du PCB du C6, dans l'air.
2. Mettre les deux antennes aux **extremites opposees** du boitier ; viser
   **au moins 25-30 mm** entre elles (plus si le boitier le permet), et idealement
   des orientations **perpendiculaires**.
3. Disposition « HAT » recommandee : BM5602 au-dessus de la moitie USB du C6,
   **tourne de 90°** pour que son antenne deborde d'un grand cote du C6, cote USB,
   loin a la fois de l'antenne du C6 (autre extremite) et de la coque metallique
   de la prise USB-C. Propose aussi une variante cote a cote si elle eloigne mieux
   les antennes pour un encombrement comparable, avec schema, et laisse Majid choisir.
4. **Aucun metal** dans le boitier pres des antennes : pas d'insert laiton ni de vis
   a moins de ~10 mm d'une antenne, pas de peinture metallisee. Plastique plein
   (PETG ou PLA), paroi de 1,6 a 2 mm devant les antennes.
5. Montage sous le bureau : un plateau en bois ne gene presque pas. **Un plateau ou
   un cadre metallique, si** : le signaler dans la notice (monter pres du bord
   avant, antennes hors du metal). La lampe est posee sur l'ecran, au-dessus du
   bureau, a ~1 m.
6. Les fils SPI ne doivent pas passer sur les antennes.

## 5. Exigences du boitier

- **Compact** : le plus petit possible en respectant la section 4 ; donne les cotes
  exterieures finales.
- **Fixation sous le bureau** : deux oreilles avec trous fraises pour vis a bois
  ~3,5 mm (tetes loin des antennes), ou trous en boutonniere (keyhole) ; en option,
  une face plane pour adhesif double face.
- **USB-C** : ouverture ajustee au connecteur de la carte (mesure reelle + 0,3 mm),
  la prise doit pouvoir s'enfoncer entierement ; prevoir le passage et un petit
  arret de traction pour le cable. Le boitier doit rester **reflashable par l'USB**.
- **LED d'etat (WS2812, IO8)** visible : fenetre fine ou guide de lumiere au-dessus
  de la WS2812 (pas de la LED d'IO15). Signification : bleu clignotant = pas encore
  appaire, orange lent = pas de reseau, eteinte avec une breve lueur blanche toutes
  les 10 s = tout va bien (detail : README, « LED d'etat »).
- **Bouton BOOT (IO9)** accessible par un trou d'epingle : **appui court =
  redemarrage**, **8 s puis relacher = retrait de Matter** (decommission) ;
  entre 2 et 8 s, rien (detail : README, « Bouton BOOT »). Trou RESET
  optionnel : le bouton BOOT suffit a redemarrer.
- Maintien des cartes sans colle sur les antennes : berceaux, nervures, clips ;
  cale isolante (entretoise imprimee ou Kapton) entre le C6 et le BM5602.
- Couvercle a clips ou vis M2 (hors zone antennes). Jeux de 0,2 a 0,3 mm.
- Materiau conseille : **PETG** (tenue en chaleur sous un bureau), couches de 0,2 mm,
  impression sans supports si possible ; indique l'orientation d'impression.
- Marquage en relief discret (« Halo ») et repere d'orientation de montage.

## 6. Notice de soudure et d'assemblage a rediger (pour Majid)

Ecris une notice pas a pas, illustree si possible, qui couvre au minimum :

1. **Materiel** : fer a panne fine (biseau 1-1,5 mm), 320-340 °C ; etain 0,5 mm
   (63/37 plombe plus facile, ou sans plomb a 350 °C) ; flux ; fil **30 AWG**
   (silicone souple ou Kynar a wrapper) en 3 couleurs au moins ; pince a denuder
   fine ; ruban **Kapton** ; tresse a dessouder ; multimetre ; bracelet ou
   precautions ESD.
2. **Preparation** : retirer les anciens fils et broches du banc si besoin (tresse),
   nettoyer au flux, **pre-etamer** pastilles et bouts de fils.
3. **Longueurs** : fils aussi courts que la disposition le permet (typiquement
   3 a 6 cm), avec une petite boucle de detente ; deux fils de masse n'ont pas lieu
   d'etre (un seul VSS suffit).
4. **Ordre conseille** : condensateurs sur le BM5602 d'abord s'ils sont retenus, puis les six fils cote
   BM5602 (pastilles fragiles : chaleur breve, sans tirer), puis cote C6 dans les
   trous du connecteur exterieur (par le dessus ou le dessous selon la disposition
   retenue). Aucune soudure sur ou pres des antennes.
5. **Controles avant mise sous tension** : continuite de chaque fil de bout en bout,
   **absence de court-circuit 3V3-GND**, absence de pont entre pastilles voisines
   (loupe) ; fils non tendus.
6. **Assemblage** : cale isolante, cartes dans leurs berceaux, fils ranges hors des
   antennes, fermeture sans pincer.
7. **Essais apres assemblage** (console serie USB, 115 200 bauds) :
   - `info` doit afficher `BM5602 : detecte (version puce 0x01000F)`. `ABSENT`
     avec `0xFFFFFF` : MISO (GIO2 -> IO19) coupe ; avec `0x000000` : alimentation,
     CSN ou SCK/MOSI inverses ;
   - `lampe autotest` doit repondre `ok` ; `lampe regs` : `RFCH 05 DM1 82 RT1 73` ;
   - `matter` : toujours `mise en service : faite` et `Thread : role child` (la
     soudure n'efface pas l'appairage) ;
   - essai reel : une commande depuis Apple Home, puis `lampe stats` : noter le
     rapport paquets / accuses et comparer a la reference ci-dessous.

Reference de performance mesuree le 23/09 (montage de banc, fils Dupont) : sans
Thread, 0 paquet perdu sur plusieurs centaines ; avec Thread actif, de 0 a 18 %
de paquets sans accuse selon les phases, toujours rattrapes par les renvois. Un
bon boitier ne doit pas faire pire ; si c'est le cas, c'est la disposition des
antennes qu'il faut revoir.

**Controle obligatoire avant/apres boitier (demande de Majid, 23/09).** Dans le
montage de reference, le BM5602 pend a **~12-15 cm au-dessus du C6** sur des fils
Dupont, antenne vers le haut : un ecartement BIEN superieur aux 25-30 mm vises
ici. Le boitier compact va donc rapprocher les antennes. Protocole :
1. AVANT demontage, montage actuel : `lampe stats raz`, puis ~20 commandes depuis
   Apple Home et ~1 min de telecommande, puis `lampe stats` ; noter paquets,
   accuses, MAX_RT, trames recues et CRC faux.
2. APRES assemblage dans le boitier : meme protocole, memes conditions (meme
   emplacement de la lampe, telephone au meme endroit).
3. Si le taux de paquets sans accuse ou de CRC faux se degrade nettement :
   **allonger le boitier** (plus d'ecart entre antennes) plutot que de compacter.
   Le levier puissance est deja epuise : le BM5602 emet a +6 dBm, son maximum
   (RFTXP_1 = 0xAF, RFTXP_2 = 0x21, table 1 de la note Holtek AN0560).

## 7. Ce que tu dois demander a Majid avant de modeliser

Au pied a coulisse (en mm, au dixieme) -- **carte debranchee, et de preference
avec un pied a coulisse en plastique** : le 24/09, la pointe d'un pied a coulisse
metallique restee aimantee sur le quartz du BM5602 a mis la radio dans un etat
anormal (plus aucune emission terminee, bruit en reception) pendant 70 minutes,
jusqu'a une reinitialisation complete du module :
- SuperMini : longueur, largeur, epaisseur du PCB, hauteur max des composants
  dessus et dessous, position et dimensions de l'USB-C, position des boutons BOOT
  et RESET et des LED, position de la zone d'antenne ;
- BM5602-60-1 : longueur, largeur, epaisseur, position et longueur de la zone
  d'antenne, position de la rangee de pastilles ;
- le bureau : materiau du plateau (bois, verre, metal ?), epaisseur, presence d'un
  cadre metallique, emplacement vise, type de vis souhaite ;
- une photo de dessus de chaque carte a plat, avec une regle.

## 8. Livrables attendus

1. Modele **parametrique** (OpenSCAD, ou CadQuery/build123d) avec toutes les cotes
   mesurees en tete de fichier et commentees.
2. Fichiers **STL/3MF** du fond et du couvercle, plus un rendu eclate de
   l'assemblage montrant les deux antennes et leur distance.
3. La **notice de soudure et d'assemblage** (section 6), en francais.
4. La liste de materiel (fil, vis, condensateurs optionnels) avec references.

Ne touche pas au firmware ni a `platformio.ini`. Le cablage de reference du banc est
decrit dans `docs/WIRING.md` ; en cas de desaccord, **ce brief fait foi pour la
version finale** (six fils, broches de la section 3).
