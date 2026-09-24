# Protocole JSON du pont Halo (v1)

Specification du protocole machine entre le firmware (ESP32-C6, env produit
`esp32c6thread`, env de banc `esp32c6diag`) et l'application de supervision :
d'abord l'app macOS native SwiftUI (`apps/macos/`, liaison USB), plus tard
l'app iOS par le reseau (Thread -> routeur de bordure Apple -> LAN). Le meme
protocole sert aux deux : seuls le transport et l'authentification changent.

Etat : SPECIFICATION, 24/09/2026. Rien n'est implemente. Premiere version du
firmware qui l'implementera : a fixer (0.4.0 propose, `FW_VERSION` vaut
aujourd'hui 0.3.0). Les champs dont la source est privee ou n'existe pas encore
sont marques **(a ajouter)** : il faudra un accesseur ou une mesure nouvelle.
Chaque champ cite sa source dans le code (`fichier : symbole`), pour que
l'implementation n'invente rien.

## 0. Decisions en bref

| Sujet | Decision |
|---|---|
| Tramage | Une ligne machine = octet RS (0x1E) + un objet JSON compact en ASCII + LF. Au plus 1024 octets, RS et LF compris (budget de pire cas : 896). Tout le reste du flux est du texte humain ou des logs, affiches tels quels par la console. |
| Sens app -> carte | Lignes de texte de la CLI existante, prefixees `id=<n> ` pour la correlation. Pas de JSON vers la carte. |
| Session | `json 1` (mode machine, avec bail de 30 s renouvele par `json ping`), `json 0`, `json etat`. Rien n'est persiste : chaque demarrage repart en mode humain. |
| Reponses | Toute ligne portant un `id` recoit un message `reponse` (etape `fin`, precedee d'une etape `debut` pour les commandes historiques). Les commandes d'etat de la lampe portant un `id` deviennent asynchrones : `reponse` tout de suite, `livraison` ensuite. |
| Etat periodique | `etat` (1 Hz), `compteurs` (1 Hz), `reseau` (0,2 Hz), chacun en plusieurs blocs d'une ligne. Les evenements (`rx`, `tx`, ...) sont des indices ; la verite est l'instantane periodique. |
| Compatibilite | Version majeure `v` dans chaque ligne ; ajouts sans changer `v` ; l'app ignore champs, types et valeurs inconnus. |
| Reseau (plus tard) | Memes messages en datagrammes UDP sur l'IPv6 Thread, service DNS-SD `_halo-pont._udp`, cle partagee + HMAC-SHA256 ; liste blanche de commandes a distance. |

## 1. Vocabulaire et principes

- **consigne** : l'etat voulu par Matter, la CLI ou l'app (`Halo1Lamp::target()`).
- **etat cru** : ce que le pilote croit de la lampe (`Halo1Lamp::believed()`).
- **carte** : l'ESP32-C6 et son firmware ; **app** : le client (macOS, iOS).
- **ligne machine** : une ligne RS + JSON (section 2) ; **texte** : toute autre ligne.
- **transport** : le port serie USB (v1), UDP sur Thread (section 10).

Principes :

1. **La carte n'attend jamais l'app.** Une ligne machine qui ne tient pas dans le
   tampon d'emission est perdue et comptee, comme les traces du pilote
   (`Halo1Lamp::trace`, `Halo1Lamp::notice` : `Serial.availableForWrite()`).
2. **Un seul producteur.** Toutes les lignes machine sont formatees et ecrites
   dans la tache loop (celle de `loop()`, `tick()` et de la CLI). Les rappels
   de la tache CHIP et de la tache des evenements IDF ne font que poser des
   compteurs, comme aujourd'hui (`matter_bridge.cpp : post`, `sSubMux`) ; la
   tache loop les traduit en messages.
3. **L'etat est periodique, les evenements sont des indices.** Une ligne perdue
   ou abimee ne fausse rien durablement : l'instantane suivant corrige. L'app
   ne reconstruit jamais un etat en cumulant des evenements.
4. **Rien de nouveau ne part vers la lampe** a cause du protocole : l'app passe
   par les memes consignes que Matter et la CLI (`Halo1Lamp::request`,
   `resolveMatter`).

## 2. Tramage sur le port serie partage

### 2.1 Ce qui circule sur le port

Le port USB CDC du C6 (USB Serial/JTAG, `HWCDC`) porte deja, entremeles :

| Source | Forme | Exemple |
|---|---|---|
| Sorties des commandes de la CLI | texte francais ASCII, lignes CRLF | `  consigne    : allumee deux lum A5 temp 35 (a livrer : -)` |
| Echo des caracteres tapes, invite | caracteres isoles, `> ` sans fin de ligne | `> lampe` |
| Annonces du pilote et du pont | `[lampe] ...`, `[matter] ...` (`Halo1Lamp::notice`, `trace`, `bridgeLog`) | `[lampe] injoignable : consigne abandonnee` |
| Logs ESP-IDF (autres taches) | `E (12345) tag: ...` ; console secondaire USB Serial/JTAG (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`), niveau par defaut ERROR, sans couleurs (`CONFIG_LOG_COLORS` absent) | `E (48213) chip[DL]: ...` (avalees par `quietVprintf` tant que `chiplog` est coupe) |
| Logs du core Arduino | `[    1234][E][fichier.cpp:12] fonction(): ...` | |
| Banniere de demarrage, ROM | `=== BenQ ScreenBar Halo -> Matter ===`, `ESP-ROM:esp32c6-...` | rarement vues : le port se re-enumere au redemarrage |

Deux chemins materiels differents ecrivent dans la meme FIFO USB : le tampon
circulaire de `HWCDC` (4096 octets, `Serial.setTxBufferSize(4096)` dans
`setup()`) et la console secondaire d'IDF, qui ecrit directement dans la FIFO
depuis n'importe quelle tache. **Un log IDF peut donc s'intercaler au milieu
d'une ligne machine.** Le tramage ne l'empeche pas : il le rend detectable.

### 2.2 La ligne machine

```
RS  JSON  LF
0x1E {"v":1,"t":"etat","n":42,"ms":61234,...} 0x0A
```

Regles (toutes obligatoires pour la carte) :

1. Elle commence par l'octet **RS (0x1E)**, suivi immediatement de `{"v":`.
   RS n'apparait dans aucune autre sortie de la carte, d'IDF, du core ou de la
   ROM (les couleurs ANSI utilisent ESC, 0x1B). L'idee vient des sequences de
   textes JSON (RFC 7464), sans en reprendre l'exclusivite : ici, du texte
   circule entre les lignes machine.
2. Un seul objet JSON, **compact** (aucun espace hors des chaines), sur une
   seule ligne, termine par **LF (0x0A)** seul. Jamais de CR dans la ligne
   (le texte humain de la CLI finit en CRLF : l'app tolere un CR final).
3. **ASCII imprimable seulement** (0x20 a 0x7E) entre RS et LF. Dans les
   chaines, `"` et `\` sont echappes (`\"`, `\\`) ; tout octet hors de
   0x20..0x7E est remplace par `?`. Aucune sequence `\uXXXX` n'est emise.
4. **Longueur maximale : 1024 octets, RS et LF compris.** Budget de pire cas
   en v1 (tous les compteurs a 4294967295, toutes les chaines a leur taille
   maximale) : **896 octets** par message, pour laisser 128 octets aux ajouts
   de 9.1 ; un message qui approcherait le budget gagne un bloc (`bloc`),
   jamais de longueur. Plus de 1024 octets reste un bogue : non emis, compte
   (`json_trop_longs`, voir `etat.sante.sys`).
5. Les quatre premiers champs sont toujours `v`, `t`, `n`, `ms`, dans cet
   ordre, puis `bloc` pour les messages en blocs (`hello`, `etat`,
   `compteurs`, `reseau`). L'app ne doit pas en
   dependre (voir 2.4), mais cela aide a lire une capture.

Pourquoi RS plutot qu'un prefixe imprimable ou `{"v":1` en debut de ligne :
une ligne machine peut arriver apres un reste de texte sans fin de ligne (la
fin d'un log coupe, ou l'invite `> ` d'une session humaine) ; avec RS, l'app la
retrouve n'importe ou dans la ligne. RS est invisible dans `pio device monitor`
et dans un terminal : la ligne y reste lisible.

### 2.3 Emission cote carte

- Formatage dans un tampon statique de 1024 octets, puis **un seul appel**
  `Serial.write(buf, len)` : `HWCDC::write` tient son verrou (`tx_lock`) pour
  tout l'appel, donc aucune autre ecriture Arduino ne s'intercale.
- Avant d'ecrire : `Serial.availableForWrite() >= len`, sinon la ligne est
  perdue et comptee (`json_perdus`). Jamais d'ecriture partielle, jamais
  d'attente : avec `setTxTimeoutMs(1000)`, un `HWCDC::write` sur un hote qui ne
  lit plus peut bloquer jusqu'a 20 x 1000 ms (core 3.3.12, `max_consec_timeouts`).
- Lignes periodiques : au plus **une par tour de `loop()`**, et seulement s'il
  reste ensuite au moins 1024 octets libres (la place d'un evenement). Sinon
  reportee au tour suivant ; perdue (comptee) apres 500 ms de retard. Les
  instantanes complets (`json 1`, `json etat`, `json hello`) passent par cette
  file : ~4,6 Ko au pire pour `etat`, `compteurs` et `reseau`, ~6,3 Ko avec
  `hello` et `config`, plus que les 4096 octets du tampon de `HWCDC` ; ecrits
  d'un seul trait, des lignes seraient perdues a chaque `json 1`.
- Evenements : ecrits tout de suite s'ils tiennent, sinon perdus (comptes).
- Le texte que le protocole emet lui-meme (fin de bail, invite apres `json 0`)
  prend le meme chemin non bloquant (3.5, 3.8).
- `n` est incremente pour chaque ligne **produite**, qu'elle soit ecrite ou
  perdue : un trou dans `n` signale une perte, cote carte (`json_perdus`) ou
  sur le fil (ligne abimee, voir 2.4).
- Jamais de formatage ni d'ecriture sous la garde d'antenne
  (`Halo1Radio::sendOne`, verrou OpenThread tenu), sous un verrou OpenThread
  ou de la pile CHIP, ni dans une section critique (`sSubMux`, `sInboxMux` :
  `portENTER_CRITICAL` masque les interruptions sur ce C6 mono-coeur). Les
  evenements `tx` se forment apres le verdict ; `sSubs`, `sResume`, `sRoles`
  sont copies sous `sSubMux` puis formates (comme `tracePoll`). Ordre des
  verrous du pont (`matter_bridge.cpp`) : jamais celui de la pile pris en
  tenant celui d'OpenThread (la tache CHIP prend OpenThread en tenant le sien).
  Un seul tampon statique : aucun formatage ne doit en appeler un autre.

### 2.4 Reception cote app

Algorithme (sur des octets bruts : aucun decodage de caracteres avant d'avoir
separe texte et JSON) :

```
tampon += octets lus
si le tampon depasse 2048 octets sans LF : le vider comme texte, compter "debordements"
pour chaque ligne terminee par LF (0x0A) :
    retirer un CR final eventuel
    i = position du DERNIER RS (0x1E) de la ligne
    si i absent :
        si la ligne finit par '}' et suit une ligne machine abimee
           (ou est la premiere lue apres une pause de lecture de l'app : veille
           du Mac, app suspendue) :
            fragment -> compter "fragments", filtre "logs systeme" seulement
        sinon :
            ligne texte -> console (decodage UTF-8 avec remplacement, jamais d'echec)
    sinon :
        avant = ligne[0..i[  -> si non vide apres retrait des espaces : texte
        json  = ligne[i+1..]
        si len(json) > 1022, ou json ne commence pas par '{"v":',
           ou ne finit pas par '}', ou n'est pas un objet JSON valide,
           ou v/t/n absents ou mal types :
            compter "lignes_abimees", ignorer (ne PAS l'afficher comme texte)
        sinon si v n'est pas une version geree : compter "versions_inconnues", ignorer
        sinon : distribuer selon t (type inconnu : ignorer)
```

Le **dernier** RS : si une ligne machine a ete coupee par un log sans que son
LF passe, sa suite peut contenir un RS valide.

Une ligne machine coupee laisse sa suite sur la ligne suivante, sans RS : un
log IDF ecrit directement dans la FIFO entre deux paquets de 64 octets, ou
`flushTXBuffer` qui jette les octets les plus anciens (le debut d'une ligne)
quand l'hote ne lit plus. C'est un fragment, jamais du texte de commande.

Controle de continuite : pour chaque transport, l'app garde le dernier `n`
recu. `n` qui saute : pertes (compter `n_nouveau - n_attendu`). `n` qui
recule : redemarrage de la carte (le confirmer par `boot`, voir 3.6) ou
lignes anciennes (voir 3.1, tampon a l'ouverture).

### 2.5 Ce que l'app classe et ignore

Lignes texte (hors RS) :

| Motif (apres retrait des sequences ANSI `ESC [ ... lettre`) | Classe | Traitement |
|---|---|---|
| `^[EWIDV] \(\d+\) [^:]+: ` | log IDF | console, filtre "logs systeme" ; niveau = premiere lettre |
| `^\[\s*\d+\]\[[EWIDV]\]\[` | log du core Arduino | idem |
| `^\[(lampe\|matter)\] ` | annonce du firmware | console ; en mode `json log 1` elles arrivent en `log` et ne passent plus ici |
| `^ESP-ROM:`, `^rst:0x`, `^boot:0x`, `^=== BenQ ScreenBar Halo` | demarrage | console, et indice de redemarrage (voir 3.6) |
| `^> ?$` | invite de la CLI (mode humain) | ignoree |
| autre | texte de commande | console, rattache a la commande en cours (voir 6.4) |

L'app ne tire **aucune** information d'etat du texte : seulement des lignes
machine. Le texte sert a la console et au journal.

### 2.6 Sens app -> carte

- Lignes ASCII imprimables (0x20..0x7E), terminees par LF ; un CR avant LF est
  ignore par la CLI (`cliPoll` : `if (c == '\r') continue`).
- **127 octets au plus**, prefixe `id=` compris : le tampon de la CLI fait 128
  octets (`cli.cpp : buf[128]`) et tronque aujourd'hui EN SILENCE au-dela.
  En v1 la carte refuse une ligne trop longue (`reponse` code `trop_long`,
  rien n'est execute) **(a ajouter)**.
- Jamais d'octet RS ni de JSON vers la carte.
- Octet 0x15 (Ctrl-U) : vide la ligne en cours de saisie **(a ajouter)**.
  L'app l'envoie a l'ouverture du port (voir 3.3) pour effacer un reste de
  ligne laisse par une session precedente, qui serait sinon execute.
- Le tampon de reception de `HWCDC` fait 256 octets (`setRxBufferSize(256)`
  dans `HWCDC::begin`) : pendant une commande bloquante, deux lignes au plus
  peuvent y attendre. D'ou la regle d'une commande en vol (6.5). Au-dela de
  256 octets en attente, l'ISR de `HWCDC` jette l'excedent sans rien dire
  (`xQueueSendFromISR` refuse) : une ligne amputee de son LF se recollerait a
  la suivante ; le `0x15` en tete de chaque tentative de connexion (3.3)
  efface un tel reste.
- En mode machine, la carte ignore tout octet recu hors 0x20..0x7E, sauf LF,
  CR, 0x15 et retour arriere **(a ajouter)**, pour que
  `Commande inconnue : "%s"` ne renvoie jamais un RS.

## 3. Session

### 3.1 Ouverture du port (macOS, C6 USB natif)

Le C6 expose l'USB Serial/JTAG : VID:PID `303A:1001`. Le debit (115200) est
ignore par ce peripherique ; l'app le regle quand meme (une variante a pont
UART, comme `esp32dev`, en a besoin : voir 9).

1. Ouvrir `/dev/cu.usbmodem*` (jamais `/dev/tty.*`, qui attend DCD) en
   `O_RDWR | O_NOCTTY | O_NONBLOCK`, puis `ioctl(TIOCEXCL)` : acces exclusif,
   `pio device monitor` ne pourra pas s'y attacher en meme temps. App
   macOS sandboxee : droit `com.apple.security.device.serial`.
2. `cfmakeraw`, 8N1, `CLOCAL | CREAD`, **`HUPCL` retire**, 115200.
3. **DTR et RTS** : voir 3.2. Recommande : les deux a 0 dans un seul
   `ioctl(TIOCMSET)` juste apres l'ouverture, et ne plus y toucher.
4. Resynchronisation : jeter tout ce qui precede le premier LF recu.
5. **Lignes anciennes** : quand l'hote ne lisait plus, `HWCDC` a pu garder
   jusqu'a 4 Ko de sortie (et, cable debranche, il remplace les plus
   anciennes : `flushTXBuffer`). Elles arrivent en premier. L'app date tout
   avec `ms` (et `boot`), jamais avec l'heure d'arrivee, et considere comme
   historique ce qui precede la reponse a son `json 1`.
6. Ouvrir le port **ne redemarre pas** la carte si les regles de 3.2 sont
   tenues. En revanche, tout redemarrage de la carte (`reboot`, panique, chien
   de garde, bouton) **re-enumere l'USB** : le noeud `/dev/cu.*` disparait
   (lecture en erreur ou EOF) puis revient, souvent sous le meme nom. L'app
   surveille l'arrivee de `303A:1001` (IOKit, `IOServiceAddMatchingNotification`
   sur `IOSerialBSDClient`) et rouvre, avec un delai de 300 ms puis 1 s, 2 s,
   5 s. Le numero de serie USB du peripherique est l'adresse MAC de la puce :
   il permet de retrouver la meme carte parmi plusieurs (a confirmer au banc).
7. Un flash (`pio run -t upload`) echoue tant que l'app tient le port
   (`TIOCEXCL`) : l'app offre "Liberer le port" (envoie `json 0` et ferme ;
   DTR et RTS sont deja a 0) et ne rouvre pas avant un clic.
8. Carte en mode telechargement (apres un flash rate, ou BOOT tenu) : meme
   VID:PID, mais la ROM ne parle que le protocole d'esptool. Aucun `hello` :
   l'app l'annonce ("aucune reponse : carte en mode telechargement ?").

### 3.2 DTR et RTS : le piege du C6

Le peripherique USB Serial/JTAG interprete DTR et RTS comme esptool les
utilise, sans aucun cablage : d'apres le manuel technique (chapitre USB
Serial/JTAG, table "CDC-ACM Settings with RTS and DTR", a relire dans sa
derniere version) et la sequence `USBJTAGSerialReset` d'esptool :

| RTS | DTR | Effet |
|---|---|---|
| 0 | 0 | aucun (efface l'indicateur de mode telechargement) |
| 0 | 1 | pose l'indicateur de mode telechargement |
| 1 | 0 | **REDEMARRE LA PUCE** (en mode telechargement si l'indicateur est pose) |
| 1 | 1 | aucun |

Consequences pour l'app :

- **Jamais l'etat RTS=1, DTR=0, meme un instant.** Un programme qui change
  DTR et RTS par deux appels separes (proprietes `rts`/`dtr` d'une
  bibliotheque, par exemple ORSSerialPort) peut y passer. Toujours
  `ioctl(fd, TIOCMSET, &bits)` avec les deux bits a la fois.
- A la fermeture, un pilote qui baisse DTR avant RTS (`HUPCL`) passe par
  RTS=1, DTR=0 : c'est le redemarrage "a la fermeture du terminal" rapporte
  sur C3 (esp-idf, issue 13075). D'ou : `HUPCL` retire, et DTR = RTS = 0 des
  l'ouverture (passage direct 1,1 -> 0,0), de sorte que la fermeture ne
  change rien.
- `HWCDC` ne conditionne pas l'emission a DTR (connexion jugee sur les trames
  SOF et la releve de la FIFO : `HWCDC::isCDC_Connected`) : DTR a 0 ne coupe
  pas le flux.
- Le registre `USB_SERIAL_JTAG_CHIP_RST_REG` a un bit `USB_UART_CHIP_RST_DIS`
  qui coupe ce redemarrage cote puce ; il couperait aussi le reset automatique
  d'esptool (flash) et un fil du forum Espressif le dit inoperant sur C6 face a
  DTR. **Non retenu** ; a n'essayer qu'au banc.

A confirmer au banc avant de s'y fier (tests U1 et U2, section 11) : 50
ouvertures-fermetures de l'app et de `pio device monitor`, `boot` inchange et
`up_s` continu.

### 3.3 Sequence de connexion

```
App                                                  Carte
ouvre le port, DTR=RTS=0, jette jusqu'au 1er LF
envoie 0x15 0x0A  ---------------------------------> ligne en cours videe, ligne vide ignoree
envoie "id=1 json 1\n" ----------------------------> mode machine : echo et invite coupes
                  <--------------------------------- hello (base, identite)
                  <--------------------------------- config
                  <--------------------------------- etat (lampe, tranches, sante)
                  <--------------------------------- compteurs (pilote, radio, matter)
                  <--------------------------------- reseau (thread, abonnements)
                  <--------------------------------- reponse id=1 fin ok
... puis etat + compteurs chaque seconde, reseau toutes les 5 s, evenements
envoie "id=k json ping\n" apres 10 s sans autre commande
```

Les lignes qui suivent `json 1` passent par la file des periodiques (2.3), une
par tour de `loop()` ; la `reponse` part apres la derniere.

Sans `hello` 2 s apres `json 1` : renvoyer (3 fois). Puis :
- texte `Commande inconnue : "id=1". Tape 'help'.` recu (l'ancien firmware
  lit `id=1` comme nom de commande ; motif `^Commande inconnue : "id=\d+"`) :
  firmware sans protocole JSON ; l'app reste en console seule et le dit
  ("flasher >= 0.4.0") ;
- rien du tout, ou du texte sans `hello` : mauvais port, mode telechargement,
  ou commande de banc en cours lancee par une session precedente ; l'app
  reste a l'ecoute et renvoie `\x15\n` puis `json 1` toutes les 30 s, pas
  plus souvent : pendant une commande de banc la CLI ne lit plus, chaque envoi
  s'empile dans les 256 octets de reception (2.6), et chaque `json 1` en file
  serait execute a la sortie (instantane complet a chaque fois).

### 3.4 La commande `json`

Nouvelle famille de la CLI **(a ajouter)**, dans `cli.cpp : handleLine`. Tout
est en RAM (voir 3.7). `json` rejoint la liste `radioFree` (`cli.cpp :
kFree`), et le prefixe `id=` est retire AVANT ce test : sinon chaque ligne de
l'app (`id=17 ...`, premier mot `id=17`) et chaque `json ping` (toutes les
10 s) ferait `lamp.settleRadio()` (jusqu'a 200 ms) puis
`lamp.invalidateRadio()`, soit une reconfiguration complete du BM5602 et
`ChipWatch::forget()`, qui efface la serie de delais, la fenetre de 10 s du
deluge et celle de la surdite : la relance automatique L2 serait retardee, ou
empechee pour un deluge qui met plus de 10 s a atteindre son seuil.

| Commande | Effet | Bornes |
|---|---|---|
| `json` | etat de la session, en texte humain | |
| `json 1 [bail <s>]` | passe en mode machine sur ce transport : echo et invite coupes, reglages de session remis aux valeurs par defaut, puis `hello`, `config` et l'instantane complet par la file des periodiques (~6,3 Ko au pire), la `reponse` `fin` apres la derniere ligne. Idempotent : renvoyer `json 1` resynchronise. | bail 0 (aucun) ou 10..600 s, defaut 30 |
| `json 0` | retour au mode humain : message `fin`, puis l'invite `> ` | |
| `json etat` | instantane complet : `etat` (3 blocs), `compteurs` (3 blocs), `reseau` (2 blocs), places dans la file des periodiques (une ligne par tour de `loop()`, 1024 octets libres apres, 2.3) ; la `reponse` `fin` part apres la derniere ligne. Pire cas cumule ~4,6 Ko, plus que le tampon de 4096 octets de `HWCDC` : jamais d'un seul trait. Marche aussi en mode humain (une fois). | |
| `json hello` | `hello` (2 blocs) et `config`, par la meme file. Marche aussi en mode humain. | |
| `json ping` | renouvelle le bail ; la `reponse` porte `bail_s` et `up_s` | |
| `json periode <ms>` | periode des `etat` | 0 (coupe) ou 200..60000, defaut 1000 |
| `json compteurs <ms>` | periode des `compteurs` | 0 ou 200..60000, defaut 1000 |
| `json reseau <ms>` | periode des `reseau` | 0 ou 1000..60000, defaut 5000 |
| `json trames 0\|1` | evenements `rx` et `tx` | defaut 1 |
| `json log 0\|1` | annonces et traces du firmware en messages `log` au lieu de texte | defaut 0 |
| `json cle [nouvelle <64 hexa>\|efface]` | cle du transport reseau (section 10.4), USB seulement | |

Reglages par transport : l'USB et chaque abonne reseau ont les leurs.

### 3.5 Bail et ping

L'app peut disparaitre sans envoyer `json 0` (plantage, veille du Mac, cable
tire, `kill`). Sans garde-fou, la carte continuerait d'emettre pour personne,
et un humain qui ouvrirait ensuite `pio device monitor` recevrait du JSON.

- La carte date le dernier octet recu du transport et la fin de la derniere
  commande executee ; le bail court depuis le plus recent des deux (une
  commande de banc de 60 s ne fait donc pas expirer le bail a sa sortie).
- Bail echu : retour au mode humain, message `fin` (`cause` : `bail`), puis
  la ligne de texte `json : mode machine coupe (hote muet depuis 30 s)` et
  l'invite, ecrites par le meme chemin non bloquant que les lignes machine
  (`availableForWrite()` d'abord, sinon perdues et comptees) : l'hote est
  justement muet, et un `Serial.println` bloquerait `loop()` jusqu'a
  20 x 1000 ms.
- L'app envoie `id=<n> json ping` apres 10 s sans autre commande. Au banc,
  un humain qui tape `json 1 bail 0` garde le mode machine jusqu'a `json 0`
  ou au redemarrage.

### 3.6 Battement, silence, redemarrage

- Les blocs `etat` servent de battement. Si `periode_ms` vaut 0 ou plus de
  2000, la carte emet un `hb` toutes les 2000 ms.
- **Silence** : aucune ligne (machine ou texte) depuis 3 x max(periode, 2 s),
  hors commande en vol (6.5). L'app renvoie `json 1` ; sans reponse sous 5 s,
  elle ferme et rouvre le port.
- **Redemarrage** : `boot` (8 chiffres hexa tires au demarrage, voir
  `hello.boot` **(a ajouter)**) change, ou `up_s` recule. L'app vide ses
  etats derives, garde ses series de courbes (nouveau segment) et renvoie
  `json 1` si le mode machine est retombe (il retombe toujours : 3.7).

### 3.7 Persistance : aucune

Le mode machine et ses reglages ne sont **pas** ecrits en NVS. Au demarrage,
la carte est en mode humain, periodes par defaut. Raisons :
- sur l'USB natif, tout redemarrage re-enumere le port : l'app se reconnecte
  de toute facon, et renvoie `json 1` (idempotent) ;
- un mode persiste inonderait de JSON le `pio device monitor` d'apres un
  flash ou un redemarrage, ou une session de banc ;
- zero ecriture flash de plus (le pilote evite deja d'ecrire pres des
  emissions : `kPersistAfterTxMs`).

### 3.8 Echo et invite

En mode machine, `cliPoll` n'echo plus les caracteres, n'emet plus le saut de
ligne apres Entree ni l'invite `> `, et n'appelle plus `Serial.flush()` apres
la commande **(a ajouter)** : `HWCDC::flush()` attend jusqu'a 1000 ms sans
progres, puis met `connected` a faux et vide tout le tampon d'emission
(`flushTXBuffer(NULL, 0)`), lignes machine comprises, sans que `json_perdus`
le voie. Seul le texte des commandes historiques reste bloquant. L'app
affiche elle-meme la commande envoyee dans la console. `json 0` et
l'expiration du bail reaffichent l'invite (non bloquante). Le retour arriere
reste gere (inutile pour l'app). A verifier au banc (U5) : sans `flush`, pas
d'octets perdus entre deux commandes (le commentaire de `cliPoll` dit que
l'USB CDC du C6 en perdait "si on enchaine trop vite").

## 4. Enveloppe et conventions

Champs communs a toutes les lignes machine :

| Champ | Type | Sens | Source |
|---|---|---|---|
| `v` | entier | version majeure du protocole, 1 | constante (a ajouter) |
| `t` | chaine | type de message (sections 5 et 6) | |
| `n` | entier 0..4294967295 | numero de ligne produite sur ce transport depuis le demarrage (pas remis a 0 par `json 1`) | compteur (a ajouter) |
| `ms` | entier 0..4294967295 | `millis()` a la production de la ligne (repart a 0 apres 49,7 jours) | `millis()` |
| `bloc` | chaine | pour `hello`, `etat`, `compteurs`, `reseau` : partie du message | |

Conventions :

- **Entiers seulement**, jamais de flottant : un gamma de 2,00 s'ecrit
  `gamma_c: 200` (centiemes). Les compteurs sont des `uint32_t` qui
  reviennent a 0 apres 4294967295.
- **Unites dans le nom** : `_ms`, `_s`, `_us`, `_dbm`, `_kbps`, `_c`
  (centiemes). Sans suffixe : un nombre d'evenements.
- **Octets en hexa** : chaines majuscules sans `0x`, 2 chiffres par octet,
  dans l'ordre de l'air : charge `"C5A5"`, trame brute `"0962D2D86B000000"`,
  registre `"2E"`. Identifiants de noeud Matter : `"0x"` + 16 chiffres (ils
  depassent 2^53, limite des nombres JSON surs), comme `nodeText`.
- **Booleens** `true`/`false` ; **`null`** = inconnu ou sans objet ; un champ
  optionnel peut etre absent.
- **Enumerations** : chaines ASCII minuscules sans accents, `_` comme
  separateur. Les roles et etats OpenThread gardent le texte d'OpenThread
  (`"child"`, `"Registered"`).
- **Pas d'horloge murale** : la carte n'a pas l'heure. L'app date a la
  reception d'un `hello` (heure locale <-> `ms`) et en deduit le reste.

Objet **Etat** (consigne ou etat cru), reutilise par plusieurs messages :

| Champ | Type | Plage | Source (`halo1::State`, `halo1_map.h`) |
|---|---|---|---|
| `marche` | booleen | | `State::power` |
| `lampes` | chaine | `avant`, `arriere`, `deux` | `State::lamps & F_LAMPS` (`lampsText`) |
| `lum` | entier | 76..254 (0x4C..0xFE), brut comme sur l'air | `State::bright` |
| `niveau` | entier | 4..254, niveau Matter canonique | `levelFromRaw(bright)` |
| `temp` | entier | 0 (froid)..100 (chaud), brut (0x00..0x64) | `State::temp` |
| `mired` | entier | 153..370 | `miredFromTemp(temp)` |

`niveau` est la valeur canonique ; Apple Home peut afficher un niveau voisin
qui donne la meme luminosite brute (`displayLevel`, affichage stable E.2).

Codes de champs de consigne (`a_livrer`, `confirme`, `champs`) : `marche`
(`FLD_FLAGS` : marche et lampes), `lum` (`FLD_BRIGHT`), `temp` (`FLD_TEMP`).

## 5. Messages periodiques et de session (carte -> app)

Chaque instantane est une suite de lignes d'un meme type, une par `bloc`.
L'app remplace les valeurs d'un (`t`, `bloc`) a chaque reception ; les blocs
d'un meme cycle ne sont pas forcement dans le meme `ms`.

### 5.1 `hello`

Emis apres `json 1`, sur `json hello`, et de nouveau si une valeur change
(jamais en pratique hors `json 1`). Deux blocs : d'un seul tenant, le pire
cas (957 octets) depassait le budget de 896 (2.2).

**Bloc `base`** (pire cas 627 octets) :

| Champ | Type | Sens | Source |
|---|---|---|---|
| `rev` | entier | revision mineure du protocole (0 en v1.0) | constante (a ajouter) |
| `fw` | chaine | version complete du firmware, ex. `0.4.0-1a2b3c4` | `FW_VERSION_FULL` (`fw_version.h`) |
| `fw_desc` | chaine | version du descripteur d'application, celle que Matter publie ; doit egaler `fw` | `esp_app_get_description()->version` |
| `date`, `heure` | chaines | compilation | `esp_app_get_description()->date`, `->time` |
| `env` | chaine | env PlatformIO, ex. `esp32c6thread` | `FW_ENV` (a ajouter : `tools/git_rev.py`, `env["PIOENV"]`) |
| `build` | chaine | `produit` ou `diag` | `DIAG_ONLY` |
| `reseau_build` | chaine | `thread`, `wifi`, `aucun` | `MATTER_NET_THREAD`, `DIAG_ONLY` |
| `puce` | chaine | `esp32c6` | `CONFIG_IDF_TARGET` |
| `idf` | chaine | ex. `v5.5.5` | `esp_get_idf_version()` |
| `arduino` | chaine | ex. `3.3.12` | `ESP_ARDUINO_VERSION_STR` |
| `boot` | chaine 8 hexa | identifiant de ce demarrage | `esp_random()` dans `setup()` avant `matterBridgeBegin()` (donc avant `Matter.begin()`), entre `bootloader_random_enable()` et `bootloader_random_disable()` (a ajouter) : aucune radio n'est encore active, et IDF ne garantit alors qu'un aleatoire pseudo |
| `reset` | chaine | `mise_sous_tension`, `broche`, `logiciel`, `panique`, `chien_int`, `chien_tache`, `chien`, `baisse_tension`, `usb`, `inconnue` | `esp_reset_reason()` (meme table que `resetReasonText`) |
| `reset_n` | entier | valeur brute de `esp_reset_reason()` | idem |
| `up_s` | entier | secondes depuis le demarrage | `esp_timer_get_time() / 1000000` |
| `session` | objet | reglages en vigueur : `transport` (`usb`, `udp`), `periode_ms`, `compteurs_ms`, `reseau_ms`, `bail_s`, `trames`, `log` | session (a ajouter) |
| `limites` | objet | `ligne_max` (1024), `cmd_max` (127) | |

**Bloc `identite`** (pire cas 467 octets) :

| Champ | Type | Sens | Source |
|---|---|---|---|
| `boot` | | comme le bloc `base` | |
| `mac` | chaine 12 hexa | MAC-48 d'usine | `esp_read_mac(mac, ESP_MAC_BASE)` |
| `id.fabricant` | chaine <= 32 | | `MATTER_VENDOR_NAME` |
| `id.produit` | chaine <= 32 | | `MATTER_PRODUCT_NAME` |
| `id.serie` | chaine <= 32 | `HALO1-` + MAC | `sSerial` (`matter_bridge.cpp : applyIdentity`) ; build diag : meme calcul, hors du pont (a ajouter) |
| `id.nom` | chaine <= 32 | NodeLabel | `MATTER_NODE_LABEL` |
| `id.hw` | entier | | `MATTER_HW_VERSION` |
| `id.hw_txt` | chaine <= 64 | | `MATTER_HW_VERSION_STRING` |
| `caps` | tableau de chaines | capacites de ce build (voir ci-dessous) | |

Capacites v1 : `matter` (pont Matter compile), `thread` (Matter sur Thread),
`garde` (garde d'antenne presente : `Halo1Radio::hasAirGuard()`), `ep4`
(`HALO1_EXPOSE_AUTO`), `led` (voyant d'etat : `PIN_RGB_STATUS_LED` ou LED
simple, hors build diag), `lampe_async` (commandes `lampe` asynchrones avec
`id`, 6.2), `trames` (`rx`/`tx`), `log`, `udp` (transport reseau, section 10),
`cle` (gestion de la cle, 10.4). L'app se regle sur `caps`, pas sur la
version du firmware.

### 5.2 `config`

Reglages lents : emis avec `hello`, et apres toute commande qui en change un
(`lampe rafale|ecart|rx|leger|garde|gamma|adresse`, `matter
med|maxint|reprise auto|impulsion`). Pire cas : 696 octets.

| Champ | Type | Source |
|---|---|---|
| `lampe.adresse` | 8 hexa, ordre d'ecriture | `Halo1Lamp::address()` |
| `lampe.air` | 8 hexa, ordre sur l'air | `Halo1Radio::air()` |
| `lampe.canal` | entier (5) | `halo1::kChannel` |
| `lampe.debit_kbps` | entier (125) | `bc5602::DATARATE_125K` |
| `reglages.paquets`, `accuses_min`, `paquets_max` | entiers | `Halo1Lamp::tuning.repeats`, `minAcks`, `maxAttempts` |
| `reglages.ecart_ms`, `reprise_ms`, `reprises` | entiers | `tuning.gapMs`, `retryMs`, `planRetries` |
| `reglages.rearm_ms`, `silence_ms` | entiers | `Halo1Radio::tuning.rearmMs`, `silenceMs` |
| `reglages.rearm_fort`, `leger`, `garde` | booleens ; `garde` : null si `!radio.hasAirGuard()` (diag, Wi-Fi) | `tuning.strongRearm`, `lightSwitch`, `airGuard` |
| `reglages.gamma_c` | entier (200 = 2,00) | `mapGamma()` |
| `seuils.delais_suite` (3), `deluge_trames` (100), `deluge_pct` (90), `fenetre_ms` (10000), `sourd_hors_rx` (1000), `sans_guerison` (3), `ecart_ms` (60000), `repli_ms` (600000) | entiers | `ChipWatch::kTimeoutRun`, `kNoiseMinFrames`, `kNoiseBadPct`, `kNoiseWindowMs`, `kDeafMinRearms`, `kFruitless`, `kGapMs`, `kBackoffMs` |
| `matter.endpoints` | objet `principal`, `avant`, `arriere`, `auto` (absent sans EP4) | `getEndPointId()` des endpoints (a ajouter) |
| `matter.lampes_en` | `lumieres` ou `prises` | `HALO1_SELECTORS_AS_LIGHTS` |
| `matter.mired_min`, `mired_max`, `niveau_plancher` | entiers | `kMiredCold`, `kMiredWarm`, `kMatterLevelFloor` |
| `matter.impulsion_ms` | entier (EP4 seulement) | `matterAutoPulseMs()` |
| `matter.med`, `matter.med_boot` | 0 routeur, 1 MED des l'init, 2 MED apres `Matter.begin()` | `matterMedMode()`, `sMedBoot` (statique : a exposer) |
| `matter.maxint_s` | entier (0 = celui du controleur) | `matterMaxIntervalCap()` |
| `matter.reprise_auto` | booleen | `matterResumeAuto()` |

Les `seuils` servent a tracer les lignes de declenchement sur les courbes.
`matter` vaut `null` dans le build diag.

### 5.3 `etat`

Periode `periode_ms` (1000 par defaut). Trois blocs : `lampe` d'un seul
tenant (pire cas 917 octets) depassait le budget de 896 (2.2), ses tranches
en sortent.

**Bloc `lampe`** (pire cas 623 octets) :

| Champ | Type | Sens | Source |
|---|---|---|---|
| `boot`, `up_s` | | comme `hello` | |
| `consigne` | Etat | etat voulu | `Halo1Lamp::target()` |
| `cru` | Etat | etat cru de la lampe | `Halo1Lamp::believed()` |
| `a_livrer` | tableau de codes | champs de la consigne pas encore livres | `Halo1Lamp::dirty()` |
| `confirme` | tableau de codes | champs confirmes depuis le demarrage | `Halo1Lamp::confirmed()` |
| `version` | entier | +1 a chaque changement de consigne | `Halo1Lamp::version()` |
| `phase` | `repos`, `rafale`, `reprise` | | `Halo1Lamp::phase_` (a ajouter) |
| `reprise_ms` | entier ou null | avant la reprise, en phase `reprise` | `max(0, (int32_t)(retryAt_ - millis()))` (a ajouter) |
| `echecs` | entier 0..`reglages.reprises` (2) | tours rates de la consigne en cours ; le suivant l'abandonne (`giveUp()` le remet a 0) | `failures_` (a ajouter) |
| `lien` | `inconnu`, `ok`, `perdu` | | `Halo1Lamp::link()` |
| `accuse_ms` | entier ou null | age du dernier accuse de la lampe (null : aucun depuis le demarrage) | `lastAckAt_`, `acked_` (a ajouter) |
| `dernier_a` | entier 0..255 | dernier numero d'appui A | `Halo1Lamp::lastAuto()` |
| `a_entendus` | entier | appuis A de la telecommande entendus (jamais remis a zero) | `remoteAutoCount()` |
| `memoire` | lampes | memoire de selection | `memoryLamps()` |
| `livrees` | entier | consignes livrees depuis le demarrage (jamais remis a zero) | `deliveredCount()` |
| `abandons` | entier | consignes abandonnees (idem) | `giveUpCount()` |
| `sauve_attente` | booleen | etat cru pas encore ecrit en NVS | `persistDirty_` (a ajouter) |
| `ecoute` | booleen | ecoute de fond de la telecommande | `listening()` |
| `trace` | booleen | traces du pilote | `tracing()` |

**Bloc `tranches`** (pire cas 422 octets, 119 sans tranche active) :

| Champ | Type | Sens | Source |
|---|---|---|---|
| `boot`, `up_s` | | comme `hello` | |
| `tranches` | tableau de 0 a 4 objets | tranches actives : `tranche` (`lum`, `temp`, `a`, `brut`), `charge` (hexa), `accuses`, `essais`, `paquets` | `slots_` : `Slot.pay`, `acks`, `attempts`, `repeats` (a ajouter) |

**Bloc `sante`** (pire cas 726 octets) :

| Champ | Type | Sens | Source |
|---|---|---|---|
| `boot`, `up_s` | | | |
| `radio.presente` | booleen | BM5602 detecte | `Halo1Radio::present()` |
| `radio.perdue` | booleen | niveau L3 : relance ratee ou sans effet | `Halo1Lamp::lost()` |
| `radio.mode` | `inconnu`, `reset`, `emission`, `ecoute`, `veille` | | `Halo1Radio::mode()` |
| `radio.configuree` | booleen | la puce porte la configuration du pilote | `Halo1Radio::configured()` |
| `radio.quartz`, `radio.calib` | booleens | au dernier `halo.begin()` | `radio.chip()->crystalReady()`, `calibrated()` |
| `surveil.panne` | booleen | EN PANNE | `ChipWatch::failed()` |
| `surveil.defaut` | booleen | critere du rouge fixe de la LED | `Halo1Lamp::moduleFault()` |
| `surveil.symptome` | `delais`, `bruit`, `sourde` ou null | symptome present (`symptom()` ne rend jamais `Verify`) | `ChipWatch::symptom()` |
| `surveil.delais_suite` | entier 0..255 | delais TX de suite (seuil 3) | `timeoutRun()` |
| `surveil.fen_trames`, `fen_crc_faux` | entiers | fenetre d'ecoute en cours (10 s) | `windowFrames()`, `windowBad()` |
| `surveil.hors_rx_10s` | entier | rearmements hors RX sur 10 s glissantes (seuil 1000) | `deafRearms()` |
| `surveil.sans_guerison` | entier | relances de suite sans guerison (EN PANNE a 3) | `unrecovered()` |
| `surveil.attente_ms` | entier | avant la prochaine relance permise | `waitMs(millis())` |
| `surveil.relances` | entier | relances automatiques comptees | `total()` |
| `surveil.derniere` | objet ou null | `cause`, `il_y_a_s` | `history(&e, 1)` |
| `led.motif` | voir `led` (7.9) | motif affiche ; null en diag | `statusLedPoll` : `sFrame.p` (a ajouter) |
| `led.test` | booleen | `led test` en cours ; null en diag | `Logic::testing()` |
| `matter` | objet ou null | `en_service`, `connecte`, `identify` | `matterIsCommissioned()`, `matterIsConnected()`, `matterIdentifying()` ; null en diag |
| `sys.heap`, `heap_min`, `heap_bloc` | octets | tas libre, minimum historique, plus grand bloc | `esp_get_free_heap_size()`, `esp_get_minimum_free_heap_size()`, `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` |
| `sys.pile_boucle` | octets | pile de la tache loop jamais utilisee | `uxTaskGetStackHighWaterMark(NULL)` |
| `sys.boucle_max_ms` | entier | plus long tour de `loop()` depuis le bloc precedent | mesure (a ajouter) |
| `sys.json_perdus`, `json_trop_longs`, `rejets` | entiers | lignes machine perdues (tampon plein), trop longues (bogue), lignes de l'hote refusees (`trop_long`, `cadence`) | compteurs (a ajouter) |

`matterIsConnected()` sur Thread ne prend jamais le verrou OpenThread en
attente (`netPoll` : `otLockTry(0)`, une fois par seconde) : sans risque pour
`tick()`.

### 5.4 `compteurs`

Periode `compteurs_ms` (1000). Trois blocs, compteurs cumulatifs : l'app
calcule les courbes par differences (section 8). `lampe stats raz`
(`Halo1Lamp::clearStats`) remet a zero les blocs `pilote` et `radio`
(`relances.*` compris, `ChipWatch::clearCounts`), ainsi que
`etat.sante.surveil.relances` et `surveil.derniere` ; ni `sans_guerison`,
`panne`, `attente_ms`, ni `tx.total`, `livrees`, `abandons`, `a_entendus`, ni
le bloc `matter`. Le champ `raz` (nombre de `lampe stats raz` depuis le
demarrage, **a ajouter**) signale la coupure.

**Bloc `pilote`** (pire cas 701) - source `Halo1Lamp::stats` sauf mention :

| Champ | Source |
|---|---|
| `raz` | (a ajouter) |
| `tx.consignes`, `paquets`, `accuses`, `ack_trame`, `max_rt`, `delais`, `fifo` | `requests`, `packets`, `acks`, `ackForeign`, `maxRt`, `timeouts`, `fifoRefused` |
| `tx.total` | `Halo1Lamp::txCount()` (jamais remis a zero, brut compris) |
| `tranches.faibles`, `preemptees`, `annulees`, `reprises`, `abandons`, `attentes` | `weakFails`, `preempted`, `cancelled`, `retries`, `giveUps`, `holdoffs` |
| `a.livres`, `a.refuses` | `autoSent`, `autoIgnoredOff` |
| `rx.trames`, `etat`, `a`, `accuses_lampe`, `service`, `favori`, `invalides`, `crc_faux` | `rxFrames`, `rxState`, `rxAuto`, `rxLampAcks`, `rxService`, `rxReserved`, `rxInvalid`, `rxCrcBad` |
| `divers.sauvegardes`, `traces_perdues`, `relances_module` | `persisted`, `traceDropped`, `restarts` |

**Bloc `radio`** (pire cas 523) :

| Champ | Source |
|---|---|
| `raz` | (a ajouter) |
| `radio.configs`, `reconf_silence`, `reconf_tx`, `verif_ratees`, `rearm`, `rearm_hors_rx`, `brutes`, `bascules` | `Halo1Radio::stats` : `fullConfigs`, `silenceReconf`, `txReconf`, `verifyFail`, `rearms`, `rearmsOffRx`, `rxRaw`, `lightSwitches` |
| `garde` (null sans garde) : `active`, `gardes`, `refus`, `attentes`, `plafonnees`, `max_us` | `tuning.airGuard` ; `stats.guarded`, `guardRefused`, `guardWaits`, `guardCapped`, `guardMaxUs` |
| `relances.total`, `verif`, `delais`, `bruit`, `sourde` | `ChipWatch::total()`, `count(Relaunch::Verify / TxTimeout / RxNoise / RxDeaf)` |

**Bloc `matter`** (pire cas 341 ; absent en diag) - `matter_bridge.cpp : sStats`
(statique : a exposer) :

| Champ | Source |
|---|---|
| `fenetres`, `ignorees` | `windows`, `bootIgnored` |
| `a_appuis`, `a_refuses`, `a_entendus`, `a_perdus` (EP4 seulement) | `autoFired`, `autoRefused`, `autoHeard`, `autoLost` |
| `reflets`, `ecritures`, `echecs`, `verrou`, `traces_perdues` | `reflects`, `writes`, `writeFails`, `lockBusy`, `logDropped` |
| `identify` | `sIdentifyCount` |

### 5.5 `reseau`

Periode `reseau_ms` (5000). Build Thread seulement (build Wi-Fi : bloc
`thread` sans l'objet `thread` ; diag : jamais). Deux blocs. Les lectures
OpenThread se font sous `otLockTry(0)` (jamais d'attente) et celles de la pile
sous `TryLockChipStack()` : verrou pris ailleurs, la carte renvoie les
dernieres valeurs lues et `frais_ms` dit leur age.

**Bloc `thread`** (pire cas 685) :

| Champ | Source |
|---|---|
| `frais_ms` | age de la derniere lecture sous verrou (a ajouter) |
| `matter.en_service`, `connecte` | `Matter.isDeviceCommissioned()`, `matterIsConnected()` |
| `matter.reseau` (`thread`, `wifi`), `matter.wifi` | `Matter.getSelectedNetwork()`, `Matter.isWiFiConnected()` |
| `matter.fabriques` | nombre de fabriques (Apple Home, Google...) : `chip::Server::GetInstance().GetFabricTable().FabricCount()` sous verrou (a ajouter, facultatif) |
| `matter.code_manuel`, `matter.qr` | seulement si pas mis en service : `Matter.getManualPairingCode()` et la charge `MT:...` = parametre `data=` de `getOnboardingQRCodeUrl()`, dont `GetQRCodeUrl` encode `:` en `%3A` (decoder les `%XX`) ; valeurs en cache dans la bibliotheque, sans verrou ; sinon null. Jamais sur le transport reseau. |
| `thread.role` | `otThreadGetDeviceRole` -> `otThreadDeviceRoleToString` (`disabled`, `detached`, `child`, `router`, `leader`) |
| `thread.canal`, `thread.mhz` | `otLinkGetChannel` ; `2405 + 5 x (canal - 11)` |
| `thread.pan` | `otLinkGetPanId`, `"0x%04X"` |
| `thread.tx_dbm` | `otPlatRadioGetTransmitPower` |
| `thread.parent_rssi` | `otThreadGetParentAverageRssi` (null si erreur) |
| `thread.mode` | `otThreadGetLinkMode` -> `linkModeText` (`rn` = MED, `rdn` = FTD) |
| `thread.type_boot`, `type_suivant` | `routeur`, `med_init`, `med_tard` : `sMedBoot`, `matterMedMode()` |
| `thread.pret_ms` | premier instant attache + hote SRP enregistre, ou null : `sNet.readyAt` |
| `thread.roles` | changements de role depuis le demarrage : `sRoleChanges` |
| `thread.mle.attaches`, `detache`, `enfant`, `routeur`, `chef`, `parent_change` | `otThreadGetMleCounters` : `mAttachAttempts`, `mDetachedRole`, `mChildRole`, `mRouterRole`, `mLeaderRole`, `mParentChanges` |
| `thread.srp.client`, `hote`, `services`, `enregistres`, `serveur`, `port` | `otSrpClientIsRunning`, `otSrpClientItemStateToString(hote)`, services comptes et enregistres, `otSrpClientGetServerAddress` |

**Bloc `abonnements`** (pire cas 530) :

| Champ | Source (`matter_bridge.cpp`) |
|---|---|
| `abonnements.actifs`, `lectures` | `sCount.subs`, `sCount.reads` (`countPoll`, toutes les 2 s) |
| `abonnements.sauves` | `collectSaved(...)` total sous `TryLockChipStack()` (lecture NVS de chaque abonnement, verrou de la pile tenu) : au plus toutes les 30 s et sur `json etat`, derniere valeur gardee ; null si verrou ou iterateur occupe |
| `abonnements.demandes`, `neufs`, `repris_pont`, `repris_pile`, `termines`, `plafonnes` | `sSubs.requested`, `fresh`, `byBridge`, `byStack`, `terminated`, `capped` |
| `abonnements.plafond_s`, `reprise_auto` | `sMaxIntCap`, `sResumeAuto` |
| `reprise.passages`, `auto`, `sessions`, `ouvertes`, `echecs`, `sans_nouvelles`, `reprises` | `sResume.runs`, `autoRuns`, `opened`, `ok`, `failed`, `lost`, `resumed` |
| `reprise.en_cours` | `resumeInFlight(millis())` |

### 5.6 `hb` et `fin`

`hb` : battement quand les `etat` sont coupes ou lents (3.6). Champs `boot`,
`up_s`, `json_perdus`.

`fin` : dernier message d'une session machine. `cause` : `commande` (`json 0`)
ou `bail`.

## 6. Commandes de l'app (app -> carte)

### 6.1 Choix : texte de la CLI prefixe par `id=<n>`

L'app envoie les commandes de la CLI existante, telles qu'un humain les
taperait, precedees de `id=<n> ` :

```
id=17 lampe niveau 200
```

`n` : entier decimal 1..999999999, croissant par connexion, repart a 1 apres
999999999. Le prefixe est retire avant l'aiguillage (`handleLine`) ; aucune
commande ne commence par `id=`.

Pourquoi pas des commandes JSON :
- **un seul analyseur** cote carte, celui de la CLI, deja teste et documente
  (`lampe help`, README) ; pas de bibliotheque JSON a lier (flash : le firmware
  C6 pese deja 2,41 Mo pour une partition `huge_app` de 3 Mo, `platformio.ini`) ;
- la **console brute** de l'app et les commandes de ses boutons sont le meme
  flux, et chaque commande de l'app se rejoue a la main dans un terminal ;
- les commandes tiennent en 127 octets ; il n'y a rien a echapper ;
- le transport reseau transporte la meme ligne, dans une enveloppe signee.

Ce que l'on perd : le typage des arguments. Il est rendu par les reponses : un
argument refuse donne `code` `usage` (commandes JSON et lampe asynchrones), et
l'app ne construit ses lignes que depuis des valeurs deja bornees.

### 6.2 Semantique d'une ligne avec `id`

| Commande | Deroulement | Messages |
|---|---|---|
| famille `json` | instantanee ; `json 1`, `json etat`, `json hello` passent par la file des periodiques | lignes produites, puis `reponse` (`fin`) apres la derniere |
| `lampe on\|off`, `lampe avant\|arriere on\|off`, `lampe mode ...`, `lampe lum ...`, `lampe niveau ...`, `lampe temp ...`, `lampe mired ...`, `lampe auto`, `lampe sync` | **asynchrone** : memes regles que la commande humaine (`runIntent`, `runState`, `pressAuto`, `reassert`), mais **sans** `waitIdle` ni bilan texte | `reponse` (`fin`) en quelques ms, puis `livraison` portant l'`id` quand le pilote a fini |
| toute autre commande (dite historique) | inchangee : texte humain, parfois bloquante | `reponse` `debut`, le texte, puis `reponse` `fin` |

Sans `id`, rien ne change : texte humain, commandes `lampe` bloquantes avec
bilan (`printBilan`), aucune `reponse`. Le mode machine (`json 1`) ne change
que l'echo, l'invite et les emissions ; c'est l'`id` qui fait la semantique.

Commandes asynchrones, detail (cli_lampe.cpp) :
- `radioReady()` faux : `reponse` `ok:false`, `code` `radio_absente` ou
  `radio_perdue`, rien n'est demande ;
- arguments hors bornes : `code` `usage` (bornes de `cmdLampe` : `lum`
  4C..FE en hexa, `niveau` 1..254, `temp` 0..100 en DECIMAL, `mired`
  153..370) ;
- `lampe auto` lampe eteinte (`pressAuto()` faux) : `code` `refuse` ;
- sinon la consigne est posee ; si le pilote est occupe (`busy()`) : `code`
  `accepte`, `suite` `livraison`, et l'`id` rejoint la liste des id en attente
  (8 au plus, les plus anciens sortent : `ids_perdus`) ; une `livraison`
  (`livree`, `abandon` ou `annulee`) suivra toujours ;
- pas occupe mais des champs restent a livrer (`dirty()` non nul : lampe
  eteinte, la luminosite partira a l'allumage) : `code` `differe`, `suite`
  `aucune` ; aucune `livraison` ne viendra pour cet `id` ;
- ni l'un ni l'autre : `code` `ok`, `suite` `aucune`.

La `livraison` suivante (7.3) porte tous les id en attente : la carte fond les
consignes (une nouvelle preempte une tranche en cours), donc une livraison
couvre toutes les commandes acceptees depuis la precedente.

### 6.3 Message `reponse`

| Champ | Type | Sens |
|---|---|---|
| `id` | entier | celui de la ligne |
| `etape` | `debut` ou `fin` | `debut` : commande historique qui commence (la boucle va peut-etre bloquer) |
| `cmd` | chaine <= 40 | la commande recue, sans le prefixe, tronquee |
| `ok` | booleen | |
| `code` | chaine | voir ci-dessous |
| `msg` | chaine <= 120, facultatif | explication en francais |
| `duree_ms` | entier (`fin`) | duree d'execution |
| `suite` | `livraison` ou `aucune` (lampe asynchrone) | |
| `consigne`, `a_livrer`, `version` | (lampe asynchrone) | consigne apres la commande |
| `bail_s`, `up_s` | (`json ping`, `json 1`) | |

Codes :

| Code | ok | Sens |
|---|---|---|
| `ok` | oui | executee |
| `accepte` | oui | lampe asynchrone : une `livraison` suivra |
| `differe` | oui | lampe asynchrone : rien a emettre maintenant |
| `en_cours` | oui | etape `debut` |
| `execute` | oui | commande historique terminee ; son resultat est dans le texte (la carte ne sait pas si elle a affiche un `Usage`) |
| `usage` | non | arguments invalides (commandes JSON et lampe asynchrones) |
| `refuse` | non | refusee par une regle (`msg` dit laquelle) |
| `radio_absente`, `radio_perdue` | non | BM5602 absent ou perdu (L3) |
| `inconnue` | non | commande inconnue (branche finale de `handleLine`) |
| `trop_long` | non | ligne de plus de 127 octets : rien d'execute |
| `cadence` | non | plus de 20 lignes par seconde sur ce transport : rien d'execute |
| `interdite` | non | interdite sur ce transport (10.5) |

### 6.4 Commandes utilisees par l'app

| Ecran | Action | Ligne envoyee | Suite attendue |
|---|---|---|---|
| tous | connexion | `json 1`, puis `json ping` | 3.3, 3.5 |
| tableau de bord | rafraichir | `json etat` | instantane complet |
| tableau de bord | tester la LED | `led test` / `led stop` | `led` (evenements), texte |
| commandes | marche / arret | `lampe on` / `lampe off` | `reponse`, `livraison` |
| commandes | lampe avant / arriere | `lampe avant on`, `lampe arriere off` | idem (regles Matter, memoire de selection) |
| commandes | mode | `lampe mode avant\|arriere\|deux` | idem (allume aussi) |
| commandes | luminosite (curseur Matter) | `lampe niveau <4..254>` (plancher `kMatterLevelFloor` ; la CLI accepte 1..254, mais 1..3 donnent 4C et sont rapportes 4) | idem |
| commandes | luminosite brute | `lampe lum <4C..FE>` (hexa) | idem |
| commandes | temperature (curseur) | `lampe mired <153..370>` | idem |
| commandes | temperature brute | `lampe temp <0..100>` (decimal) | idem |
| commandes | bouton A | `lampe auto` | idem, ou `refuse` lampe eteinte |
| commandes | resynchroniser | `lampe sync` | idem (tout est renvoye) |
| trames | pause / reprise du flux | `json trames 0\|1` | |
| trames | ecoute de fond | `lampe ecoute 0\|1` (banc) | etat visible dans `etat.lampe.ecoute` |
| courbes | remise a zero | `lampe stats raz` (confirmation) | `compteurs` avec `raz` + 1 |
| console | tout le reste | ligne tapee, prefixee d'un `id` | `reponse` `debut`, texte, `reponse` `fin` |

Curseurs : l'app envoie au plus une commande toutes les 150 ms pendant le
glissement, et toujours la valeur finale au relachement. La carte fond les
consignes (la valeur finale obtient toujours sa rafale complete :
`Halo1Lamp::setSlot`) ; inutile d'aller plus vite que la telecommande (~9
trames par seconde a la molette).

**Console brute.** Chaque ligne tapee part avec un `id`, pour que l'app sache
ou finit la sortie : le texte recu entre `reponse debut` et `reponse fin` de
cet `id` lui est rattache (au mieux : un log IDF peut s'y glisser). Les
commandes `lampe` d'etat tapees dans la console deviennent donc asynchrones
(6.2) ; l'app rend `reponse` et `livraison` en texte lisible dans la console.
Confirmation demandee par l'app avant d'envoyer : `reboot`, `decommission`,
`erase`, `wifi`, `addr`, `chan`, `xo`, `debit`, `amble`, `aw`, `holtek`,
`regcfg`, `lampe oublie`, `lampe adresse <x>`, `lampe stats raz`, `matter
med|maxint|reprise auto`, `json cle nouvelle|efface`. Apres `reboot` ou
`decommission`, l'app attend la re-enumeration (3.1).

### 6.5 Cadence et commandes bloquantes

- **Une commande en vol a la fois** : l'app attend la `reponse` `fin` d'une
  ligne avant d'envoyer la suivante (file d'attente cote app), `json ping`
  compris.
- Sans `reponse` sous 3 s (et sans `debut`) : la commande est marquee "sans
  reponse", l'app demande `json etat` et passe a la suivante ; **pas de
  reemission automatique** (`lampe auto` n'est pas idempotent : un appui de
  plus change le mode).
- Apres `reponse debut` : pas de verdict de silence (3.6) jusqu'a la `fin`,
  sans liste codee en dur. La plus longue commande actuelle est `txack`
  (200 x 5000 ms, hors canal 5, ~17 min) ; `ecoute`, `prxack`, `sniffspi`,
  `etalon tx`, `ccasync` et `lampe attends` vont jusqu'a 600 s. Au-dela de
  20 min, l'app propose de fermer le port (aucune commande de banc ne lit
  `Serial` : rien ne l'interrompt). Pendant ce temps la boucle est bloquee
  dans la commande : ni `etat`, ni battement, ni Matter (`matterBridgePoll`),
  ni LED (`statusLedPoll`) ; la CLI ne lit plus. L'app affiche "commande de
  banc en cours".
- Commandes historiques bloquantes : la liste suit `cli.cpp`, l'app n'en
  garde aucune et se fie a `reponse debut`. Toute commande `lampe` d'etat
  SANS `id` bloque jusqu'a 6 s (`kWaitMs`), `lampe brut` jusqu'a ~26 s,
  `lampe rampe` selon ses arguments.
- Cote carte : au plus 20 lignes par seconde et par transport ; au-dela,
  `reponse` `cadence` sans execution **(a ajouter)**.

## 7. Evenements (carte -> app)

Emis au moment ou le firmware les constate, dans la tache loop. Ce sont des
indices : les totaux exacts sont dans `compteurs`. Plafonds de debit par type
(au-dela, l'evenement n'est pas produit, ne consomme pas de `n`, et le
suivant du meme type porte `sautes`, le nombre omis) :

| Type | Plafond | Conditions |
|---|---|---|
| `rx` | 50 par seconde, dont 10 `crc_faux` | `json trames 1` |
| `tx` | 50 par seconde | `json trames 1` |
| `log` | 20 par seconde | `json log 1` |
| autres | aucun (rares par nature) | toujours |

### 7.1 `rx` : trame entendue

Source : `Halo1Lamp::onAir()` (chaque trame lue en ecoute passive, CRC faux
compris), et le chemin `AckForeign` de `Halo1Lamp::onVerdict()` (trame recue a
la place d'un accuse, jamais observee). Pire cas 263 octets.

| Champ | Type | Sens | Source |
|---|---|---|---|
| `source` | `ecoute` ou `accuse` | ecoute passive, ou trame dans une fenetre d'accuse | chemin d'appel |
| `brut` | 16 hexa ou null | les 8 octets lus apres l'adresse (null pour `accuse`) | `raw` de `Halo1Radio::pollRx`, local a `Halo1Lamp::tick()` : a passer a `onAir` (a ajouter) |
| `len`, `pid`, `no_ack` | entiers | champ de controle (PCF 9 bits) ; `accuse` : `len` vaut toujours 2 (seul cas transmis : `TxReport.fLen == 2`, `Halo1Lamp::onVerdict`), `pid` et `no_ack` null | `AirFrame.len`, `pid`, `noAck` (`decodeAir`) |
| `charge` | hexa, 0 a 4 octets | charge (`""` : accuse vide) | `AirFrame.pay[0..len-1]` |
| `crc`, `crc_ok` | 4 hexa, booleen | CRC lu, et juste ou non (`accuse` : null et true, CRC verifie par la puce) | `AirFrame.crc`, `crcOk` |
| `type` | `lum`, `temp`, `a`, `accuse_lampe`, `service`, `favori`, `invalide`, `crc_faux` | classement | `halo1::classify()` : `Bright`, `Temp`, `Auto`, `LampAck`, `Service`, `Reserved`, `Invalid`, `CrcBad` |
| `sens` | objet ou null | `lum`/`temp` : `marche`, `lampes`, et `lum` ou `temp` (valeur de l'air, non bornee) ; `a` : `numero`, `copie` (copie d'un meme appui, `AutoPressFilter::feed` faux) ; sinon null | drapeaux `F_POWER`, `F_LAMPS`, valeur |

Pour `crc_faux`, `len`, `pid`, `charge` sont decodes de bits douteux : a
afficher en gris. Les trames `lum`/`temp` de la telecommande changent l'etat
cru et la consigne (`onRemotePayload`) : le bloc `etat.lampe` suivant le dira.

### 7.2 `tx` : paquet emis

Source : `Halo1Lamp::onVerdict()`, au point de la trace `[lampe] TX ...`,
avant `complete()` (donc avant la `livraison` qu'il declenche). Pire cas 226.

| Champ | Type | Sens | Source |
|---|---|---|---|
| `num` | entier | numero du paquet depuis le demarrage (a partir de 0) | `txCount() - 1` apres `sendPacket` |
| `tranche` | `lum`, `temp`, `a`, `brut` | | `TxLog.slot` (`SLOT_*`) |
| `charge` | 4 hexa | | `TxLog.pay` |
| `essai`, `paquets`, `accuses` | entiers | rang de ce paquet dans la tranche, paquets prevus, accuses obtenus (celui-ci compris) | `Slot.attempts`, `repeats`, `acks` |
| `verdict` | `ack`, `ack_trame`, `max_rt`, `delai`, `fifo` | | `Halo1Radio::Verdict` |
| `us` | entier 0..65535 | de CE=1 a TX_DS ou MAX_RT ; `delai` : 30000 ou un peu plus ; `fifo` : 0 ; sature a 65535 | `TxReport.us` |
| `rt2`, `irq1`, `status` | 2 hexa | registres relus apres le paquet | `TxReport.rt2`, `irq1`, `status` |

`paquets` est le nombre prevu (`repeats`) : faute d'accuses, la tranche va
jusqu'a `reglages.paquets_max` (`maxAttempts`), donc `essai` peut le depasser.
Reperes du banc : accuse ~1600-1720 us, `irq1` `2E`, `rt2` `00` ; MAX_RT
~11470 us, `irq1` `1E`, `rt2` `10`.

### 7.3 `livraison` : fin d'une consigne

Source : un observateur dans la tache loop **(a ajouter)**, juste apres
`lamp.tick()` et apres toute consigne posee (CLI, Matter), avant
`statusLedPoll()`, sur le front `Halo1Lamp::busy()` vrai -> faux. `issue` se
lit sur les compteurs : `deliveredCount()` a change -> `livree` ;
`giveUpCount()` a change -> `abandon` ; ni l'un ni l'autre -> `annulee`
(tranche retiree sans livraison : trame de la telecommande qui fixe elle-meme
la valeur pendant l'attente ou la reprise, ou en rafale a la place d'un accuse
(l'ecoute est coupee en rafale), `setSlot` -> `stats.cancelled` ;
`lampe oublie` ; A abandonne lampe eteinte). Des crochets dans
`complete()`/`giveUp()` ne suffisent pas : ces fins n'y passent pas. Cas
particuliers :
- consigne commencee et finie dans une commande bloquante (commande `lampe`
  humaine, `waitIdle`) : pas de front vu, mais un compteur a change ; la
  `livraison` part quand meme a la sortie de la commande ;
- les deux compteurs ont change (commande bloquante qui enchaine des
  consignes, `lampe rampe`) : `abandon` l'emporte ;
- periode occupee par la seule tranche `brut` (`lampe brut`) : aucune
  `livraison`.

Pire cas 528.

| Champ | Type | Sens | Source |
|---|---|---|---|
| `issue` | `livree`, `abandon` ou `annulee` | | front de `busy()` et compteurs |
| `cause` | `injoignable`, `module` ou absent | abandon apres les reprises (`fail()`), ou module perdu / configuration rejetee (`tick()`, `restartModule()`) | (a ajouter : raison notee par `giveUp`) |
| `derniere` | `lum`, `temp`, `a`, ou null (`abandon`, `annulee`) | tranche dont la fin a clos la consigne (`livree`) | (a ajouter : tranche notee par `complete()` avec `delivered_++`), `kSlotText[id]` en minuscules (`"A"` -> `a`) |
| `version` | entier | | `Halo1Lamp::version()` |
| `consigne`, `cru` | Etat | apres la livraison ou l'abandon (abandon : consigne = cru) | `target()`, `believed()` |
| `a_livrer` | codes | ce qui reste (lampe eteinte : luminosite differee) | `dirty()` |
| `ids`, `ids_perdus` | tableau d'entiers (8 au plus), entier | commandes de l'app couvertes ; `[]` si la consigne venait d'ailleurs (Matter, telecommande, commande humaine) | liste (a ajouter) |
| `attente_ms` | entier ou null | depuis la premiere demande en attente ; null si l'observateur n'a pas vu la periode occupee (commande bloquante) | `pendingSince_` (a ajouter), releve par l'observateur tant que `busy()` est vrai : `endBurst()` et `giveUp()` le remettent a 0 avant le front |
| `livrees`, `abandons` | entiers | totaux (jamais remis a zero) | `deliveredCount()`, `giveUpCount()` |

### 7.4 `relance` : relance du module BM5602

Source : `Halo1Lamp::restartModule()`, apres `restart_()` (`halo.begin()`,
~300 ms, jusqu'a ~0,5 s), pour les relances L2 (`announceRelaunch`) et les
essais L3 (module perdu, toutes les 60 s). Emis une fois le resultat connu.

| Champ | Type | Sens | Source |
|---|---|---|---|
| `cause` | `verif`, `delais`, `bruit`, `sourde`, `l3` | | `halo1::Relaunch` (`None` = essai L3) |
| `rang` | entier ou null | relances de suite sans guerison, celle-ci comprise (null pour `l3`) | `watch_.unrecovered() + 1` avant `relaunched()` |
| `detail` | objet | `delais` : `suite` ; `bruit` : `trames`, `crc_faux`, `ms` ; `sourde` : `hors_rx`, `ms` ; `verif` : `verif_ratees` | `timeoutRun()` (lu AVANT `relaunched()`, qui l'efface), `lastFlood()`, `lastDeaf()`, `radio.stats.verifyFail` |
| `ok` | booleen | `halo.begin()` a reussi | retour de `restart_()` |
| `quartz`, `calib` | booleens ou null | ce qui a ete refait (null si echec) | `radio.chip()->crystalReady()`, `calibrated()` |
| `duree_ms` | entier | duree de la relance (boucle bloquee) | mesure (a ajouter) |
| `total`, `panne` | entier, booleen | apres cette relance | `watch_.total()`, `failed()` |

### 7.5 `module` : etat du module

Transitions annoncees aujourd'hui en texte par `notice()`.

| `etat` | Quand | Champs | Source |
|---|---|---|---|
| `panne` | EN PANNE : 3 relances sans guerison, le symptome revient | `sans_guerison`, `symptome`, `essai_s` (600) | `noteFault()` |
| `retabli` | fin de la panne | | `noteFault()` |
| `perdu` | relance ratee : module muet (L3) | | `restartModule()` (`lost_`) |
| `retrouve` | relance reussie apres une perte | | `restartModule()` |
| `config_rejetee` | configuration toujours rejetee apres relance (L3) | `rfch`, `dm1`, `rt1` (2 hexa, attendus `05`, `82`, `73`) ou null si illisible | `restartModule()` (`stuck_`), `radio.readConfig()` |
| `config_verifiee` | configuration verifiee apres une relance qui avait echoue | | `tick()` (`stuck_` efface) |

### 7.6 `intent` : ordres Matter resolus

Source : `matter_bridge.cpp : applyIntents()`, a la fermeture d'une fenetre de
coalescence (120 ms de calme, 400 ms au plus). Absent en diag.

| Champ | Type | Sens | Source |
|---|---|---|---|
| `recu` | objet | ordres de la fenetre, derniere valeur par champ : `ep1` (bool), `niveau` (0..254), `mireds`, `avant`, `arriere` (bool), `a` (true, EP4) ; seuls les champs presents | `MatterIntents` (`has`, `IN_*`) |
| `fenetre_ms` | entier | du premier ordre a la fermeture | `now - first` |
| `ignore` | `demarrage` ou null | garde-fou des 2 premieres secondes ; `demarrage` : `champs`, `consigne`, `version` et `a` absents (retour anticipe d'`applyIntents`) | `sBootGuard`, `HALO1_BOOT_IGNORE_MS` |
| `champs` | codes | champs demandes au pilote (vide : ordre ecarte, par exemple un niveau ecrit par la pile avec EP1 off) | `Resolution.fields` |
| `consigne`, `version` | Etat, entier | apres `lamp.request()` | `target()`, `version()` |
| `a` | `appui`, `ignore`, `refuse` ou null | sort du bouton A (EP4 seulement) | `r.fireAuto`, `pressAuto()` |

### 7.7 `abonnement` : abonnements Matter (build Thread)

Source : `matter_bridge.cpp : tracePoll()` (tache loop), qui compare les
compteurs poses par la tache CHIP (`sSubs`, `sResume`, sous `sSubMux`). Deux
evenements entre deux passages (50 ms) se fondent en un : le message porte
les details du dernier et les totaux, qui disent combien il y en a eu.

| `quoi` | Champs | Source |
|---|---|---|
| `demande` | `abonne` (`0x...`), `plancher_s`, `max_s` (demande), `applique_s` (apres plafond) | `sSubs.reqPeer`, `reqMin`, `reqMax`, `reqApplied` |
| `etabli` | `origine` (`neuf`, `pont`, `pile`), `min_s`, `max_s` | `sSubs.lastKind`, `lastMin`, `lastMax` |
| `termine` | | `sSubs.terminated` |
| `reprise` | `mode` (`auto`, `manuelle`), `verdict` (`lance`, `rien`, `sans_stockage`, `iterateur_occupe`), `sauves`, `abonnes`, `lances`, `servis`, `en_cours` | `sResume.runKind`, `runVerdict`, `runSaved`, `runPeers`, `runLaunched`, `runServed`, `runBusy` |
| `session` | `abonne`, `ok`, `erreur` (`0x..` ou null), `duree_ms` | `sResume.doneNode`, `doneErr`, `doneMs` |
| `reprise_abonne` | `abonne`, `verdict` (`repris`, `deja_servi`, `rien`, `sans_stockage`, `iterateur_occupe`, `file_pleine`), `repris`, `sans_readhandler`, `rates` | `sResume.peerNode`, `peerVerdict`, `peerResumed`, `peerUnsettled`, `peerFailed` |

Tous portent `totaux` : `demandes`, `etablis`, `termines`, `passages`. Comme
les traces, un passage automatique sans effet identique au precedent n'est
emis qu'une fois (`quiet`).

### 7.8 `thread` : changement de role

Source : historique `sRoles` (`onOtRoleChanged`), relu par `tracePoll()`.
Champs : `de`, `vers` (textes OpenThread), `a_ms` (`RoleChange.ms`), `total`
(`sRoleChanges`). L'historique garde 6 changements : si `total` saute de plus,
des roles intermediaires sont perdus.

### 7.9 `led` : motif du voyant

Source : `statusLedPoll()`, quand le motif choisi (`Logic::frame(now).p`)
change. Champs : `motif`, `avant` (motif precedent), `test`.

| `motif` | `statusled::Pattern` | Voyant |
|---|---|---|
| `identification` | `Identify` | arc-en-ciel |
| `injoignable` | `Unreachable` | rouge, 3 clignements (1200 ms) |
| `panne_radio` | `RadioFault` | rouge fixe |
| `livree` | `Delivered` | eclat vert (150 ms) |
| `non_appaire` | `Unpaired` | bleu clignotant 250/250 ms |
| `hors_reseau` | `Offline` | orange lent 1 s/1 s |
| `operationnel` | `Online` | eteint, lueur blanche toutes les 10 s |

L'app anime son icone d'apres le motif et les constantes de `status_led.h`
(la couleur instantanee n'est pas transmise). Absent en diag.

### 7.10 `log` : annonces et traces du firmware

Seulement avec `json log 1`. Les lignes de `Halo1Lamp::notice()` (`niv`
`notice`), `Halo1Lamp::trace()` (`niv` `trace`) et `bridgeLog()` (`src`
`matter`, `niv` `notice`) partent alors en message `log` **au lieu** du texte.
Champs : `src` (`lampe`, `matter`), `niv`, `txt` (la ligne, 191 caracteres au
plus, ASCII). Les logs IDF et le texte des commandes ne passent jamais par
`log`.

## 8. Courbes : ce que l'app calcule

A partir de deux blocs `compteurs` successifs (differences, par fenetre de
10 s ou 1 min) ; une difference negative, ou `raz` qui change, ouvre un
nouveau segment (pas de valeur aberrante) :

| Courbe | Calcul |
|---|---|
| Taux de perte TX | `(d max_rt + d delais + d fifo) / d paquets` ; en complement `1 - d accuses / d paquets` |
| Consignes abandonnees | `d tranches.abandons` (et marqueurs des `livraison` `abandon`) |
| CRC faux | `d rx.crc_faux` par minute, et `d rx.crc_faux / d rx.trames` ; seuil du deluge : `seuils.deluge_trames` et `deluge_pct` sur `fenetre_ms` |
| Refus en reception | `d radio.rearm_hors_rx` (rearmements ou la puce n'etait pas en RX : symptome de surdite, seuil `sourd_hors_rx` sur 10 s) ; cote emission `d tx.fifo` ; garde Thread `d garde.refus` |
| Relances | `relances.total` en escalier, empile par cause ; marqueurs des `relance` et `module` |
| Sante Matter | `abonnements.actifs`, `thread.parent_rssi`, changements de role |

## 9. Versionnage, compatibilite, debit

### 9.1 Regles

- `v` (majeure) change seulement pour une rupture : champ retire, type ou
  unite change, sens change, tramage change. L'app declare les `v` qu'elle
  gere ; un `hello` d'une autre version : elle le dit et reste en console seule.
- Tout le reste est **additif** et garde `v` : nouveau champ, nouveau type de
  message, nouveau bloc, nouvelle valeur d'enumeration, nouvelle commande,
  nouvelle capacite. `rev` (dans `hello`) augmente a chaque ajout, pour
  l'affichage seulement.
- L'app **ignore** les champs inconnus, les types et blocs inconnus, et range
  une valeur d'enumeration inconnue sous "inconnu" sans echouer. Tout champ
  peut etre `null` ou absent si la doc le dit facultatif ; un champ
  obligatoire absent rend le message invalide (ignore, compte).
- Le firmware ne change jamais le type, l'unite ou le sens d'un champ dans une
  meme `v` : il en ajoute un autre et garde l'ancien au moins une version.
- Decodage Swift : un `Codable` par (`t`, `bloc`) avec des proprietes
  optionnelles, apres un premier decodage de l'enveloppe (`v`, `t`, `n`,
  `ms`, `bloc`) ; enumerations avec un cas `inconnu`.

### 9.2 Debit sur l'USB CDC

Tailles mesurees sur les exemples de la section 12 (octets par ligne, RS et
LF compris) : `etat` 509 (`lampe`) + 97 (`tranches`, vide) + 613 (`sante`),
`compteurs` 462 + 350 + 166, `reseau` 568 + 365, `rx` ~200, `tx` ~175,
`hello` 487 + 316, `config` 657.

| Situation | Debit |
|---|---|
| Au repos, reglages par defaut (etat et compteurs 1 Hz, reseau 0,2 Hz) | ~2,4 Ko/s |
| Molette de la telecommande (~9 trames + ~9 accuses de la lampe par s) | + ~3,6 Ko/s |
| Commande de l'app (un paquet par 100 ms pendant la rafale) | + ~1,8 Ko/s pendant ~0,3 s par trame |
| Tous les plafonds (etat et compteurs a 5 Hz, reseau 1 Hz, rx 50/s, tx 50/s, log 20/s) | ~35 Ko/s ; ~50 Ko/s avec les tailles de pire cas |

L'USB full-speed n'est pas le goulot. Les limites reelles : le tampon de 4 Ko
de `HWCDC` (une demi-seconde du debit de pointe courant, si l'hote tarde) et
le formatage dans la tache loop (a mesurer : quelques dizaines a quelques
centaines de microsecondes par ligne ; `sys.boucle_max_ms` le montre).

Sur une cible a pont UART (`esp32dev`), `setup()` ne regle ni
`setTxBufferSize` ni `setTxTimeoutMs` (seulement sous `ARDUINO_USB_MODE == 1`) :
tampon d'emission nul, et `availableForWrite()` rend la place de la FIFO
materielle (128 octets au plus, `uartAvailableForWrite`). Toute ligne machine
plus longue y serait perdue a chaque fois. Avant d'y annoncer le protocole :
`Serial.setTxBufferSize(4096)` avant `Serial.begin()` pour ces cibles aussi ;
puis, a 115200 bauds (~11,5 Ko/s), garder les defauts et couper `trames` hors
besoin.

## 10. Transport futur : iOS par le reseau

Pas en v1 de l'app macOS ; specifie ici pour que les messages n'aient pas a
changer.

### 10.1 Chemin

iPhone (Wi-Fi) -> LAN -> routeur de bordure Apple (HomePod, Apple TV) ->
Thread -> noeud (MED, recepteur toujours actif). Le noeud a une adresse OMR
(prefixe hors maillage annonce par le routeur de bordure), joignable depuis
le LAN. Aucune dependance a Matter pour ce canal : c'est un service IPv6 de
plus sur le noeud, a cote de Matter (sockets lwIP sur l'interface Thread). A
verifier : la pile CHIP d'esp_matter passe-t-elle deja par lwIP
(`esp_openthread_netif_glue`), ou faut-il creer l'interface ?

### 10.2 Datagrammes UDP

- Un message = un datagramme. Charge : l'enveloppe de securite (10.4)
  suivie du JSON (carte -> app) ou de la ligne de commande (app -> carte),
  sans RS ni LF.
- Taille : 1024 octets de message + 64 d'enveloppe au plus < 1232 octets
  (charge UDP maximale sans fragmentation IPv6 sur un MTU de 1280). La fragmentation
  6LoWPAN reste sous le capot de Thread.
- Port par defaut 52540, le SRV de DNS-SD fait foi.
- Emission sans jamais bloquer la tache loop : `sendto` prend le verrou du
  coeur lwIP (`CONFIG_LWIP_TCPIP_CORE_LOCKING=y`) puis, vers l'interface
  Thread, le second mutex du verrou OpenThread (commentaire d'`otLockTry`,
  `matter_bridge.cpp`), que tiennent tour a tour la tache tcpip, la pile
  OpenThread et la garde d'antenne (jusqu'a ~26 ms par paquet lampe). La
  tache loop pose le datagramme dans une file (2 a 4 places, perdu et compte
  si pleine) ; une petite tache fait `sendto`. Jamais d'emission reseau sous
  un verrou OpenThread ou de la pile (interblocage avec tcpip). Reception en
  `MSG_DONTWAIT`. Un datagramme de ~1 Ko prend ~10 des 65 tampons OpenThread
  (`CONFIG_OPENTHREAD_NUM_MESSAGE_BUFFERS`) partages avec Matter : une seule
  emission en vol.
- Pourquoi UDP et pas TCP : les messages sont deja des unites ; l'etat est
  periodique et les commandes portent un `id` (une perte se rattrape) ; pas
  d'etat de connexion ni de tampons TCP par client dans une RAM deja partagee
  avec Matter. Pertes : la carte garde les 8 dernieres `reponse` par session
  et renvoie la meme a un `id` repete, sans reexecuter.

### 10.3 Decouverte

Service DNS-SD `_halo-pont._udp`, enregistre par le client SRP d'OpenThread
sur l'hote SRP deja enregistre par Matter
(`CONFIG_OPENTHREAD_SRP_CLIENT_MAX_SERVICES=5` dans le sdkconfig du core : de
la place, Matter en utilise 1 ou 2) ; le
routeur de bordure le publie en mDNS sur le LAN. Instance : `Halo-<6 derniers
chiffres de la MAC>`. TXT : `v=1`, `sn=HALO1-...`, `fw=0.4.0-...`.
A verifier : cohabitation avec la gestion SRP de la pile CHIP (elle peut
effacer hote et services, par exemple a la remise a zero).

App iOS : `NWBrowser` sur `_halo-pont._udp` ; `Info.plist` avec
`NSBonjourServices` (`_halo-pont._udp`) et `NSLocalNetworkUsageDescription`.

### 10.4 Authentification

Cle partagee (PSK) de 32 octets, tiree par la carte de son aleatoire
(`esp_fill_random`, radio active : aleatoire materiel) melange a celui de
l'app, gardee en NVS (`halo1/cle`). Elle ne passe **jamais** par le reseau.

- `id=<n> json cle nouvelle <64 hexa>` (USB, mode machine, avec `id`) : l'app
  fournit 32 octets de `SecRandomCopyBytes`, la carte les melange aux siens
  (`cle = HMAC-SHA256(alea_app, esp_fill_random(32))`), ecrit la cle en NVS et
  la rend une seule fois (`cle`, 64 hexa). L'app masque `cle` dans sa console
  et son journal. Ne jamais taper cette commande dans `pio device monitor` :
  `log2file` (`platformio.ini`) enregistre la session dans
  `platformio-device-monitor-*.log`, a la racine, non ignores par
  `.gitignore`. Toutes les sessions reseau tombent.
- `json cle` : empreinte (`empreinte` : 8 premiers hexa de SHA-256(cle)) ;
  `json cle efface` : plus de transport reseau.
- L'app macOS range la cle dans le trousseau, element synchronise
  (iCloud) partage avec l'app iOS (meme groupe d'acces) ; a defaut, un QR code
  affiche par l'app macOS.
- NVS non chiffree : qui a la carte en main a de toute facon tout (CLI USB).

Session (poignee de main, texte ASCII) :

```
app  -> carte : H1 SALUT <kid> <na>
carte -> app  : H1 DEFI <sid> <nc> <mac_defi>
```

`kid` : empreinte de la cle ; `na`, `nc` : 16 octets aleatoires (32 hexa) de
l'app et de la carte ; `sid` : 8 hexa aleatoires ; `mac_defi` =
HMAC-SHA256(PSK, `"H1|DEFI|" kid "|" na "|" nc "|" sid`) tronque a 16 octets
(la carte prouve qu'elle a la cle). Cle de session Ks = HMAC-SHA256(PSK,
`"H1|SESSION|" na "|" nc "|" sid`).

Messages ensuite :

```
H1 <sid> <ctr> <mac> <charge>
```

Champs separes par une espace ; `charge` = tout ce qui suit la quatrieme,
octet pour octet (la ligne de commande, ou le JSON). `ctr` : decimal, par
sens, a partir de 1, strictement croissant (fenetre glissante de 32 contre le
desordre) ; `mac` = 16 premiers octets, en 32 hexa majuscules, de
HMAC-SHA256(Ks, `sens "|" sid "|" ctr "|" charge`), `sens` = `A` (app ->
carte) ou `C` (carte -> app), pour qu'un message ne puisse pas etre renvoye a
son auteur. Toute comparaison de MAC se fait en temps constant. Le premier
message de l'app (en general `id=1 json 1`) prouve qu'elle a la cle. Session
oubliee apres 10 min sans message valide. Une poignee de main n'ouvre qu'une
session provisoire (1 place) ; elle ne devient une des 2 sessions etablies,
en remplacant la plus ancienne, qu'au premier message de l'app au MAC juste :
sinon des `SALUT` du LAN evinceraient l'app. 2 `DEFI` par seconde au plus EN
TOUT (pas par source, pas d'amplification). Adresse et port de l'app : ceux
du dernier message au MAC juste (adresses temporaires de l'iPhone). L'app
applique la meme fenetre de 32 aux `ctr` de la carte.

A verifier (R1) : un prefixe OMR global (delegation DHCPv6-PD) rendrait le
noeud joignable d'Internet. Un filtre sur l'adresse source (ULA fc00::/7
seulement) couperait en revanche un iPhone qui n'a qu'une adresse globale sur
le LAN (selection d'adresse source, RFC 6724) ; le HMAC protege de toute
facon, seul le cout des `DEFI` est expose, d'ou leur limite globale.

Integrite et authenticite, **pas de confidentialite** en v1 : l'etat de la
lampe est lisible sur le LAN. Pour chiffrer plus tard : enveloppe `H2` en
AEAD (AES-CCM, materiel du C6 via mbedTLS). Ecartes : DTLS-PSK (tampons et
poignee de main trop lourds a cote de Matter), un cluster Matter proprietaire
(l'app iOS devrait etre un controleur Matter a part entiere, et un cluster ne
porte pas un flux de trames).

### 10.5 Commandes a distance : liste blanche

Autorisees (toujours avec `id`) :
- `json 1 [bail <10..120>]` (jamais `bail 0` a distance : la carte emettrait
  sur Thread pour un iPhone parti jusqu'a l'oubli de la session, 10 min),
  `json 0`, `json etat`, `json hello`, `json ping`,
  `json periode` (2000 ms au moins), `json compteurs` (0 ou 5000 au moins),
  `json reseau` (0 ou 10000 au moins), `json trames 0|1` (coupe seul apres
  60 s), `json log 0|1` ;
- `lampe on|off`, `lampe avant|arriere on|off`, `lampe mode ...`, `lampe
  lum|niveau|temp|mired ...`, `lampe auto`, `lampe sync` (asynchrones) ;
- `led test|stop`.

Tout le reste : `reponse` `interdite`, rien d'execute. En particulier :
`reboot`, `decommission`, `erase`, `wifi`, `addr`, `chan`, `debit`, `amble`,
`aw`, `xo`, `holtek`, `regcfg`, `calib`, `rfinit`, `regs`, tous les outils de
banc radio (`txack`, `ecoute`, `prxack`, `amont`, `rafale`, `spectre`, `cc*`,
`swd`, `sniffspi`...), `lampe brut|croire|rafale|ecart|rx|
leger|garde|gamma|trace|ecoute|attends|rampe|stats|regs|decode|autotest|
adresse|oublie|sauve`, `matter ...`, `chiplog`, `json cle ...`. Le
flash (il n'y a pas de partition OTA : `huge_app`) reste USB seulement, comme
toute evolution OTA future. Les commandes historiques ecrivent sur `Serial` :
leur texte ne partirait pas sur le reseau ; une console distante demanderait
de les faire ecrire dans un `Print` fourni (hors v1).

`reseau.thread.matter.code_manuel` et `qr` valent toujours null a distance.

### 10.6 Profil distant

`json 1` recu par le reseau regle : `periode_ms` 2000, `compteurs_ms` 0,
`reseau_ms` 30000, `trames` 0, `log` 0. Raison : chaque `etat` (~1,2 Ko en
trois datagrammes) fait une douzaine de trames 802.15.4, emises a quelques centimetres du BM5602. La
garde d'antenne (`Halo1AirGuard`) empeche une trame Thread neuve pendant un
paquet lampe, mais le terrain du 23/09 a montre des MAX_RT en rafale apres
chaque commande Apple Home : mesurer `compteurs.pilote.tx.max_rt` avec et sans
client distant avant d'augmenter les cadences (test R3).

## 11. A implementer et a verifier au banc

Firmware (sans rien changer au comportement humain) :
- `src/json_out.{h,cpp}` : ecrivain (echappement, tampon de 1024, ecriture
  unique, `n`, pertes, plafonds par type, file des periodiques, qui porte
  aussi les instantanes de `json 1`, `json etat`, `json hello`) ;
- `cli.cpp` : `json` dans `kFree`, prefixe `id=` retire avant `radioFree` ;
  `reponse` `debut`/`fin`, famille `json`, echo, invite et `Serial.flush()`
  coupes en mode machine, octets hors 0x20..0x7E ignores en mode machine,
  refus `trop_long` et `cadence`, Ctrl-U, bail (texte de fin non bloquant) ;
- `cli_lampe.cpp` : variantes asynchrones avec `id`, liste des id en attente ;
- `Halo1Lamp` : accesseurs (`phase_`, `retryAt_`, `failures_`, `slots_`,
  `lastAckAt_`/`acked_`, `persistDirty_`, `pendingSince_`), `raw` passe a
  `onAir`, cause d'abandon notee par `giveUp()` et derniere tranche par
  `complete()`, crochets d'evenement (`rx`, `tx`, `relance`, `module`) ;
  compteur `raz` dans `clearStats()` ;
- tache loop : observateur de `livraison` (front de `busy()` et compteurs,
  7.3), avant `statusLedPoll()` ;
- `matter_bridge.cpp` : fonctions qui remplissent `compteurs.matter`,
  `reseau.*`, `config.matter`, et les evenements `intent`, `abonnement`,
  `thread` depuis `applyIntents()` et `tracePoll()` ;
- `status_led.cpp` : motif courant et changements ;
- `setup()` : `boot` (`bootloader_random_enable()`, avant `Matter.begin()`) ;
  `platformio.ini`/`tools/git_rev.py` : `FW_ENV`.

Tests au banc :

| Test | Verification |
|---|---|
| U1 | 50 ouvertures-fermetures du port par l'app (DTR=RTS=0) : `boot` inchange, `up_s` continu |
| U2 | idem avec `pio device monitor` ; et une fermeture volontairement "mauvaise" (DTR baisse seul, RTS haut) : la carte redemarre-t-elle ? `hello.reset` vaut `usb` si la carte a redemarre par DTR/RTS |
| U3 | `reboot` : re-enumeration, reconnexion de l'app en moins de 5 s, `hello` avec `reset` `logiciel` |
| U4 | `chiplog` actif (logs IDF en rafale) : lignes abimees rejetees, trous de `n` comptes, aucune fausse valeur affichee |
| U5 | app suspendue 60 s (hote ne lit plus) : `sys.boucle_max_ms` reste bas, `json_perdus` monte, le bail rend le mode humain sans bloquer ; a la reprise, les fragments sont classes comme tels (2.4) ; et, sans `Serial.flush()` en mode machine, 1000 commandes enchainees sans octet perdu (3.8) |
| U6 | molette de la telecommande 30 s : `rx` sans `sautes` aux reglages par defaut ; debit mesure conforme a 9.2 |
| U7 | `id=1 lampe niveau 200` : `reponse` `accepte` en moins de 20 ms, `livraison` avec `ids:[1]` ~0,3-0,6 s plus tard |
| U8 | ligne de 140 octets : `trop_long`, rien d'execute |
| U9 | `json ping` toutes les 10 s et commandes `lampe` avec `id` pendant 10 min : `compteurs.radio.radio.configs` n'augmente pas a chaque ligne (`json` et `id=` hors `invalidateRadio`, 3.4) |
| U10 | lampe debranchee, `id=3 lampe niveau 200`, puis la molette de la telecommande pendant la reprise (1 s apres le premier tour) : `livraison` `annulee` avec `ids:[3]` (7.3) |
| R1-R3 | (reseau) decouverte `_halo-pont._udp` depuis un iPhone ; rejet d'un message rejoue ou d'une mauvaise cle ; `max_rt` avec et sans client distant |

## 12. Exemples

`<RS>` note l'octet 0x1E ; le LF final est omis. Valeurs coherentes avec le
banc (adresse sur l'air `63 FD F0 4F`, gamma 2 : niveau 180 = lum A5, niveau
200 = lum BA ; temp 53 = 268 mireds).

### 12.1 Connexion

App -> carte (`\x15` : l'octet 0x15, Ctrl-U, suivi de LF) :

```
\x15
id=1 json 1
```

Carte -> app :

```
<RS>{"v":1,"t":"hello","n":0,"ms":83512,"bloc":"base","rev":0,"fw":"0.4.0-1a2b3c4","fw_desc":"0.4.0-1a2b3c4","date":"Sep 24 2026","heure":"14:02:11","env":"esp32c6thread","build":"produit","reseau_build":"thread","puce":"esp32c6","idf":"v5.5.5","arduino":"3.3.12","boot":"3FA2C901","reset":"logiciel","reset_n":3,"up_s":83,"session":{"transport":"usb","periode_ms":1000,"compteurs_ms":1000,"reseau_ms":5000,"bail_s":30,"trames":true,"log":false},"limites":{"ligne_max":1024,"cmd_max":127}}
<RS>{"v":1,"t":"hello","n":1,"ms":83513,"bloc":"identite","boot":"3FA2C901","mac":"F0F5BD012345","id":{"fabricant":"Djoko-CLI","produit":"Pont ScreenBar Halo","serie":"HALO1-F0F5BD012345","nom":"Halo","hw":1,"hw_txt":"ESP32-C6 SuperMini + BM5602"},"caps":["matter","thread","garde","led","lampe_async","trames","log"]}
<RS>{"v":1,"t":"config","n":2,"ms":83514,"lampe":{"adresse":"4FF0FD63","air":"63FDF04F","canal":5,"debit_kbps":125},"reglages":{"paquets":3,"accuses_min":2,"paquets_max":5,"ecart_ms":100,"reprise_ms":1000,"reprises":2,"rearm_ms":100,"silence_ms":500,"rearm_fort":false,"leger":false,"garde":true,"gamma_c":200},"seuils":{"delais_suite":3,"deluge_trames":100,"deluge_pct":90,"fenetre_ms":10000,"sourd_hors_rx":1000,"sans_guerison":3,"ecart_ms":60000,"repli_ms":600000},"matter":{"endpoints":{"principal":1,"avant":2,"arriere":3},"lampes_en":"lumieres","mired_min":153,"mired_max":370,"niveau_plancher":4,"med":1,"med_boot":1,"maxint_s":20,"reprise_auto":true}}
<RS>{"v":1,"t":"etat","n":3,"ms":83515,"bloc":"lampe","boot":"3FA2C901","up_s":83,"consigne":{"marche":true,"lampes":"deux","lum":165,"niveau":180,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":165,"niveau":180,"temp":53,"mired":268},"a_livrer":[],"confirme":["marche","lum","temp"],"version":12,"phase":"repos","reprise_ms":null,"echecs":0,"lien":"ok","accuse_ms":41210,"dernier_a":0,"a_entendus":0,"memoire":"deux","livrees":4,"abandons":0,"sauve_attente":false,"ecoute":true,"trace":false}
<RS>{"v":1,"t":"etat","n":4,"ms":83516,"bloc":"tranches","boot":"3FA2C901","up_s":83,"tranches":[]}
<RS>{"v":1,"t":"etat","n":5,"ms":83517,"bloc":"sante","boot":"3FA2C901","up_s":83,"radio":{"presente":true,"perdue":false,"mode":"ecoute","configuree":true,"quartz":true,"calib":true},"surveil":{"panne":false,"defaut":false,"symptome":null,"delais_suite":0,"fen_trames":0,"fen_crc_faux":0,"hors_rx_10s":0,"sans_guerison":0,"attente_ms":0,"relances":0,"derniere":null},"led":{"motif":"operationnel","test":false},"matter":{"en_service":true,"connecte":true,"identify":false},"sys":{"heap":112640,"heap_min":86016,"heap_bloc":45056,"pile_boucle":4380,"boucle_max_ms":3,"json_perdus":0,"json_trop_longs":0,"rejets":0}}
<RS>{"v":1,"t":"compteurs","n":6,"ms":83518,"bloc":"pilote","raz":0,"tx":{"consignes":6,"paquets":21,"accuses":19,"ack_trame":0,"max_rt":2,"delais":0,"fifo":0,"total":21},"tranches":{"faibles":0,"preemptees":1,"annulees":0,"reprises":0,"abandons":0,"attentes":0},"a":{"livres":0,"refuses":0},"rx":{"trames":148,"etat":36,"a":0,"accuses_lampe":36,"service":76,"favori":0,"invalides":0,"crc_faux":0},"divers":{"sauvegardes":3,"traces_perdues":0,"relances_module":0}}
<RS>{"v":1,"t":"compteurs","n":7,"ms":83519,"bloc":"radio","raz":0,"radio":{"configs":161,"reconf_silence":139,"reconf_tx":2,"verif_ratees":0,"rearm":560,"rearm_hors_rx":3,"brutes":148,"bascules":0},"garde":{"active":true,"gardes":21,"refus":0,"attentes":4,"plafonnees":0,"max_us":2380},"relances":{"total":0,"verif":0,"delais":0,"bruit":0,"sourde":0}}
<RS>{"v":1,"t":"compteurs","n":8,"ms":83520,"bloc":"matter","fenetres":5,"ignorees":1,"reflets":11,"ecritures":14,"echecs":0,"verrou":2,"traces_perdues":0,"identify":0}
<RS>{"v":1,"t":"reseau","n":9,"ms":83521,"bloc":"thread","frais_ms":310,"matter":{"en_service":true,"connecte":true,"reseau":"thread","wifi":false,"fabriques":1,"code_manuel":null,"qr":null},"thread":{"role":"child","canal":25,"mhz":2475,"pan":"0x1A2B","tx_dbm":20,"parent_rssi":-48,"mode":"rn","type_boot":"med_init","type_suivant":"med_init","pret_ms":21870,"roles":2,"mle":{"attaches":1,"detache":1,"enfant":1,"routeur":0,"chef":0,"parent_change":0},"srp":{"client":true,"hote":"Registered","services":1,"enregistres":1,"serveur":"fd8e:1c2a:44b0:1::1","port":53535}}}
<RS>{"v":1,"t":"reseau","n":10,"ms":83522,"bloc":"abonnements","frais_ms":1210,"abonnements":{"actifs":1,"lectures":0,"sauves":1,"demandes":1,"neufs":1,"repris_pont":0,"repris_pile":0,"termines":0,"plafond_s":20,"plafonnes":1,"reprise_auto":true},"reprise":{"passages":1,"auto":1,"sessions":0,"ouvertes":0,"echecs":0,"sans_nouvelles":0,"reprises":0,"en_cours":false}}
<RS>{"v":1,"t":"reponse","n":11,"ms":83523,"id":1,"etape":"fin","cmd":"json 1","ok":true,"code":"ok","duree_ms":12,"bail_s":30,"up_s":83}
```

### 12.2 Commande de luminosite depuis l'app

```
id=2 lampe niveau 200
```

```
<RS>{"v":1,"t":"reponse","n":71,"ms":95002,"id":2,"etape":"fin","cmd":"lampe niveau 200","ok":true,"code":"accepte","duree_ms":1,"suite":"livraison","consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":["lum"],"version":13}
<RS>{"v":1,"t":"tx","n":72,"ms":95004,"num":21,"tranche":"lum","charge":"C5BA","essai":1,"paquets":3,"accuses":1,"verdict":"ack","us":1719,"rt2":"00","irq1":"2E","status":"11"}
<RS>{"v":1,"t":"tx","n":73,"ms":95104,"num":22,"tranche":"lum","charge":"C5BA","essai":2,"paquets":3,"accuses":2,"verdict":"ack","us":1612,"rt2":"00","irq1":"2E","status":"11"}
<RS>{"v":1,"t":"tx","n":75,"ms":95204,"num":23,"tranche":"lum","charge":"C5BA","essai":3,"paquets":3,"accuses":3,"verdict":"ack","us":1617,"rt2":"00","irq1":"2E","status":"11"}
<RS>{"v":1,"t":"livraison","n":76,"ms":95206,"issue":"livree","derniere":"lum","version":13,"consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":[],"ids":[2],"ids_perdus":0,"attente_ms":204,"livrees":5,"abandons":0}
<RS>{"v":1,"t":"led","n":77,"ms":95207,"motif":"livree","avant":"operationnel","test":false}
<RS>{"v":1,"t":"led","n":78,"ms":95357,"motif":"operationnel","avant":"livree","test":false}
```

La tranche finit au troisieme paquet : 3 paquets par trame (`repeats`) et 2
accuses au moins (`minAcks`), `Halo1Lamp::onVerdict`. Ordre garanti : `tx` du
dernier paquet, `livraison`, puis `led` (le voyant lit `deliveredCount()` plus
loin dans le meme tour de `loop()`, dans `statusLedPoll`, apres
l'observateur de livraison). `n` 74 manque ici : c'est un bloc
`etat` periodique, omis de l'exemple.

### 12.3 Trames de la telecommande

```
<RS>{"v":1,"t":"rx","n":105,"ms":101310,"source":"ecoute","brut":"087F8068D3800000","len":2,"pid":0,"no_ack":0,"charge":"FF00","crc":"D1A7","crc_ok":true,"type":"service","sens":null}
<RS>{"v":1,"t":"rx","n":106,"ms":101402,"source":"ecoute","brut":"0962D2D86B000000","len":2,"pid":1,"no_ack":0,"charge":"C5A5","crc":"B0D6","crc_ok":true,"type":"lum","sens":{"marche":true,"lampes":"deux","lum":165}}
<RS>{"v":1,"t":"rx","n":107,"ms":101404,"source":"ecoute","brut":"0126588000000000","len":0,"pid":1,"no_ack":0,"charge":"","crc":"4CB1","crc_ok":true,"type":"accuse_lampe","sens":null}
<RS>{"v":1,"t":"rx","n":109,"ms":101611,"source":"ecoute","brut":"0B619AA284800000","len":2,"pid":3,"no_ack":0,"charge":"C335","crc":"4509","crc_ok":true,"type":"temp","sens":{"marche":true,"lampes":"deux","temp":53}}
<RS>{"v":1,"t":"rx","n":118,"ms":102930,"source":"ecoute","brut":"0A70008705800000","len":2,"pid":2,"no_ack":0,"charge":"E001","crc":"0E0B","crc_ok":true,"type":"a","sens":{"numero":1,"copie":false}}
<RS>{"v":1,"t":"rx","n":121,"ms":103031,"source":"ecoute","brut":"0962D2C86B000000","len":2,"pid":1,"no_ack":0,"charge":"C5A5","crc":"90D6","crc_ok":false,"type":"crc_faux","sens":null,"sautes":0}
```

Les `brut` sont ceux que `encodeAir()` produit pour l'adresse `63 FD F0 4F`
(le dernier : un bit retourne dans le CRC). `FF00` est une trame de service
(reveil) ; `E001` le premier appui sur A (3 copies a ~100 ms : les deux
suivantes auraient `copie` vrai).

### 12.4 Echec, abandon, relance

Lampe debranchee, apres `id=7 lampe lum 80` (consigne lum 128, version 16) :
5 paquets en MAX_RT par tour, 3 tours (reprises a +1 s et +2 s), puis
abandon : la consigne revient a l'etat cru (version 17).

```
<RS>{"v":1,"t":"tx","n":208,"ms":120450,"num":40,"tranche":"lum","charge":"C580","essai":1,"paquets":3,"accuses":0,"verdict":"max_rt","us":11476,"rt2":"10","irq1":"1E","status":"01"}
<RS>{"v":1,"t":"livraison","n":251,"ms":124970,"issue":"abandon","cause":"injoignable","version":17,"consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":[],"ids":[7],"ids_perdus":0,"attente_ms":4520,"livrees":5,"abandons":1}
<RS>{"v":1,"t":"led","n":252,"ms":124971,"motif":"injoignable","avant":"operationnel","test":false}
```

Puce sourde (incident du 24/09) : relance, puis EN PANNE plus tard :

```
<RS>{"v":1,"t":"relance","n":19040,"ms":3605120,"cause":"sourde","rang":1,"detail":{"hors_rx":1204,"ms":2870},"ok":true,"quartz":true,"calib":true,"duree_ms":312,"total":1,"panne":false}
<RS>{"v":1,"t":"module","n":28770,"ms":5410022,"etat":"panne","sans_guerison":3,"symptome":"delais","essai_s":600}
```

### 12.5 Matter

```
<RS>{"v":1,"t":"intent","n":315,"ms":140220,"recu":{"ep1":true,"niveau":127},"fenetre_ms":131,"ignore":null,"champs":["marche","lum"],"consigne":{"marche":true,"lampes":"deux","lum":120,"niveau":127,"temp":53,"mired":268},"version":18,"a":null}
<RS>{"v":1,"t":"thread","n":1170,"ms":300480,"de":"child","vers":"detached","a_ms":300402,"total":3}
<RS>{"v":1,"t":"thread","n":1172,"ms":300620,"de":"detached","vers":"child","a_ms":300571,"total":4}
<RS>{"v":1,"t":"abonnement","n":1301,"ms":324210,"quoi":"termine","totaux":{"demandes":1,"etablis":1,"termines":1,"passages":1}}
<RS>{"v":1,"t":"abonnement","n":1340,"ms":331050,"quoi":"demande","abonne":"0x000000000001B669","plancher_s":0,"max_s":600,"applique_s":20,"totaux":{"demandes":2,"etablis":1,"termines":1,"passages":1}}
<RS>{"v":1,"t":"abonnement","n":1343,"ms":331690,"quoi":"etabli","origine":"neuf","min_s":0,"max_s":20,"totaux":{"demandes":2,"etablis":2,"termines":1,"passages":1}}
```

(Niveau 127 -> lum 0x78 = 120 a gamma 2, rapporte 127.)

### 12.6 Console brute, commande historique

```
id=8 lampe stats
```

```
<RS>{"v":1,"t":"reponse","n":530,"ms":180002,"id":8,"etape":"debut","cmd":"lampe stats","ok":true,"code":"en_cours"}
  emission : 7 consignes, 24 paquets, 22 accuses, 0 ACK+TRAME, 2 MAX_RT, 0 delais, 0 FIFO refusees
  ... (texte de Halo1Lamp::printStats)
<RS>{"v":1,"t":"reponse","n":531,"ms":180009,"id":8,"etape":"fin","cmd":"lampe stats","ok":true,"code":"execute","duree_ms":7}
```

Refus :

```
<RS>{"v":1,"t":"reponse","n":585,"ms":190001,"id":9,"etape":"fin","cmd":"lampe auto","ok":false,"code":"refuse","msg":"lampe eteinte : A n'est pas emis","duree_ms":0}
<RS>{"v":1,"t":"reponse","n":588,"ms":190400,"id":10,"etape":"fin","cmd":"lampe lum 20","ok":false,"code":"usage","msg":"lum : 4C..FE, en hexa","duree_ms":0}
```

### 12.7 Mode `log`, battement, fin de session

Apres `id=11 json log 1` puis `id=12 json periode 0` et `id=13 json compteurs
0` (etat et compteurs coupes : battement `hb` toutes les 2 s), l'app se tait ;
30 s apres sa derniere ligne, le bail rend le mode humain.

```
<RS>{"v":1,"t":"log","n":640,"ms":200100,"src":"lampe","niv":"notice","txt":"[lampe] injoignable : consigne abandonnee"}
<RS>{"v":1,"t":"hb","n":641,"ms":202000,"boot":"3FA2C901","up_s":202,"json_perdus":0}
<RS>{"v":1,"t":"fin","n":662,"ms":231400,"cause":"bail"}
json : mode machine coupe (hote muet depuis 30 s)
>
```
