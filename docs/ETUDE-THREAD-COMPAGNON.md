# Etude de faisabilite : app compagnon par Thread (24/09/2026)

**Etude de faisabilite : joindre le pont Halo par Thread a travers les bornes Apple**

# 0. Resultats de l'etape A (24/09, 19 h 30 - 20 h) : GO

Mac sur l'Ethernet USB seul (Wi-Fi coupe), Tailscale actif.

**A1. Route du Mac**
- Avec une seule interface, toujours "not in table". Le noyau connait `fd77:9e:f4bb::/64` par 6 routeurs (5 bornes Apple + l'Aqara) et en marque un installe (`fe80::42a:d4d9:3614:70d3%en18`), absent de la table. Une seule interface ne suffit donc pas : la marque perimee survit.
- Debrancher puis rebrancher l'adaptateur USB lui-meme (detachement de l'interface, `nd6_purge` vide la liste RTI) : route revenue en quelques secondes, `fd77:9e:f4bb::/64 fe80::42a:d4d9:3614:70d3%en18 UGc`.
- L'auteur de la suppression initiale reste inconnu. Une surveillance filtree de `route -n monitor` tourne (scratchpad `routes.log`) pour relever son pid a la prochaine perte.

**A2. Ping** : 5/5, RTT 21 a 27 ms, hlim 254 (un saut par la borne). `561F9A6463953778.local` se resout bien en `fd77:9e:f4bb:0:6c06:6762:45d6:a3f0`, avec 2 fabrics (`309BEA1CCA0C1569`, `20A842B5C3C38A0D`). La confirmation par coupure du pont reste a faire.

**A3. UDP** : 5/5 REFUS sur le port 40000. Temoin : le port Matter 5540 (ouvert) donne DELAI, comme attendu pour un datagramme invalide. Un port non annonce traverse donc les bornes Apple : **GO pour une v1 sans service SRP** (1re ligne du tableau de decision). Le risque 2 est leve.

**Nouveau : l'OMR bouge deja.** A 19 h 33, l'Aqara annoncait `omr=fd77:9e:f4bb::/64` et une RIO basse pour ce prefixe. A 19 h 55, il annonce `omr=fd0d:eec8:5ef:1::/64` et une RIO moyenne pour celui-ci seulement. Les bornes Apple et tous les noeuds restent sur `fd77`. L'Aqara semble avoir quitte la partition Apple. Si `fd77` etait bien son prefixe, les bornes Apple publieront le leur et l'adresse du pont changera : c'est le cas R5 en conditions reelles, et la surveillance le verra.

**Phase 1 (firmware) ecrite le 24/09 au soir, validee au banc le 25/09 (R1 ; R2 en partie) : docs/PROTOCOLE-JSON.md 10.**
- Relue par 4 agents (concurrence OpenThread, securite H1, non-regression USB, robustesse), puis contre-verifiee par 2 agents ; un bug majeur trouve et corrige (reponses jetees comme perimees).
- Route du Mac : la cause est un bug du noyau de macOS (suppression par le noyau quand un routeur de bordure parait injoignable, jamais remise) ; seule une route statique tient. Suite : un assistant systeme pour l'app macOS (docs/PROTOCOLE-JSON.md 10.1).
- `src/h1_proto.*` (pur, 78 verifications sur l'hote avec des vecteurs Python), `src/h1_crypto.cpp` (mbedTLS), `src/net_udp.*` (socket OpenThread, files RX/TX, cle NVS), `json_mode.cpp` a une session par transport (USB + 2 reseau), liste blanche `jsonp::remoteRefusal`, cache des 8 dernieres reponses par session.
- Ecarts assumes par rapport a cette etude : port **5480** ; une ligne = un datagramme (1078 octets au plus, 6LoWPAN fragmente), pas de decoupage a 512 (a revoir apres R3) ; decouverte par le nom SRP (`reseau` bloc `ip`, `srp.nom`), pas de service `_halo-pont._udp` en v1 ; `json cle nouvelle` exige un `id` mais pas le mode machine.
- Client de banc : `tools/halo_udp.py` (cle par l'USB, session, commandes, test `refus`).
- Reste : phase 2 (app macOS, source "Reseau"), phase 3 (iOS).

# 1. Verdict : GO, si l'experience de la section 2 passe (passee le 24/09, voir section 0)

Rien n'empeche le chemin Mac ou iPhone -> LAN -> borne Thread Apple -> noeud. Le firmware a deja les briques necessaires : UDP OpenThread, client SRP, HMAC avec SHA materiel. Deux points ne sont pas prouves, et un troisieme bloque ce Mac aujourd'hui. Aucun code avant l'experience.

**Etat du Mac releve a 16 h 44 (lecture seule)**
- `route -n get -inet6 fd77:9e:f4bb::1` repond toujours "not in table".
- La liste RTI du noyau garde deux entrees marquees installees :
  - `fe80::cf4:afe0:c89a:397c%en0`
  - `fe80::42a:d4d9:3614:70d3%en18`
- `561F9A6463953778.local` se resout en `fd77:9e:f4bb:0:6c06:6762:45d6:a3f0`. C'est probablement le pont : seul noeud Thread en SII/SAI 2000/2000. A confirmer en A2.

**Risques cles, du plus bloquant au moins bloquant**

1. **Route du Mac (bloquant aujourd'hui, certain).**
   - Le noyau est dans un etat incoherent : 2 routes RTI marquees installees, aucune dans la table.
   - XNU ne la reinstallera pas seul, meme en attendant.
   - Cause probable, non prouvee : Wi-Fi et Ethernet USB branches sur le meme lien.
   - 24/09 soir : avec une seule interface, la marque perimee restait. Le double branchement n'est donc pas la cause directe de l'etat bloque. Le detachement de l'interface (adaptateur USB debranche) le repare.
   - Consequence pour l'app macOS : elle doit detecter EHOSTUNREACH/ENETDOWN et l'expliquer a l'utilisateur (remede : debrancher l'adaptateur, couper puis rallumer le Wi-Fi, ou redemarrer).
2. **Filtrage par port dans la borne : leve le 24/09 (A3 = 5/5 REFUS).**
   - Les Best Practices du Thread Group (section 4.1) autorisent une borne a ne laisser entrer que le trafic que les noeuds ont demande.
   - Toutes les preuves publiques visent des ports enregistres en SRP (Matter 5540, `_hap._udp`).
   - Si la borne filtre, le service SRP maison devient obligatoire.
3. **Republication d'un type DNS-SD tiers par les HomePod et l'Apple TV.** Probable d'apres le code source mDNSResponder-2881, non prouve sur le logiciel reellement livre.
4. **Cohabitation avec le client SRP de CHIP (maitrisable, mais peut casser Matter).**
   - CHIP efface 628 octets sur chaque service retire.
   - Un nom d'instance reste lie a un ancien hote entraine le rejet YXDOMAIN de toute la mise a jour. Matter est alors bloque jusqu'a l'expiration du bail.
   - CHIP efface tout a chaque demarrage.
   - Le service SRP maison reste donc une option, jamais la base.
5. **Radio.** Chaque datagramme part en trames 802.15.4 a quelques cm du BM5602, et le lien parent est faible (parent_rssi -80). MAX_RT a mesurer (R3). Datagrammes de 512 octets au plus.
6. **Prefixe OMR non fige.** Le prefixe actif `fd77:9e:f4bb::/64` est celui de l'Aqara Hub M100 (cle `omr=`), pas d'une borne Apple. Il peut changer. Toujours resoudre un nom, ne jamais figer l'adresse. Confirme le 24/09 a 19 h 55 : l'Aqara annonce desormais `fd0d:eec8:5ef:1::/64`, et les bornes Apple gardent `fd77`.
7. **Flash.** Il reste 143 248 octets libres (4,6 %). La v1 est estimee entre 10 et 20 Ko (non mesure).
8. **Cote Apple.**
   - La confidentialite du reseau local impose une signature Apple Development.
   - Le partage de trousseau avec une equipe gratuite est a verifier.
   - iOS 18 minimum (Synchronization.Mutex).

# 2. Experience decisive la moins chere

## Etape A : sans flasher (environ 15 min, depuis le Mac)

**A1. Remettre la route (a faire par toi)**
1. Couper le Wi-Fi ET debrancher l'Ethernet USB, ou redemarrer le Mac.
2. Rebrancher une seule interface et attendre 3 min (les annonces arrivent toutes les 180 s environ).
3. Lancer `route -n get -inet6 fd77:9e:f4bb::1` puis `python3 /private/tmp/claude-501/rtilist.py`.
   - Attendu : `gateway: fe80::...%enX`, et une seule entree avec `stateflags=0x1`.
4. Si c'est toujours "not in table" :
   - redemarrer le Mac ;
   - en dernier recours, route manuelle provisoire (perdue au redemarrage) : `sudo route -n add -inet6 -prefixlen 64 fd77:9e:f4bb:: fe80::cf4:afe0:c89a:397c%en0`
   - pour l'annuler : `sudo route -n delete -inet6 -prefixlen 64 fd77:9e:f4bb::`
   - La suite du test reste valable, mais c'est alors un defaut macOS a documenter.
5. Apres A3, rebrancher la seconde interface et refaire l'etape 3. Si la route disparait, le double branchement est la cause : l'app l'indiquera comme configuration non prise en charge.

**A2. Ping et identification du pont en une fois**
1. Terminal 1 : `ping6 -i 1 fd77:9e:f4bb:0:6c06:6762:45d6:a3f0`
2. Terminal 2 : `dns-sd -B _matter._tcp local.`
3. Couper l'alimentation du pont 10 s, puis la remettre (pas de commande serie).

Resultats :
- **C'est bien le pont** : les reponses s'arretent a la coupure, ses instances `_matter` (une par fabric) passent en Rmv puis Add, et les reponses reviennent 20 a 30 s apres (pret_ms observe : environ 22 s). Le chemin IPv6 LAN -> Thread fonctionne.
- **Les reponses ne s'arretent pas** : ce n'est pas le pont. Faire `dns-sd -L <instance> _matter._tcp local.` sur les instances passees en Rmv/Add, puis `dns-sd -G v6 <HOTE>.local`.
- **Aucune reponse alors que la route est valide** : essayer d'autres bornes comme passerelle manuelle (`fe80::cb5:8b5f:9a0a:cf65`, `fe80::c62:3ffd:fab3:fed`, `fe80::8be:d542:2b01:7560`). Si aucune ne passe alors qu'Apple Home fonctionne : NO-GO par les bornes, voir la section 4.

**A3. UDP vers un port ferme quelconque (40000, hors de la plage ephemere d'OpenThread)**
- Commande : `python3 /private/tmp/claude-501/halo_udp_test.py refus fd77:9e:f4bb:0:6c06:6762:45d6:a3f0 40000 5` (script verifie en local).
- **5/5 "REFUS"** : lwIP a renvoye un ICMPv6 "port injoignable", donc un port non annonce traverse la borne. GO pour une v1 sans service SRP.
- **"DELAI" alors que A2 repond** : filtrage par port probable. Ce n'est pas concluant seul, car l'ICMP d'erreur peut se perdre. L'etape B tranche.
- **"ERREUR 65"** : la route est perdue, revenir a A1.

## Etape B : sonde firmware de banc

A faire seulement si A3 n'a pas donne "REFUS", ou pour valider le SRP et la radio. C'est toi qui flashes par USB. Taille : 2 a 4 Ko, sous `#if MATTER_NET_THREAD`, en commandes humaines seulement.

**Ce que fait la sonde**
- `udp test <port>` :
  - ouvre un socket otUdp lie a `OT_NETIF_THREAD_INTERNAL`, sur un port fixe inferieur a 49152 (proposition : 5480, a fixer) ;
  - le rappel de reception, qui tourne dans ot_task, ne fait que copier le message dans une file de 2 places ;
  - la tache loop renvoie l'echo sous `otLockTry(0)`.
- `udp etat` affiche :
  - les adresses OMR et ML-EID ;
  - le nom d'hote SRP (`otSrpClientGetHostInfo()->mName`) ;
  - l'etat de notre service ;
  - les compteurs rx, tx, pertes et erreurs, et le minimum de tampons OT libres.
- `udp srp on|off` ajoute ou retire `_halo-pont._udp` :
  - nom d'instance derive du nom d'hote SRP ;
  - service place dans une zone statique alignee de 768 octets ;
  - ajout seulement si l'hote et au moins un service CHIP sont Registered ;
  - si le service reste en attente plus de 60 s : `otSrpClientClearService`.
- `udp stop` ferme le tout.

**B1. Echo sans SRP**
- Sur le pont : `udp test 5480`.
- Sur le Mac : `python3 /private/tmp/claude-501/halo_udp_test.py echo <OMR> 5480 16 256 512 1024 1232` (100 envois par taille, 1 par seconde).
- **Echos recus** : un port quelconque passe (confirme A3). Noter les pertes et le RTT par taille.
- **Rien, et `udp etat` montre rx=0** : la borne filtre, passer a B2.
- **rx augmente mais aucun echo** : probleme d'adresse source au retour (verifier `mSockAddr`).

**B2. Service SRP**
- Sur le pont : `udp srp on`. `json reseau` doit montrer hote Registered et 3 services sur 3 enregistres.
- Sur le Mac : `dns-sd -B _halo-pont._udp local.` puis `dns-sd -L Halo-... _halo-pont._udp local.`
- **Instance visible, port 5480, hote `<HOTE>.local`** : les bornes republient un type tiers, la decouverte DNS-SD est possible.
- **Enregistre (3/3) mais invisible depuis le Mac** : les bornes ne republient pas ce type, passer au repli F1 ou F2 (section 4).
- **Reste en Adding, avec "SRP update error" dans `chiplog`** : le serveur rejette la mise a jour. Faire `udp srp off` tout de suite et verifier qu'Apple Home repond.
- Refaire ensuite B1 avec le service enregistre : si l'echo ne passe qu'avec le service, le service SRP est obligatoire.

**B3. Robustesse**
- Redemarrer le pont : notre service se re-enregistre seul en moins de 60 s, et les services Matter restent intacts.
- `udp srp off` ne provoque pas de plantage.
- Apple Home reste reactif pendant tout le test.

**B4. Radio (test R3)** : echo de 512 octets par seconde pendant 10 min, puis 10 min sans echo. Comparer `compteurs.pilote.tx.max_rt` et les livraisons de la lampe.

**B5. iPhone (facultatif ici)** : build de test avec un `NWConnection` UDP vers `<OMR>:5480`, sur un vrai iPhone (le simulateur ne gere pas l'autorisation reseau local).

## Decision

| Resultat | Decision |
|---|---|
| A1, A2 OK et A3 "REFUS" (ou B1 OK) | GO. v1 avec decouverte par nom d'hote appris en USB ; service SRP en option |
| B1 echoue sans SRP mais passe avec | GO. Service SRP obligatoire, risques 3 et 4 a encadrer |
| B2 invisible et B1 echoue | NO-GO par les bornes Apple, voir section 4 |
| A2 echoue avec une route valide | NO-GO, voir section 4 |
| Route du Mac instable avec deux interfaces | GO pour l'iPhone et pour un Mac a une seule interface ; message dans l'app |
| MAX_RT nettement en hausse en B4 | GO avec un profil distant reduit : periode plus longue, datagrammes plus petits |

# 3. Mise en oeuvre (si GO)

## Phase 0 : documentation

Corriger la section 10 avant d'ecrire du code (details en fin de section).

## Phase 1 : firmware

**1a. Champs USB, sans risque (moins de 1 Ko)**
- Ajouter a `json reseau` et a `matter` : le nom d'hote SRP, les adresses (OMR avec leur origine) et le port UDP.
- Aujourd'hui, seul l'etat SRP est expose (matter_bridge.cpp:1532 et 2230).

**1b. `src/net_udp.{h,cpp}`**
- Socket otUdp sur le port fixe.
- File de reception de 2 places d'environ 560 octets.
- Emission depuis la tache loop sous `otLockTry(0)`, une seule en vol, gestion de `NULL` et `OT_ERROR_NO_BUFS`.
- Compteurs, et `udp` ajoute a `kFree` (cli.cpp:359).
- Dans le rappel de reception : jamais de verrou CHIP, jamais de `Serial`.

**1c. `src/h1.{h,cpp}` : enveloppe H1 (section 10.4)**
- Poignee de main SALUT/DEFI, 1 session provisoire et 2 etablies.
- Fenetre de 32, 2 DEFI par seconde au total, oubli apres 10 min, cache des 8 dernieres reponses.
- HMAC avec `mbedtls_md_hmac` (SHA materiel), comparaison en temps constant ecrite par nous sur 16 octets.
- Cle partagee en NVS `halo1/cle`, commandes `json cle nouvelle|cle|efface` par USB (emplacement deja reserve a json_mode.cpp:916).

**1d. `json_mode` : le plus gros morceau**
- Aujourd'hui, une seule session machine ecrit directement dans `Serial` (json_mode.cpp:98-114, 608, 703).
- Il faut :
  - une sortie par session (USB, et 2 sessions reseau) ;
  - le profil distant (10.6) ;
  - la liste blanche 10.5 appliquee par la carte, qui fait foi (`PolitiqueCommandes` cote app n'est qu'un confort) ;
  - le decoupage des messages en 512 octets au plus.

**1e. Service `_halo-pont._udp` (seulement si B2 passe)**
- Desactivable, avec les gardes de B2 : zone de 768 octets, garde de 60 s, nom d'instance derive du nom d'hote.
- Re-ajout apres chaque `_ClearSrpHost` de CHIP.

**Taille estimee (non mesuree)**
- Flash : 10 a 20 Ko sur les 143 Ko libres.
- RAM : 5 a 7 Ko sur environ 128 Ko de tas libre (minimum mesure : 91 Ko).
- Pas de tache supplementaire avec otUdp.

## Phase 2 : app macOS

**Nouveau framework `HaloReseau` (macOS et iOS)**
- `EnveloppeH1`, avec des vecteurs de test communs au firmware.
- `TransportUDP: Transport`, sur `NWConnection`.
- `DecouverteHalo` : `<HOTE>.local` et port appris par USB ; `NWBrowser` seulement si B2 passe.
- `TrousseauCle`.
- Le squelette compile deja : `/private/tmp/halo-apple/verif/TransportUDP.swift` (283 lignes).

**`Pont`**
- Nouvelle source `Source.reseau`.
- Reconnexion au reveil du Mac, au changement de chemin reseau et apres un silence (`.rouvrir`).
- Messages clairs pour EHOSTUNREACH/ENETDOWN ("pas de route IPv6 vers le reseau Thread") et pour `localNetworkDenied`.

**Projet**
- Droit `com.apple.security.network.client`.
- `NSLocalNetworkUsageDescription`, plus `NSBonjourServices` dans une Info.plist partielle si l'app navigue en DNS-SD.
- Signature Apple Development.
- La navigation ne demarre que quand l'utilisateur choisit la source "Reseau", pour ne pas montrer l'alerte aux utilisateurs USB.

**Taille estimee** : 800 a 1200 lignes Swift avec les tests (non mesure).

## Phase 3 : iOS

- `HaloProtocole` passe en multiplateforme, et `Pont` sort d'AppKit et d'IOKit.
- iOS 18 minimum.
- `json 0` et coupure au passage en arriere-plan, reconnexion au retour.
- Cellulaire exclu.
- Cle par le trousseau iCloud, ou par QR code a defaut.

## Phase 4 : tests (a ajouter en section 11)

- **R1** : decouverte, par nom d'hote puis par `_halo-pont._udp`.
- **R2** : message rejoue, mauvaise cle, mauvais sens et commande hors liste blanche rejetes comme `interdite`.
- **R3** : MAX_RT avec et sans client distant.
- **R4** : debrancher la borne passerelle et chronometrer la reprise.
- **R5** : redemarrage du pont et changement d'OMR, suivis d'une nouvelle resolution.
- **R6** : Mac branche sur deux interfaces.
- **R7** : refus de l'autorisation reseau local, avec message clair.
- **R8** : 1000 commandes (pertes, RTT, reponse rejouee sans reexecution).

## Changements dans docs/PROTOCOLE-JSON.md

- **10.1**
  - Le "A verifier" est resolu : CHIP attache deja l'interface Thread a lwIP.
  - Ajouter que le client doit avoir la route RIO vers l'OMR (meme lien que les bornes), et que l'OMR peut venir d'une borne tierce et changer.
- **10.2**
  - Port fixe inferieur a 49152 : 52540 est dans la plage ephemere d'OpenThread (49152 a 65535).
  - Voie otUdp au lieu de `sendto` et d'une tache dediee : `sendto` attend deux fois le verrou OT sans limite de temps, avec le verrou du coeur lwIP tenu.
  - Datagramme conseille : 512 octets, 1232 au plus. Gerer NO_BUFS.
- **10.3**
  - Retirer l'argument "MAX_SERVICES=5" : ce n'est que la reserve de tampons.
  - Decouverte de base : nom d'hote SRP `<16 hexa>.local` et port, donnes par `json reseau` en USB et gardes par l'app avec la cle.
  - `_halo-pont._udp` devient optionnel, avec ses gardes.
  - `thread.srp.services` passe a 3.
- **10.4**
  - MAC tronque compare par notre propre code des deux cotes : CryptoKit refuse un MAC tronque.
  - Publier un vecteur de test.
- **10.5** : liste blanche appliquee par la carte ; ajouter `udp` aux commandes interdites.
- **10.6** : cadences et tailles a fixer apres R3.
- **Section 5** : nouveaux champs `thread.srp.nom`, `thread.omr` et un bloc `udp` (port, sessions, rx, tx, pertes, tampons_min).

# 4. Replis

- **F1. La borne route mais ne republie pas notre service.**
  - La decouverte par nom d'hote SRP fonctionne quand meme ; c'est de toute facon la base de la v1.
  - Le nom change apres une nouvelle mise en service, peut-etre aussi apres un changement de reseau Thread (a verifier). L'app le relit a chaque branchement USB.
- **F2. Enregistrement operationnel Matter.**
  - Le pont publie deja une instance `_matter._tcp` par fabric, de la forme `<CompressedFabricId>-<NodeId>`, et les bornes la republient.
  - La carte la donnerait par USB, depuis la table des fabrics.
  - L'app fait `-L` sur ce nom pour obtenir l'hote puis l'AAAA, et parle ensuite a notre port.
  - Survit a un changement de nom d'hote tant que la fabric existe, sans rien ajouter en SRP. Suppose que la borne ne filtre pas les ports.
- **F3. Adresse IPv6 saisie a la main, ou derniere OMR lue par USB.** Depannage seulement : casse au changement d'OMR.
- **F4. La borne filtre les ports ET ne republie pas notre service.**
  - Pas de chemin propre par les bornes Apple : on garde l'USB.
  - Derniere option, a reevaluer : l'env esp32c6supermini (Matter sur Wi-Fi, UDP direct sur le LAN).
  - Mais Thread est alors abandonne.
  - Et le canal Wi-Fi 1 (2402 a 2422 MHz) recouvre la lampe a 2405 MHz : il faudrait imposer les canaux 6 a 11 (a verifier sur la box).
- **F5. Ce Mac ne garde pas la route.** Passer par l'iPhone (une seule interface) ou un Mac a une seule interface. L'app macOS retombe sur l'USB et explique pourquoi.

Aucun fichier du depot n'a ete modifie, rien n'a ete flashe et aucun port serie n'a ete ouvert. Script de test ajoute : `/private/tmp/claude-501/halo_udp_test.py`.

---

## Verification sceptique

**Contre-expertise des cinq hypothèses du go/no-go (Thread via les bornes Apple)**

Recherche en lecture seule. Je n'ai rien modifié dans le dépôt, rien flashé et ouvert aucun port série. Aucun paquet n'a atteint le nœud : le seul essai UDP a échoué au `connect` sur « No route to host ». Mes fichiers de travail sont dans `/private/tmp/claude-501/skeptic/`.

## Synthèse

| # | Hypothèse | Verdict |
|---|---|---|
| 1 | Les bornes Apple font passer n'importe quel UDP entre le LAN et Thread | **Incertain.** L'annonce de route est confirmée. Le passage vers les ports enregistrés en SRP (5540) est confirmé ailleurs. Rien ne prouve le passage vers un port non annoncé. |
| 2 | Le proxy des bornes republie un service SRP maison | **Probable, non prouvé.** Le code source ne filtre pas par type ; le logiciel livré sur les bornes n'est pas vérifié. |
| 3 | Un second service SRP peut vivre à côté de ceux de CHIP | **Oui, sous conditions.** Les dangers sont confirmés, et j'en ajoute un. L'affirmation du rapport « apple-br » sur ce point est réfutée. |
| 4 | Les sockets lwIP marchent sur l'interface Thread de ce build | **Confirmé**, avec les mises en garde du rapport « firmware », toutes vérifiées. |
| 5 | Règles de confidentialité du réseau local sur iOS et macOS | **Confirmé sur le fond.** En revanche, les tests du rapport « app » ne vérifiaient pas ce point. |
| — | Ce Mac joint le réseau Thread aujourd'hui | **Non.** C'est un état bloqué du noyau du Mac, et attendre ne le réparera pas. |

Deux erreurs dans les rapports :
- **Le rapport « app » vise le mauvais nœud.** `32E1CCA9C1A0F448` annonce SII=6000 SAI=1100 SAT=500, comme 10 autres hôtes (autres marques), sur une seule fabric. Ce nom ne se résout même plus.
- **Le nœud du rapport « apple-br » est probablement le bon.** `561F9A6463953778` → `fd77:9e:f4bb:0:6c06:6762:45d6:a3f0` est le seul nœud Thread qui annonce les valeurs 2000/2000 du sdkconfig. L'autre hôte en 2000/2000 est le hub Aqara lui-même : `54EF448D15E50000.local`, port 5552, T=6, fabric à part. C'est à confirmer par USB ; ce n'est pas prouvé. Script : `/private/tmp/claude-501/skeptic/txt.py`.

## Le Mac : pourquoi la route manque, et pourquoi attendre ne suffit pas

- Relevé à nouveau à 16 h : `route -n get -inet6 fd77:9e:f4bb::1` → « not in table ». `ping6` et `connect` UDP → EHOSTUNREACH.
- **Deux entrées marquées « installée » à la fois.** La liste des routes annoncées du noyau (`rtilist.py`) a deux entrées en `stateflags=0x1`, sans marque « scoped » : `fe80::cf4:afe0:c89a:397c%en0` et `fe80::42a:d4d9:3614:70d3%en18`.
- **Le noyau ne tient qu'une seule route par préfixe.** XNU installe ces routes sans les lier à une interface (`nd6_rtr.c`, `defrouter_select` : « XXX For now we treat RTI routes as un-scoped »). Deux marques en même temps sont donc incohérentes. `defrouter_select` journalise d'ailleurs ce cas comme « this should not happen » (« more than one » routeur installé).
- **Le noyau ne la réinstallera jamais seul.** `defrouter_addreq` sort sur « already installed » dès que la marque est posée, et ne la pose qu'après un `rtrequest` réussi. La route a donc été supprimée après coup sans que la marque soit effacée. Les annonces rafraîchies toutes les ~180 s ne la remettront pas : « débrancher en18 et attendre 3 min » ne suffit pas, parce que l'entrée en0 garde sa marque.
- 200 s d'écoute de `route -n monitor` : aucun événement fd77.
- **Test qui tranche (à faire par toi) :**
  1. Couper le Wi‑Fi **et** débrancher l'Ethernet USB (ou redémarrer), puis rebrancher une seule interface.
  2. Vérifier que `route -n get -inet6 fd77:9e:f4bb::1` donne une passerelle `fe80::…%enX`.
  3. Rebrancher la seconde interface et relancer ce test. Si la route disparaît, le double branchement sur le même réseau est la cause. Il faudra alors l'indiquer comme configuration non prise en charge par l'app macOS.

## 1. Passage de n'importe quel UDP par les bornes Apple : incertain

**Confirmé :**
- Les bornes annoncent la route : `fd77:9e:f4bb::/64`, 5 bornes Apple × 2 interfaces en préférence moyenne, plus l'Aqara en basse, durée 1800 s.
- La prise en charge de ces annonces par XNU est notée « since xnu-7195 » (macOS 11 / iOS 14) dans un tableau tiers.

**Ce qui fragilise l'hypothèse :**
- Le guide « Thread Border Router Best Practices » (Thread Group, §4.1) dit : « MUST ensure that only ingress traffic enters a Thread network that Thread hosts have expressed interest in ». Une borne peut donc légitimement ne laisser entrer que les ports annoncés en SRP.
- Toutes les preuves publiques de passage par une borne Apple visent des ports enregistrés en SRP : Matter 5540 (python-matter-server) et `_hap._udp` (HomeKit Controller).
- Sur ce réseau, rien ne montre un autre port : 57 des 58 services `_matter._tcp` sont sur 5540 (le 58e est l'Aqara, sur le réseau local), et il n'y a aucun `_hap._udp` (`hap.py`).

**Test sans rien flasher, dès que la route existe :**
- lwIP répond « port injoignable » (ICMPv6) sur un port fermé. `udp_input` appelle `icmp6_dest_unreach` à 0x4204ffea dans firmware.elf, et OpenThread passe à lwIP tout port qu'il n'utilise pas (`ip6.cpp:986`).
- Commande :
  ```
  python3 -c "import socket;s=socket.socket(socket.AF_INET6,socket.SOCK_DGRAM);s.settimeout(3);s.connect(('fd77:9e:f4bb:0:6c06:6762:45d6:a3f0',40000));s.send(b'x');s.recv(9)"
  ```
- Lecture du résultat :
  - `ConnectionRefusedError` : un port quelconque passe.
  - Délai dépassé alors que `ping6` répond (lwIP répond aux pings, mode d'écho `RLOC_ALOC_ONLY`) : la borne filtre par port.
- **Si la borne filtre**, le service SRP devient obligatoire, ce qui renverse la recommandation v1 « résoudre `<hôte>.local` ».

## 2. Republication d'un service SRP maison : probable, non prouvé

- **Code source :** mDNSResponder-2881.0.25 (commit d4658af). `_matter` et `_hap` ne servent qu'aux statistiques (`srp-mdns-proxy.c:358-372`). Il n'y a pas de limite de services par hôte. Le seul contrôle lié au type porte sur la cohérence des sous-types `_sub`.
- **Incertain :** le logiciel livré sur HomePod et Apple TV peut différer. Je n'ai trouvé aucun rapport public d'un type tiers republié par une borne Apple.
- **Test :** impossible sans flasher. Il faut la sonde du firmware (`udp srp on`), puis `dns-sd -B _halo-pont._udp local.` et `dns-sd -L`.

## 3. Second service SRP à côté de ceux de CHIP : oui, sous conditions

**Confirmé :**
- CHIP efface 628 octets sur chaque service retiré : `memset(service,0,sizeof(Service))` (hpp:907), `li a2,628` à 0x421022e0. `mService` est bien le premier membre (.h:184).
- `_ClearSrpHost` → `otSrpClientRemoveHostAndServices(false,true)` à chaque démarrage : notre service tombe avec ceux de CHIP.
- `MAX_SERVICES=5` ne dimensionne que les tampons (`OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_SERVICES`), ce n'est pas une limite.

**Réfuté (rapport « apple-br ») :** `_InvalidateAllSrpServices` et `_RemoveInvalidSrpServices` ne parcourent que `mSrpClient.mServices`, le tableau de CHIP. Elles ne retirent pas notre service.

**Nouveau risque, lu dans le code d'Apple :**
- Le serveur rejette la mise à jour entière (YXDOMAIN) si un nom d'instance pointe déjà vers un autre nom d'hôte (`srp-mdns-proxy.c`, `compare_instance`, lignes 3309-3352).
- Une mise à jour SRP porte l'hôte et tous ses services. Un nom fixe `Halo-XXXXXX` resté sous l'ancien hôte bloquerait donc aussi les services Matter jusqu'à l'expiration de l'ancien bail. Cas typique : effacement complet par esptool puis nouvelle mise en service, sans la désinscription que CHIP envoie lors d'une vraie remise à zéro.
- **Contre-mesures :** la surveillance « 60 s en ToAdd/Adding → `otSrpClientClearService` » proposée par le rapport « firmware » devient obligatoire. Et le nom d'instance doit dériver du nom d'hôte SRP.
- C'est une raison de plus de faire la v1 sans service maison, sous réserve du test 1.

## 4. Sockets lwIP sur l'interface Thread de ce build : confirmé

- `OpenthreadLauncher.cpp.obj` importe `esp_netif_new`, `esp_netif_attach` et `esp_openthread_netif_glue_init`.
- `UDPEndPointImplLwIP::SendMsgImpl` est lié, et Matter fonctionne par ce chemin.
- **Blocage :** `openthread_netif_transmit` (0x4218b544) et le crochet de choix d'adresse source (0x4218b3f8) prennent tous deux le verrou OpenThread sans limite de temps (`li a0,-1`).
- **Filtre :** `IsPortInUse` (`ip6.cpp:986`) empêche lwIP de voir un port tenu par OpenThread.
- **Port 52540 :** il est dans la plage éphémère d'OpenThread (49152–65535, `udp6.hpp:654`), et `GetEphemeralPort` ne saute que les ports réservés. Il faut un port fixe sous 49152, quelle que soit la voie.
- **Flash :** 3 002 480 octets pour 0x300000, soit 143 248 octets libres. Confirmé.

## 5. Confidentialité du réseau local : confirmé, mais pas testé par le rapport « app »

**Confirmé (TN3179) :**
- Un envoi unicast vers une adresse jointe via un routeur n'est pas du « réseau local » : « Traffic to a local network address goes directly; it's not forwarded by a router ».
- Mais « Resolving a local DNS name » (`.local`), ainsi que la navigation et la résolution Bonjour, demandent l'autorisation. L'alerte est donc inévitable pour les deux méthodes de découverte.
- `NSBonjourServices` n'est nécessaire que pour la navigation.
- Il faut signer avec une identité délivrée par Apple.

**Les tests du rapport « app » ne couvrent pas ce point :**
- Ce sont des exécutables `sbtest` signés en ad hoc, sans `NSLocalNetworkUsageDescription`.
- Or TN3179 autorise d'office les « Command-line tools run from Terminal or over SSH, including any child processes ».
- Seule la partie bac à sable est prouvée : `network.client` est nécessaire et suffit pour un UDP connecté.

**Test :** l'app réelle signée Apple Development, lancée depuis le Finder. Vérifier l'alerte, puis le cas de refus (`.waiting` avec `localNetworkDenied`). Refaire sur un vrai iPhone (le simulateur ne gère pas cette autorisation).

## Identifier le nœud sans flasher

Couper puis rétablir l'alimentation du pont (pas par le port série) pendant que `dns-sd -B _matter._tcp local.` tourne.

Au démarrage, CHIP retire puis réenregistre ses services (`_ClearSrpHost`). Les deux instances du pont devraient donc apparaître en « Rmv » puis « Add ». `dns-sd -L` sur l'une d'elles donnera ensuite l'hôte.

## Sources
- TN3179 : https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy
- Thread Group, BR Best Practices : https://www.threadgroup.org/Portals/0/documents/support/ThreadBorderRouterBestPractices_2530_1.pdf
- mDNSResponder : https://github.com/apple-oss-distributions/mDNSResponder
- XNU nd6_rtr.c : https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet6/nd6_rtr.c
- Tableau de prise en charge RFC 4191 : https://github.com/dxdxdt/gists/blob/master/writeups/ipv6/rfc4191/rfc4191.md
- HomeKit Controller : https://www.home-assistant.io/integrations/homekit_controller/
- Forum Home Assistant : https://community.home-assistant.io/t/matter-over-thread-devices-periodically-go-unavailable-host-has-no-ipv6-route-to-thread-subnet-homepod-mini-tbr-apple-home-unaffected/1011614

Fichiers dans `/private/tmp/claude-501/skeptic/` : `txt.py`, `hap.py`, `routemon.txt`, `tn3179.txt`, `brbp.txt`.
