# Source "Reseau" de l'app Halo Compagnon (phase 2) : design

Date : 25/09/2026. Statut : design valide section par section avec Djoko, a
relire avant le plan d'implementation.

References : docs/PROTOCOLE-JSON.md section 10 (transport UDP sur Thread,
enveloppe H1, liste blanche, profil distant), docs/ETUDE-THREAD-COMPAGNON.md
(phase 2), tools/macos/halo-routes/ (route du Mac vers le reseau Thread).

## 1. But et perimetre

L'app macOS joint le pont par le reseau, sans cable : UDP sur Thread a travers
les routeurs de bordure, enveloppe H1 (HMAC-SHA256, cle partagee). Une fois
connectee, elle montre le meme tableau de bord, le meme journal et la meme
console que par l'USB, dans les limites de la liste blanche (10.5) et du
profil distant (10.6).

Dans le perimetre :
- la source "Reseau" (transport UDP, enveloppe H1, reprise) ;
- la creation de la cle par l'USB et sa garde dans le trousseau ;
- les messages d'erreur (route absente, reseau local refuse, cle, pont) ;
- la lecture de la cle du trousseau par `tools/halo_udp.py` ;
- la signature Apple Development de l'app.

Hors perimetre :
- iOS (phase 3) ; le code nouveau reste cependant sans AppKit quand il le peut ;
- plusieurs sources a la fois (USB et reseau ensemble) ;
- toute cadence au-dela du profil distant (compteurs, trames continues) tant
  que R3 (MAX_RT avec et sans client distant) n'est pas mesure ;
- la decouverte DNS-SD (`_halo-pont._udp`, `NWBrowser`) : le nom SRP suffit (10.3).

## 2. Decisions

1. **Un transport UDP derriere le protocole `Transport` existant** (approche A).
   Pour `Pont`, `MoteurSession` et le correlateur, c'est un port de plus : un
   datagramme recu devient une ligne RS + JSON + LF, ce que le commentaire de
   `Transport` prevoyait. Ecartee : un moteur de session reseau separe
   (bail, `id`, silences et correlation en double).
2. **L'enveloppe H1 est du code pur dans `HaloProtocole`**, testee avec les
   vecteurs de 10.4 (les memes que le firmware). Pas de framework
   `HaloReseau` separe : `HaloProtocole` deviendra multiplateforme en phase 3.
3. **L'app cree la cle par l'USB** (`json cle nouvelle`) et la garde dans le
   trousseau du Mac ; `halo_udp.py` la relit dans le trousseau. L'app est la
   source de verite de la cle.
4. **Signature Apple Development** (equipe personnelle gratuite de Djoko) :
   l'autorisation reseau local et un trousseau stable d'une compilation a
   l'autre la demandent. L'identifiant d'equipe reste hors du depot public.
5. **Profil distant tel quel** (etat toutes les 2 s, reseau toutes les 30 s,
   ni compteurs ni trames) : chaque ligne fait des trames 802.15.4 a quelques
   centimetres du BM5602 ; on n'augmente rien avant R3.

## 3. Composants

### 3.1 HaloProtocole (pur, teste sans reseau)

`Reseau/EnveloppeH1.swift` (CryptoKit, `HMAC<SHA256>`, 16 premiers octets en
32 hexa MAJUSCULES) :
- `kid(cle)` : 8 premiers hexa de SHA-256(cle) ;
- `salut(cle:kid:na:)` : texte `H1 SALUT <kid> <na> <mac_salut>` ;
- `verifierDefi(_:cle:kid:na:)` : `(sid, nc)` si le datagramme est un DEFI au
  MAC juste pour ce `na`, sinon nil (DEFI d'un essai precedent, ou faux) ;
- `cleSession(cle:na:nc:sid:)` : Ks ;
- `SessionH1` (struct, `Sendable`) : `sceller(ligne)` rend
  `H1 <sid> <ctr> <mac> <ligne>` (sens `A`, `ctr` +1 a chaque appel) ;
  `ouvrir(datagramme)` rend la charge d'un message `C` si forme canonique,
  `sid`, MAC (comparaison en temps constant) et fenetre sont bons, sinon nil et
  `ecartes` +1 ;
- fenetre anti-rejeu de 32, jugee APRES le MAC (un `ctr` forge ne la pousse
  jamais) ;
- forme canonique stricte (10.4) : hexa en majuscules seulement, `ctr`
  decimal sans zero de tete, 1..4294967295.

`Commandes/Correlateur.swift` : une regle de plus, reservee a l'UDP. Une
commande envoyee sans aucune `reponse` est renvoyee avec le meme `id` (memes
octets ; le transport y met un `ctr` neuf) a 2 s puis a 4 s, et passe
`sansReponse` a 6 s. Un doublon de `reponse` `fin` pour une commande terminee
est ignore. `deja_traite` termine la commande et demande un `json etat`
(10.2). L'USB garde ses regles (pas de renvoi, `sansReponse` a 3 s).

Liste blanche cote app (`autoriseeADistance`, deja la) : on verifie qu'elle
refuse `json cle ...`, `reboot` et le reste de 10.5 avant l'envoi.

### 3.2 App

`Reseau/TransportUDP.swift` : `final class TransportUDP: Transport`,
`genre = .udp`, sur `NWConnection` (UDP, IPv6 impose) vers `<hote>:5480`,
`hote` = `<nom>.local` (ou une adresse, pour les tests). Etat sous `Mutex`,
file Dispatch propre, `AsyncStream<EvenementTransport>` comme
`TransportSerie`.
- `ouvrir()` : attend `.ready` (5 s au plus), puis la poignee de main :
  SALUT signe, `na` neuf a chaque essai, 2 s d'attente d'un DEFI juste, 3
  essais ; rend le flux, ou leve une `ErreurReseau` (4.6).
- `envoyer(_:)` : decoupe en lignes ; ecarte les lignes vides ou de controle
  seul (le Ctrl-U `LigneCommande.effacement` que `MoteurSession` envoie a
  l'ouverture n'a pas de sens ici) ; scelle chaque ligne dans son datagramme.
- reception : `receiveMessage` en boucle ; `SessionH1.ouvrir` ; charge rendue
  en RS + JSON + LF ; datagrammes ecartes comptes.
- `fermerApresVidage` : `json 0` deja confie part, puis fermeture a la
  confirmation d'envoi ou apres 300 ms au plus.
- apres `.ready`, un passage en `.waiting` ou `.failed` ferme le flux
  (`.ferme(raison:)`).
- delais (attente de `.ready`, du DEFI, nombre d'essais) injectables pour les
  tests.

`Reseau/Trousseau.swift` :
- protocole `TrousseauCles` : `lister() -> [PontConnu]` (`nom`,
  `empreinte`), `lire(nom:) throws -> Data`, `ranger(nom:cle:empreinte:)
  throws`, `oublier(nom:) throws` ;
- implementation reelle : mot de passe generique du trousseau de session
  (`SecItem`), service `fr.djoko.halo.pont`, compte = nom SRP (16 hexa, sans
  `.local`), valeur = les 64 hexa MAJUSCULES, commentaire = empreinte, libelle
  "Halo - pont <nom>" ; non synchronise (iCloud : phase 3) ;
- implementation en memoire pour les tests.

`Modele/Pont.swift` :
- `Source.reseau(nom: String)` ; `sourceParDefaut` : port Espressif, sinon
  pont reseau connu du trousseau, sinon demo ;
- `ouvrir()` : lit la cle (erreur `cleAbsente` sinon), cree `TransportUDP` ;
- activite anti-App Nap tenue aussi pendant une session reseau ;
- reprise : la reconnexion existante (0,3 s, 1 s, 2 s, 5 s ; 40 essais) ; au-dela,
  la source reseau reprend a un changement de chemin reseau (`NWPathMonitor`)
  ou au reveil du Mac, la ou l'USB attend le retour du port (IOKit) ;
- console : en UDP, toute ligne passe par le correlateur avec un `id` (jamais
  `ligneBrute` : le pont ignore une ligne sans `id` a distance) ;
- creation de la cle (section 5) et masquage du secret.

Vues :
- barre laterale : section "Reseau", un element par pont connu du trousseau
  (`<nom>.local`, empreinte), clic = connexion ; menu contextuel "Oublier ce
  pont..." ;
- carte "Thread et Matter", par l'USB : ligne "Transport reseau" a trois etats
  et les boutons de la section 5 ;
- en mode reseau : commandes hors liste blanche grisees (ecran Commandes,
  outils de banc), avec leur raison en aide.

Projet (`project.yml`, cibles app et tests) :
- droit `com.apple.security.network.client` (la sandbox reste) ;
- `INFOPLIST_KEY_NSLocalNetworkUsageDescription`, texte francais et anglais ;
- `Signature.xcconfig` commite (signature ad hoc par defaut) qui se termine
  par `#include? "Local.xcconfig"` ; `Local.xcconfig` (ignore par git) donne
  `DEVELOPMENT_TEAM` et `CODE_SIGN_IDENTITY = Apple Development`. Sans lui, le
  depot compile toujours, signe ad hoc (sans autorisation reseau local
  durable).

### 3.3 Hors de l'app : tools/halo_udp.py

- lecture de la cle, dans l'ordre : `HALO_CLE` ; le trousseau (`security
  find-generic-password -s fr.djoko.halo.pont -a <nom> -w`, `<nom>` tire de
  l'hote `<nom>.local` ; pour une adresse, le seul pont du trousseau, et une
  erreur s'il y en a plusieurs ; macOS demande une fois d'autoriser
  `security`) ;
  `~/.config/halo-pont/cle` ;
- `cle <port>` n'ecrit plus que dans le fichier designe par `HALO_CLE`
  (obligatoire) : voie d'un banc sans l'app ; il previent que la cle de l'app
  devient perimee. Raison : une cle ecrite dans `~/.config` serait masquee par
  celle du trousseau, lue avant.

## 4. Deroulement

### 4.1 Ouverture de `.reseau(nom)`

1. `Pont` lit la cle du trousseau, cree `TransportUDP(hote: "<nom>.local", cle)`.
2. `ouvrir()` : `NWConnection`, attente de `.ready` ; le nom est resolu a
   chaque ouverture (changement d'OMR suivi, R5).
3. Poignee de main (10.4) ; session H1 etablie ; flux rendu.
4. `Pont` enchaine comme pour l'USB : `id=N json 1`, `hello`, instantane. La
   carte applique le profil distant (`hello.base.session.transport` = `udp`).

### 4.2 Envoi

Chaque ligne : `ctr` neuf, MAC, un datagramme. Une commande a la fois en vol
(6.5). Sans reponse : meme `id` renvoye a 2 s et 4 s, `sansReponse` a 6 s
(3.1). La carte repond depuis son cache sans reexecuter, ou par `deja_traite`.

### 4.3 Reception

Seul un datagramme au `sid` de la session, au MAC juste (sens `C`) et au `ctr`
admis par la fenetre est rendu. Les autres (lignes d'une session precedente,
DEFI en retard, falsifications) sont ecartes sans bruit et comptes.

### 4.4 Bail, silences, reprise

Mecanique de `MoteurSession`, inchangee : `json ping` apres 10 s sans autre
commande (bail 30 s) ; apres 6 s sans ligne (3 x la periode de 2 s),
`json 1` renvoye ; sans `hello` 5 s plus tard, `.rouvrir` : fermeture,
nouvelle resolution, nouvelle poignee de main. Couvre le redemarrage du pont
et une session H1 oubliee par la carte (10 min sans message, nouvelle cle).

### 4.5 Fermeture

Deconnexion, changement de source, fin de l'app : `json 0` scelle, puis
fermeture (envoi confirme ou 300 ms). La spec 10.4 le demande : un dernier
message capture ne peut plus etre rejoue ensuite.

### 4.6 Erreurs

`TransportUDP` leve `ErreurReseau` ; `Pont` en tire le message (francais et
anglais) et la reprise.

| Cas | Detection | Message (resume) | Reprise |
|---|---|---|---|
| Reseau local refuse (R7) | chemin `unsatisfied` pour `.localNetworkDenied`, ou `PolicyDenied` a la resolution ; au banc (25/09, macOS 27) : `NoSuchRecord` aussitot a la resolution de `<nom>.local`, chemin `satisfied` | "Acces au reseau local refuse : Reglages Systeme > Confidentialite et securite > Reseau local" | bandeau tenu pendant les essais ; reprise automatique (amende au banc : le refus se leve sans evenement pour l'app, et la connexion en attente repart seule a la reautorisation) |
| Pas de route (bug du noyau, 10.1) | `EHOSTUNREACH`, `ENETUNREACH`, `ENETDOWN` | assistant absent : "installer l'assistant : sh tools/macos/halo-routes/installer.sh" ; present (si la sandbox laisse voir son plist) : "route en cours de retablissement" | automatique |
| Nom introuvable | resolution de `<nom>.local` en echec ou en delai ; `EHOSTDOWN` (ICMPv6 adresse injoignable : le noeud ne repond pas) | "Pont introuvable : eteint, hors du reseau Thread, ou routeurs de bordure injoignables" | automatique |
| Port injoignable | ICMPv6 apres le SALUT (`ECONNREFUSED`) | "Le pont n'a plus de cle : le brancher en USB, puis Activer l'acces reseau" | arret, "Reessayer" |
| Aucun DEFI (3 essais) | silence | "Aucune reponse : cle differente (comparer les empreintes par l'USB) ?" | automatique |
| Pas de `hello` apres le DEFI | `MoteurSession` (`aucuneReponse`) | ajout : "deux autres sessions deja actives (autre Mac, iPhone, halo_udp.py) ?" | `json 1` renvoye (meme `id`) a 2, 4 et 6 s ; 30 s apres le dernier, nouvelle poignee de main (`.rouvrir`, note "nouvelle poignee de main") et non `json 1` : la carte a oublie la session provisoire (30 s apres le SALUT, 10.4) ; "Reessayer" de meme ; le bandeau reste jusqu'au `hello`, sa note une fois |
| Cle absente du trousseau | lecture a l'ouverture | "Cle absente de ce Mac : brancher le pont en USB, puis Activer l'acces reseau" | arret |
| Trousseau refuse ou en erreur | `OSStatus` | texte de l'erreur | arret, "Reessayer" |
| Chemin perdu en session | `.waiting` / `.failed` (ou erreur de reception) apres `.ready` | "Connexion reseau perdue : <cause>" | automatique |

Les cas "arret" s'affichent en bandeau, par la voie des alertes graves ; les
autres dans la ligne d'etat de la barre laterale ("en attente : ..."). Un
meme message n'est note qu'une fois dans la console tant que la cause ne
change pas.

## 5. La cle

### 5.1 Creation (USB seulement)

Ligne "Transport reseau" de la carte Thread, d'apres le bloc `ip`
(`udp.ouvert`, `udp.empreinte`, `srp.nom`) et le trousseau :
- pas de cle sur le pont : bouton "Activer l'acces reseau..." ;
- cle du pont = cle de ce Mac (meme empreinte, meme nom) : "cle connue de ce
  Mac", et "Nouvelle cle..." en option ;
- autre cle : "cle inconnue de ce Mac", bouton "Nouvelle cle...".

Boutons grises tant que `srp.nom` n'est pas connu. Confirmation : "Les
sessions reseau en cours tombent ; halo_udp.py relira la nouvelle cle dans le
trousseau."

Deroulement : 32 octets de `SecRandomCopyBytes` ; `id=N json cle nouvelle
<64 HEXA>` par le correlateur ; a la `reponse` `fin` `ok` : `empreinte` =
kid(`cle`) verifiee, puis `ranger(nom: srp.nom, cle:, empreinte:)` ; le pont
apparait dans la section "Reseau". `refuse` : message, rien ne change. Sans
reponse : l'app relit `json cle` ; une empreinte inconnue fait proposer de
recommencer (10.4).

### 5.2 Secret

La cle ne va que dans le trousseau : jamais dans la console, le journal, les
suivis de commandes, les preferences ni un fichier. La commande est suivie et
affichee sous le texte `json cle nouvelle` (sans l'alea) ; le champ `cle` de
la reponse est masque partout ou la ligne est montree ou gardee. Seule
l'empreinte s'affiche.

### 5.3 Cle effacee par le pont, pont oublie

Remise a zero Matter ou depart du dernier controleur : le pont efface sa cle
(10.4) ; la connexion echoue en "port injoignable" (4.6). "Oublier ce
pont..." (barre laterale) supprime l'element du trousseau apres confirmation ;
le pont garde sa cle.

## 6. Tests

Unitaires, `HaloProtocoleTests` :
- `EnveloppeH1Tests` : vecteurs 10.4 a l'octet (kid 630DCD29, SALUT, MAC du
  DEFI, Ks, datagramme A 1, ouverture de C 1) ; forme canonique (minuscules,
  `ctr` a zero de tete ou nul, champs, `sid` etranger, message de sens `A`
  presente comme `C`) ; fenetre (ordre, desordre, rejeu, trop ancien, saut ;
  un MAC faux ne pousse jamais la fenetre) ;
- `CorrelationTests` : renvois UDP a 2 s et 4 s, `sansReponse` a 6 s ; USB
  inchange ; doublon de `fin` ignore ; `deja_traite` ;
- liste blanche : `json cle`, `json cle nouvelle`, `reboot` refusees en UDP.

App, `HaloCompagnonTests` :
- `TransportUDP` face a un pair UDP local sur `[::1]` qui joue le cote pont du
  deroulement normal (enveloppe H1, cle des vecteurs), delais raccourcis :
  lignes rendues en RS + JSON + LF ; commande scellee recue ; `json 0` a la
  fermeture ; port ferme -> "port injoignable" ; pair muet -> "aucun DEFI"
  apres 3 essais ;
- creation de la cle de bout en bout : le simulateur de demo repond a
  `json cle nouvelle` ; trousseau en memoire ; la cle y arrive ; aucune suite
  de 64 hexa dans la console, le journal ni les suivis ;
- catalogues francais et anglais alignes (tests existants).

Build : avertissements traites en erreurs ; `codesign -dv` montre l'equipe,
la sandbox et `network.client`.

Au banc, avec Djoko (pont reel, route par l'assistant ou a la main) :
1. cle creee depuis l'app par l'USB ; `halo_udp.py session` la relit dans le
   trousseau ;
2. source reseau : `hello` (rev 3, `udp`), `etat` toutes les 2 s, commandes
   lampe depuis l'app (Djoko present) ;
3. R5 : redemarrage du pont, reprise en ~15 s ;
4. R7 : reseau local refuse puis reautorise ;
5. route retiree : message, puis reprise par l'assistant.
Le `decommission` n'est pas teste (destructif).

## 7. Criteres de fin

Tous les tests passent (existants : 96 du framework, 17 de l'app ; et les
nouveaux) ; les points de banc 1 a 5 sont valides avec Djoko.

## 8. Risques et points a verifier

- Confidentialite du reseau local : la resolution `.local` (mDNS) la
  declenche ; l'envoi vers une ULA routee (OMR, hors des sous-reseaux du Mac)
  peut-etre pas. La detection passe par `unsatisfiedReason` et `PolicyDenied` ;
  a confirmer en R7. Banc du 25/09 : ni l'un ni l'autre ; la resolution
  `.local` refusee rend `NoSuchRecord` aussitot (un nom `.local` absent ne
  rend rien), et une session deja ouverte vers l'ULA continue.
- `network.client` doit suffire pour recevoir les reponses d'une socket UDP
  connectee (experience SB_client de l'etude) ; a confirmer au premier essai.
- Trousseau de session (fichier) plutot que trousseau a protection des
  donnees : ce dernier demande un profil d'approvisionnement ; iCloud et le
  partage avec iOS viendront en phase 3.
- La resolution de `<nom>.local` doit rendre l'adresse OMR (le client SRP
  d'OpenThread n'enregistre ni lien local ni maillage) ; a confirmer.
- Voir le plist de l'assistant depuis la sandbox n'est pas garanti : le
  message "pas de route" a alors une seule forme (installer, ou attendre si
  deja installe).
