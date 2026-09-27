**Français** · [English](README.md)

# halo-routes : assistant systeme du Mac pour le reseau Thread

Demon launchd (root) qui garde la route du Mac vers le reseau Thread, donc vers
le pont Halo en UDP (docs/PROTOCOLE-JSON.md, section 10).

## Pourquoi

Le Mac joint les noeuds Thread par le prefixe OMR (ULA `/64`) que les routeurs
de bordure (HomePod, Apple TV) annoncent sur le LAN (option RIO des annonces de
routeur). Bug du noyau de macOS (10.1) : il retire la route de ce prefixe quand
il change de routeur (l'un d'eux parait un instant injoignable, ou son annonce
expire), mais sa liste des routes annoncees peut la croire toujours posee. La
route n'est alors plus remise : `No route to host` jusqu'au redemarrage de
l'interface, parfois au-dela. Une route statique tient mieux, mais il faut etre
root pour la poser, et une app ne peut pas choisir seule son routeur de sortie.

## Ce qu'il fait

A chaque message du noyau sur les routes (300 ms apres, les rafales sont
fondues) et toutes les 10 s (un routeur perdu ne donne lieu a aucun message), il
relit la liste des routes annoncees du noyau
(`sysctl net.inet6.icmp6.nd6_rtilist`), l'etat des routeurs (cache des voisins)
et celui des interfaces. Pour chaque prefixe ULA `/64` annonce :

- aucune route : il en pose une, statique, marquee `RTF_PROTO1` (drapeau `1`
  dans `netstat -rn`), via le routeur le plus sur : joignable d'abord (voisin
  `REACHABLE`, `STALE`, `DELAY` ou `PROBE`, comme en juge le noyau), puis celui
  que le noyau croit avoir installe (s'il tombe, le noyau retire lui-meme la
  route et pose la sienne), puis sur l'interface principale ; jamais via une
  interface tombee ;
- notre route passe par un routeur qui n'annonce plus le prefixe : il change de
  routeur ; par un routeur injoignable (ou dont l'interface est tombee) alors
  qu'un autre est joignable : il change aussi, si c'est confirme a deux passages
  espaces de 2 s au moins. Le changement se fait sur place (`route change` :
  passerelle et interface, sans coupure) ; a defaut, retrait puis ajout ;
- plus aucune annonce : il retire notre route (l'annonce expiree, le noyau
  n'aurait plus de route non plus).

La route statique n'est pas a l'abri : quand le noyau change de routeur ou
qu'une annonce expire, il retire la route du prefixe, quelle qu'elle soit (par
prefixe et masque), et pose en general la sienne ; sinon, le demon remet la
sienne au passage suivant. Au plus 6 changements par prefixe et par minute ;
apres un echec, rien avant la minute suivante. A l'arret (`launchctl bootout`,
desinstallation), il retire les routes qu'il a posees.

## Ce qu'il ne fait pas

- Il ne touche a aucune route qui n'est pas a lui : celles du noyau, celles
  posees a la main, et tout prefixe hors de `fc00::/7` ou d'une autre longueur
  que 64. Il ne pose que ce que le noyau aurait pose lui-meme pour une annonce
  qu'il a acceptee.
- Aucune entree hors du noyau : ni port reseau, ni fichier de commande, ni
  argument a l'execution. Les changements passent par `/sbin/route`, argv
  fixe, sans shell.
- Le prefixe du reseau Thread n'est pas ecrit en dur : s'il change, le demon
  suit les annonces.

## Installer

Sous son compte, sans sudo (le programme est compile et teste ici ; seules la
copie et la mise en service demandent le mot de passe administrateur) :

```
sh tools/macos/halo-routes/installer.sh
```

L'installeur montre d'abord ce que le demon ferait (essai, rien n'est change).
Une route posee a la main pour le meme prefixe reste a son proprietaire : le
demon la laisse en place et ne la garde pas. L'installeur la signale ; la
retirer pour qu'il en prenne la garde (`sudo route -n delete -inet6
-prefixlen 64 <prefixe>`).

Fichiers : `/Library/PrivilegedHelperTools/fr.djoko.halo.routes` (programme),
`/Library/LaunchDaemons/fr.djoko.halo.routes.plist` (lancement au demarrage,
relance s'il s'arrete), `/Library/Logs/fr.djoko.halo.routes.log` (journal).

## Verifier

```
tail -f /Library/Logs/fr.djoko.halo.routes.log
netstat -rn -f inet6 | grep '^fd'
```

Une route du demon porte les drapeaux `S` (statique) et `1` (sa marque). Essai
sans rien installer ni changer (pas besoin d'etre root) :

```
sh tools/macos/halo-routes/tests.sh
```

## Desinstaller

```
sh tools/macos/halo-routes/desinstaller.sh
```

Le demon retire ses routes en s'arretant ; le journal est garde.

## Limites

- Tue sans pouvoir se nettoyer (SIGKILL), il reprend ses routes a la relance
  tant que leur prefixe est annonce ; une route dont le prefixe a disparu
  entre-temps reste jusqu'au redemarrage du Mac.
- Course minime : entre sa lecture de la table et un retrait ou un changement,
  le noyau peut poser sa propre route pour le prefixe ; `route` vise le prefixe
  et le masque sans regarder a qui est la route, et toucherait alors celle du
  noyau (le passage suivant remet une route si besoin).
- Au plus 32 prefixes ULA `/64` suivis (et 32 routeurs par prefixe) : au-dela,
  les prefixes en trop sont ignores et, tant que la liste deborde, le demon ne
  retire plus ses routes faute d'annonce ; le journal le signale.
- Le journal n'est pas tourne : quelques lignes par incident du noyau.
- Contourne le bug, ne le corrige pas : sans demon, la route du noyau peut
  toujours disparaitre.
