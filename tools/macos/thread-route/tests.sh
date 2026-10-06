#!/bin/sh
# Tests de Thread Route sans rien installer : decision (test_logique.c), attente du dechargement
# d'un demon (attendre_decharge de commun.sh, sourcee, sous launchctl et sleep simules), ordre de
# l'installation, de la migration depuis halo-routes et de la desinstallation (installer.sh et
# desinstaller.sh --plan, sur une fausse racine ; l'installation et la desinstallation reelles sous
# sudo, clang, launchctl et sleep simules), puis compilation du demon et un passage en essai
# (-n : lit le noyau, ne change rien).
# Une seule branche depend de ce Mac : l'installation reelle ne lit que la vraie racine, donc le
# `|| true` du bootout de l'ancien demon n'y est exerce que si les fichiers de halo-routes y sont
# (celui du nouveau, et celui de la desinstallation, le sont toujours).
#   sh tests.sh
set -eu
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -o "$OUT/test_logique" logique.c test_logique.c
"$OUT/test_logique"

# Les commandes simulees. Elles passent devant le PATH de chaque lancement d'un installateur : un
# installateur lance par ces tests n'atteint jamais le vrai sudo, le vrai launchctl ni le vrai
# clang. Elles notent ce qu'on leur demande dans le journal $J et ne font rien d'autre.
FAUX="$OUT/faux"
J="$OUT/journal"
mkdir -p "$FAUX"
: > "$J"
cat > "$FAUX/sudo" <<'FIN'
#!/bin/sh
echo "sudo $*" >> "$FAUX_JOURNAL"
if [ "$1 $2" = "launchctl bootout" ]; then exit "${FAUX_BOOTOUT:-0}"; fi
exit 0
FIN
# launchctl print : 0 = charge, 113 = inconnu de launchd, comme le vrai. Il compte ses appels
# (FAUX_COMPTE) : les FAUX_PRINT_K premiers repondent FAUX_PRINT, les suivants FAUX_PRINT_APRES
# (sans FAUX_PRINT_K, tous repondent FAUX_PRINT). Passe 100 appels, il repond 250 : une boucle sans
# fin s'arrete d'elle-meme.
cat > "$FAUX/launchctl" <<'FIN'
#!/bin/sh
if [ "$1" = "print" ]; then
  n=$(($(cat "$FAUX_COMPTE") + 1))
  echo "$n" > "$FAUX_COMPTE"
  if [ "$n" -gt 100 ]; then exit 250; fi
  if [ -n "${FAUX_PRINT_K:-}" ] && [ "$n" -gt "$FAUX_PRINT_K" ]; then exit "${FAUX_PRINT_APRES:-113}"; fi
  exit "${FAUX_PRINT:-113}"
fi
echo "launchctl $*" >> "$FAUX_JOURNAL"
FIN
# sleep : ne dort pas, note ce qu'on lui demande (FAUX_DORMIS).
cat > "$FAUX/sleep" <<'FIN'
#!/bin/sh
echo "$*" >> "$FAUX_DORMIS"
FIN
# clang : ecrit, a la place du programme, un script qui se note et sort sur FAUX_ESSAI (l'essai).
cat > "$FAUX/clang" <<'FIN'
#!/bin/sh
sortie=
while [ $# -gt 0 ]; do
  if [ "$1" = "-o" ]; then sortie=$2; shift; fi
  shift
done
nom=$(basename "$sortie")
echo "compile $nom" >> "$FAUX_JOURNAL"
printf '#!/bin/sh\necho "lance %s" >> "$FAUX_JOURNAL"\n' "$nom" > "$sortie"
if [ "$nom" = thread-route ]; then printf 'exit "${FAUX_ESSAI:-0}"\n' >> "$sortie"; fi
chmod +x "$sortie"
FIN
printf '#!/bin/sh\nexit 0\n' > "$FAUX/netstat"
chmod +x "$FAUX/sudo" "$FAUX/launchctl" "$FAUX/clang" "$FAUX/netstat" "$FAUX/sleep"
for c in sudo launchctl clang netstat sleep; do
  if [ "$(PATH="$FAUX:$PATH" sh -c "command -v $c")" != "$FAUX/$c" ]; then
    echo "$c simule n'est pas en tete du PATH : les tests de l'installateur ne sont pas lances." >&2
    exit 1
  fi
done
PRINT=113
PRINT_K=
PRINT_APRES=113
BOOTOUT=0
ESSAI=0
# Lance une commande avec ces commandes simulees (et leurs reponses PRINT, PRINT_K, PRINT_APRES,
# BOOTOUT et ESSAI).
avec_faux() {
  env PATH="$FAUX:$PATH" FAUX_JOURNAL="$J" FAUX_COMPTE="$OUT/compte" FAUX_DORMIS="$OUT/dormis" \
    FAUX_PRINT="$PRINT" FAUX_PRINT_K="$PRINT_K" FAUX_PRINT_APRES="$PRINT_APRES" \
    FAUX_BOOTOUT="$BOOTOUT" FAUX_ESSAI="$ESSAI" "$@"
}
# Remet a zero le compte des launchctl print et la liste des sleep. relectures : le nombre de
# launchctl print faits ; dormis : le nombre de sleep.
raz() { echo 0 > "$OUT/compte"; : > "$OUT/dormis"; }
relectures() { cat "$OUT/compte"; }
dormis() { grep -c . "$OUT/dormis" || true; }
raz

R="$OUT/racine"
racine_neuve() {
  rm -rf "$R"
  mkdir -p "$R/Library/LaunchDaemons" "$R/Library/PrivilegedHelperTools"
}
plan() { avec_faux THREAD_ROUTE_RACINE="$R" sh installer.sh --plan; }
PT=$R/Library/PrivilegedHelperTools
LD=$R/Library/LaunchDaemons
BOOT_ANCIEN="launchctl bootout system/fr.djoko.halo.routes"
ligne_print() { echo "launchctl print system/$1 [doit repondre introuvable (au plus 25 s d'attente), sinon arret]"; }
PRINT_ANCIEN=$(ligne_print fr.djoko.halo.routes)
PRINT_NEUF=$(ligne_print fr.djoko.thread.route)
RM_ANCIEN="rm -f $PT/fr.djoko.halo.routes $LD/fr.djoko.halo.routes.plist"
BOOT_NEUF="launchctl bootout system/fr.djoko.thread.route
$PRINT_NEUF"
INSTALL_D="install -d -m 1755 -o root -g wheel $PT"
MISE_EN_SERVICE="install -m 755 -o root -g wheel <programme compile> $PT/fr.djoko.thread.route
install -m 644 -o root -g wheel fr.djoko.thread.route.plist $LD/fr.djoko.thread.route.plist
launchctl bootstrap system $LD/fr.djoko.thread.route.plist"
MIGRATION="$BOOT_ANCIEN
$PRINT_ANCIEN
$RM_ANCIEN"
VERIFS=0
ECHECS=0
verifier() {
  VERIFS=$((VERIFS + 1))
  if [ "$2" != "$3" ]; then
    ECHECS=$((ECHECS + 1))
    printf 'ECHEC installation (%s) :\n%s\n-- attendu :\n%s\n' "$1" "$2" "$3"
  fi
}

# attendre_decharge (commun.sh), sourcee dans un shell neuf, sous launchctl et sleep simules.
# $1 : plan (0 ou 1), $2 : la racine (vide : la vraie), $3 : l'etiquette. Installation reelle :
# PLAN=0, RACINE vide, comme les vrais scripts.
decharge() {
  raz
  avec_faux sh -c '. ./commun.sh; PLAN=$1; RACINE=$2; attendre_decharge "$3"' x "$1" "$2" "$3" \
    > "$OUT/sortie" 2> "$OUT/erreur" && CODE=0 || CODE=$?
}
# Les quatre mesures d'un passage : code de sortie, launchctl print faits, sleep faits, sleep != 1.
mesures() { echo "code $CODE, $(relectures) relecture(s), $(dormis) sleep, $(grep -vc '^1$' "$OUT/dormis" || true) autre(s) duree(s)"; }
# 113 : inconnu de launchd, on passe aussitot (sans dormir, sans rien dire).
PRINT=113
decharge 0 "" demon
verifier "attente : inconnu de launchd, on passe" "$(mesures)" "code 0, 1 relecture(s), 0 sleep, 0 autre(s) duree(s)"
verifier "attente : inconnu, rien d'affiche" "$(cat "$OUT/sortie" "$OUT/erreur")" ""
# 0 un moment, puis 113 : on relit toutes les secondes, et on passe.
PRINT=0
PRINT_K=3
decharge 0 "" demon
verifier "attente : charge puis decharge apres 3 relectures" "$(mesures)" "code 0, 4 relecture(s), 3 sleep, 0 autre(s) duree(s)"
verifier "attente : un seul message d'attente" "$(grep -c "Attente de l'arret de demon" "$OUT/erreur")" 1
verifier "attente : pas de message d'arret" "$(grep -c 'Arreter le demon' "$OUT/erreur")" 0
# La borne : 25 relectures ; charge a la 25e, decharge a la 26e : on passe.
PRINT_K=25
decharge 0 "" demon
verifier "attente : decharge a la derniere relecture permise" "$(mesures)" "code 0, 26 relecture(s), 25 sleep, 0 autre(s) duree(s)"
# Toujours charge : arret, apres exactement 25 relectures d'une seconde, avec un message clair.
PRINT_K=26
decharge 0 "" demon
verifier "attente : toujours charge a la borne" "$(mesures)" "code 1, 26 relecture(s), 25 sleep, 0 autre(s) duree(s)"
verifier "attente : borne, message avec le temps attendu" "$(grep -c 'demon est encore charge par launchd apres 25 s d.attente' "$OUT/erreur")" 1
verifier "attente : borne, conseil de bootout" "$(grep -c 'sudo launchctl bootout system/demon' "$OUT/erreur")" 1
verifier "attente : borne, rien sur la sortie standard" "$(cat "$OUT/sortie")" ""
PRINT_K=
decharge 0 "" demon
verifier "attente : jamais decharge (aucune borne de compteur)" "$(mesures)" "code 1, 26 relecture(s), 25 sleep, 0 autre(s) duree(s)"
# Un code inconnu (ni 0, ni 113) : arret immediat, sans attendre ni relire, meme au-dela de 113.
for c in 1 112 114 150; do
  PRINT=$c
  decharge 0 "" demon
  verifier "attente : code $c, arret immediat" "$(mesures)" "code 1, 1 relecture(s), 0 sleep, 0 autre(s) duree(s)"
  verifier "attente : code $c, message" "$(grep -c "a repondu avec le code $c : impossible de savoir" "$OUT/erreur")" 1
done
# Un code inconnu pendant l'attente : arret immediat aussi.
PRINT=0
PRINT_K=2
PRINT_APRES=1
decharge 0 "" demon
verifier "attente : code inconnu apres deux relectures, arret" "$(mesures)" "code 1, 3 relecture(s), 2 sleep, 0 autre(s) duree(s)"
PRINT_K=
PRINT_APRES=113
# --plan sur une fausse racine : la ligne, une seule lecture, jamais d'attente.
LIGNE=$(ligne_print demon)
PRINT=0
decharge 1 /fausse demon
verifier "attente en plan : charge, arret sans attendre" "$(mesures)" "code 1, 1 relecture(s), 0 sleep, 0 autre(s) duree(s)"
verifier "attente en plan : la ligne est montree" "$(cat "$OUT/sortie")" "$LIGNE"
PRINT=113
decharge 1 /fausse demon
verifier "attente en plan : inconnu, on passe" "$(mesures)" "code 0, 1 relecture(s), 0 sleep, 0 autre(s) duree(s)"
verifier "attente en plan : inconnu, la ligne est montree" "$(cat "$OUT/sortie")" "$LIGNE"
# --plan sur la vraie racine : la ligne est montree, launchd n'est pas interroge (il n'a rien arrete).
PRINT=0
decharge 1 "" demon
verifier "attente en plan sur la vraie racine : pas de lecture" "$(mesures)" "code 0, 0 relecture(s), 0 sleep, 0 autre(s) duree(s)"
verifier "attente en plan sur la vraie racine : la ligne est montree" "$(cat "$OUT/sortie")" "$LIGNE"
# Installation reelle (PLAN=0, racine vide) : launchd est interroge, rien n'est affiche sur la
# sortie standard. Le cas PRINT=0 de plus haut est celui qui retient le garde en installation reelle.
PRINT=113

# Installation, en essai : l'ordre des actions. Le dossier des programmes n'est cree que s'il manque
# (le plan le dit comme l'installation le fait).
racine_neuve
rmdir "$PT"
verifier "sans ancien demon, dossier des programmes absent" "$(plan)" "$BOOT_NEUF
$INSTALL_D
$MISE_EN_SERVICE"
racine_neuve
touch "$PT/fr.djoko.thread.route" "$LD/fr.djoko.thread.route.plist"
verifier "mise a jour : dossier et nouveau demon deja la" "$(plan)" "$BOOT_NEUF
$MISE_EN_SERVICE"

# Migration : l'ancien demon (halo-routes) est arrete, verifie decharge, puis retire, avant que le
# nouveau soit pose.
racine_neuve
touch "$LD/fr.djoko.halo.routes.plist" "$PT/fr.djoko.halo.routes"
verifier "migration depuis halo-routes" "$(plan)" "$MIGRATION
$BOOT_NEUF
$MISE_EN_SERVICE"
rm "$LD/fr.djoko.halo.routes.plist"
verifier "ancien programme seul" "$(plan)" "$MIGRATION
$BOOT_NEUF
$MISE_EN_SERVICE"
rm "$PT/fr.djoko.halo.routes"
rmdir "$PT"
touch "$LD/fr.djoko.halo.routes.plist"
verifier "ancien plist seul, dossier des programmes absent" "$(plan)" "$MIGRATION
$BOOT_NEUF
$INSTALL_D
$MISE_EN_SERVICE"

# Mise a jour : le nouveau demon, encore charge apres son bootout, n'est pas remplace.
racine_neuve
touch "$PT/fr.djoko.thread.route" "$LD/fr.djoko.thread.route.plist"
PRINT=0
raz
sortie=$(plan 2> "$OUT/erreur") && code=0 || code=$?
PRINT=113
verifier "nouveau encore charge : code de sortie" "$code" 1
verifier "nouveau encore charge : arret avant le remplacement" "$sortie" "$BOOT_NEUF"
verifier "nouveau encore charge : message" "$(grep -c 'fr.djoko.thread.route est encore charge par launchd' "$OUT/erreur")" 1
verifier "nouveau encore charge : pas d'attente en plan" "$(dormis)" 0

# Migration : l'ancien demon est encore charge (ou launchd ne sait pas le dire) apres son bootout :
# arret, code 1, un message, et ni rm, ni install, ni bootstrap.
racine_neuve
touch "$LD/fr.djoko.halo.routes.plist" "$PT/fr.djoko.halo.routes"
PRINT=0
raz
sortie=$(plan 2> "$OUT/erreur") && code=0 || code=$?
verifier "ancien encore charge : code de sortie" "$code" 1
verifier "ancien encore charge : pas d'attente en plan" "$(dormis)" 0
verifier "ancien encore charge : arret avant tout retrait" "$sortie" "$BOOT_ANCIEN
$PRINT_ANCIEN"
verifier "ancien encore charge : message" "$(grep -c 'est encore charge par launchd' "$OUT/erreur")" 1
PRINT=1
sortie=$(plan 2> "$OUT/erreur") && code=0 || code=$?
verifier "etat de l'ancien inconnu : code de sortie" "$code" 1
verifier "etat de l'ancien inconnu : arret avant tout retrait" "$sortie" "$BOOT_ANCIEN
$PRINT_ANCIEN"
verifier "etat de l'ancien inconnu : message" "$(grep -c 'a repondu avec le code 1 ' "$OUT/erreur")" 1
PRINT=113
# Sur la vraie racine, --plan montre la verification sans l'interroger : launchd n'a rien arrete.
PRINT=0
avec_faux sh installer.sh --plan > /dev/null 2>&1 && code=0 || code=$?
verifier "plan sur la vraie racine : launchctl print n'est pas interroge" "$code" 0
PRINT=113
# Aucune commande simulee n'a ete appelee par les essais (ni sudo, ni clang).
verifier "essais : aucun sudo, aucun clang" "$(cat "$J")" ""

# Installation reelle, sous sudo, clang et launchctl simules : la compilation, les tests et l'essai
# precedent la premiere action root, un bootout qui echoue n'arrete rien, et la fausse racine est
# ignoree sans --plan.
lancer_installateur() {
  : > "$J"
  raz
  avec_faux THREAD_ROUTE_RACINE="$R" sh installer.sh > "$OUT/sortie" 2> "$OUT/erreur" && CODE=0 || CODE=$?
}
TETE="compile test_logique
lance test_logique
compile thread-route
lance thread-route"
BOOTOUT=3
lancer_installateur
BOOTOUT=0
verifier "installation reelle : sort sur 0 meme si un bootout echoue" "$CODE" 0
verifier "installation reelle : compilation, tests et essai avant la premiere action root" "$(head -4 "$J")" "$TETE"
verifier "installation reelle : ensuite, seulement des actions root" "$(tail -n +5 "$J" | grep -vc '^sudo ')" 0
verifier "installation reelle : finit par la mise en service" "$(tail -1 "$J")" "sudo launchctl bootstrap system /Library/LaunchDaemons/fr.djoko.thread.route.plist"
verifier "installation reelle : le programme est pose a sa place" "$(grep -c "^sudo install -m 755 -o root -g wheel .* /Library/PrivilegedHelperTools/fr.djoko.thread.route\$" "$J")" 1
verifier "installation reelle : fausse racine ignoree" "$(grep -cF "$R" "$J")" 0
verifier "installation reelle : fausse racine signalee" "$(grep -c 'THREAD_ROUTE_RACINE ignoree' "$OUT/erreur")" 1
verifier "installation reelle : launchd est interroge sur le nouveau, sans bruit" "$(grep -c 'launchctl print' "$OUT/sortie")" 0
# Un demon encore charge (apres 25 s d'attente) : arret, code 1, rien retire ni pose, que l'ancien
# soit la ou non (l'installation reelle lit la vraie racine) : le nouveau, lui, est toujours verifie.
PRINT=0
lancer_installateur
PRINT=113
verifier "installation reelle, demon encore charge : code de sortie" "$CODE" 1
verifier "installation reelle, demon encore charge : rien retire, rien pose" "$(grep -Ec '^sudo (rm|install|launchctl bootstrap)' "$J")" 0
verifier "installation reelle, demon encore charge : 25 relectures, 25 sleep" "$(relectures) $(dormis)" "26 25"
verifier "installation reelle, demon encore charge : message" "$(grep -c 'est encore charge par launchd apres 25 s' "$OUT/erreur")" 1
verifier "installation reelle, demon encore charge : l'installation n'est pas annoncee" "$(grep -c '^Installe' "$OUT/sortie")" 0
# Il se decharge au bout de quelques relectures : l'installation va jusqu'au bout.
PRINT=0
PRINT_K=2
lancer_installateur
PRINT=113
PRINT_K=
verifier "installation reelle, decharge apres attente : code de sortie" "$CODE" 0
verifier "installation reelle, decharge apres attente : finit par la mise en service" "$(tail -1 "$J")" "sudo launchctl bootstrap system /Library/LaunchDaemons/fr.djoko.thread.route.plist"
verifier "installation reelle, decharge apres attente : deux sleep" "$(dormis)" 2
# L'essai echoue : rien n'est installe, rien n'est demande a root.
ESSAI=1
lancer_installateur
ESSAI=0
verifier "essai en echec : code de sortie" "$CODE" 1
verifier "essai en echec : rien n'est demande a root" "$(cat "$J")" "$TETE"
verifier "essai en echec : message" "$(grep -c "L'essai a echoue" "$OUT/erreur")" 1

# Desinstallation, en essai : Thread Route, puis l'ancien s'il reste ; chacun est verifie decharge
# avant que ses fichiers soient retires.
racine_neuve
DESINSTALLATION="launchctl bootout system/fr.djoko.thread.route
$PRINT_NEUF
rm -f $PT/fr.djoko.thread.route $LD/fr.djoko.thread.route.plist
$BOOT_ANCIEN
$PRINT_ANCIEN
$RM_ANCIEN"
verifier "desinstallation, en essai" "$(avec_faux THREAD_ROUTE_RACINE="$R" sh desinstaller.sh --plan)" "$DESINSTALLATION"
verifier "desinstallation, en essai : aucun sudo" "$(cat "$J" | grep -c '^sudo ')" 0
PRINT=0
raz
sortie=$(avec_faux THREAD_ROUTE_RACINE="$R" sh desinstaller.sh --plan 2> "$OUT/erreur") && code=0 || code=$?
PRINT=113
verifier "desinstallation, en essai, encore charge : code de sortie" "$code" 1
verifier "desinstallation, en essai, encore charge : arret avant le rm, sans attente" "$sortie $(dormis)" "launchctl bootout system/fr.djoko.thread.route
$PRINT_NEUF 0"
# Desinstallation reelle, sous sudo, launchctl et sleep simules.
ACTIONS="sudo launchctl bootout system/fr.djoko.thread.route
sudo rm -f /Library/PrivilegedHelperTools/fr.djoko.thread.route /Library/LaunchDaemons/fr.djoko.thread.route.plist
sudo launchctl bootout system/fr.djoko.halo.routes
sudo rm -f /Library/PrivilegedHelperTools/fr.djoko.halo.routes /Library/LaunchDaemons/fr.djoko.halo.routes.plist"
desinstaller() {
  : > "$J"
  raz
  avec_faux THREAD_ROUTE_RACINE="$R" sh desinstaller.sh > "$OUT/sortie" 2> "$OUT/erreur" && CODE=0 || CODE=$?
}
desinstaller
verifier "desinstallation reelle : code de sortie" "$CODE" 0
verifier "desinstallation reelle (sudo simule) : les actions, la fausse racine ignoree" "$(cat "$J")" "$ACTIONS"
verifier "desinstallation reelle : launchd est interroge pour chaque demon" "$(relectures) $(dormis)" "2 0"
verifier "desinstallation : le journal de Thread Route est nomme" "$(grep -c '/Library/Logs/fr.djoko.thread.route.log' "$OUT/sortie")" 1
verifier "desinstallation : le journal de halo-routes est nomme" "$(grep -c '/Library/Logs/fr.djoko.halo.routes.log' "$OUT/sortie")" 1
# Un bootout qui echoue (rien n'est charge : le cas ordinaire de l'ancien) n'arrete rien.
BOOTOUT=3
desinstaller
BOOTOUT=0
verifier "desinstallation reelle, bootout en echec : code de sortie" "$CODE" 0
verifier "desinstallation reelle, bootout en echec : les actions, jusqu'au bout" "$(cat "$J")" "$ACTIONS"
verifier "desinstallation reelle, bootout en echec : annoncee" "$(grep -c '^Desinstalle' "$OUT/sortie")" 1
# Charge un moment apres le bootout : on attend, puis tout est retire.
PRINT=0
PRINT_K=2
desinstaller
PRINT=113
PRINT_K=
verifier "desinstallation reelle, decharge apres attente : code de sortie" "$CODE" 0
verifier "desinstallation reelle, decharge apres attente : les actions" "$(cat "$J")" "$ACTIONS"
verifier "desinstallation reelle, decharge apres attente : deux sleep" "$(dormis)" 2
# Encore charge apres 25 s : arret avant le rm, rien n'est annonce comme desinstalle.
PRINT=0
desinstaller
PRINT=113
verifier "desinstallation reelle, demon encore charge : code de sortie" "$CODE" 1
verifier "desinstallation reelle, demon encore charge : rien n'est retire" "$(cat "$J")" "sudo launchctl bootout system/fr.djoko.thread.route"
verifier "desinstallation reelle, demon encore charge : 25 relectures, 25 sleep" "$(relectures) $(dormis)" "26 25"
verifier "desinstallation reelle, demon encore charge : message" "$(grep -c 'est encore charge par launchd apres 25 s' "$OUT/erreur")" 1
verifier "desinstallation reelle, demon encore charge : pas de Desinstalle" "$(grep -c 'Desinstalle' "$OUT/sortie")" 0
echo "installation et desinstallation : $VERIFS verification(s), $ECHECS echec(s)"
[ "$ECHECS" -eq 0 ]

clang -std=c11 -O2 -Wall -Wextra -Werror -o "$OUT/thread-route" thread-route.c logique.c
"$OUT/thread-route" -n -1 -v
