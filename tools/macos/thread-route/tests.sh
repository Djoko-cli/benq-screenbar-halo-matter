#!/bin/sh
# Tests de Thread Route sans rien installer : decision (test_logique.c), ordre de l'installation, de
# la migration depuis halo-routes et de la desinstallation (installer.sh et desinstaller.sh --plan,
# sur une fausse racine ; l'installation reelle sous sudo, clang et launchctl simules), puis
# compilation du demon et un passage en essai (-n : lit le noyau, ne change rien).
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
# launchctl print : 0 = charge, 113 = inconnu de launchd, comme le vrai.
cat > "$FAUX/launchctl" <<'FIN'
#!/bin/sh
if [ "$1" = "print" ]; then exit "${FAUX_PRINT:-113}"; fi
echo "launchctl $*" >> "$FAUX_JOURNAL"
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
chmod +x "$FAUX/sudo" "$FAUX/launchctl" "$FAUX/clang" "$FAUX/netstat"
for c in sudo launchctl clang netstat; do
  if [ "$(PATH="$FAUX:$PATH" sh -c "command -v $c")" != "$FAUX/$c" ]; then
    echo "$c simule n'est pas en tete du PATH : les tests de l'installateur ne sont pas lances." >&2
    exit 1
  fi
done
PRINT=113
BOOTOUT=0
ESSAI=0
# Lance une commande avec ces commandes simulees (et leurs reponses PRINT, BOOTOUT et ESSAI).
avec_faux() {
  env PATH="$FAUX:$PATH" FAUX_JOURNAL="$J" FAUX_PRINT="$PRINT" FAUX_BOOTOUT="$BOOTOUT" FAUX_ESSAI="$ESSAI" "$@"
}

R="$OUT/racine"
racine_neuve() {
  rm -rf "$R"
  mkdir -p "$R/Library/LaunchDaemons" "$R/Library/PrivilegedHelperTools"
}
plan() { avec_faux THREAD_ROUTE_RACINE="$R" sh installer.sh --plan; }
PT=$R/Library/PrivilegedHelperTools
LD=$R/Library/LaunchDaemons
BOOT_ANCIEN="launchctl bootout system/fr.djoko.halo.routes"
PRINT_ANCIEN="launchctl print system/fr.djoko.halo.routes [doit repondre introuvable, sinon arret]"
RM_ANCIEN="rm -f $PT/fr.djoko.halo.routes $LD/fr.djoko.halo.routes.plist"
BOOT_NEUF="launchctl bootout system/fr.djoko.thread.route"
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

# Migration : l'ancien demon est encore charge (ou launchd ne sait pas le dire) apres son bootout :
# arret, code 1, un message, et ni rm, ni install, ni bootstrap.
racine_neuve
touch "$LD/fr.djoko.halo.routes.plist" "$PT/fr.djoko.halo.routes"
PRINT=0
sortie=$(plan 2> "$OUT/erreur") && code=0 || code=$?
verifier "ancien encore charge : code de sortie" "$code" 1
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
# L'essai echoue : rien n'est installe, rien n'est demande a root.
ESSAI=1
lancer_installateur
ESSAI=0
verifier "essai en echec : code de sortie" "$CODE" 1
verifier "essai en echec : rien n'est demande a root" "$(cat "$J")" "$TETE"
verifier "essai en echec : message" "$(grep -c "L'essai a echoue" "$OUT/erreur")" 1

# Desinstallation, en essai : Thread Route, puis l'ancien s'il reste.
racine_neuve
verifier "desinstallation, en essai" "$(avec_faux THREAD_ROUTE_RACINE="$R" sh desinstaller.sh --plan)" "launchctl bootout system/fr.djoko.thread.route
rm -f $PT/fr.djoko.thread.route $LD/fr.djoko.thread.route.plist
launchctl bootout system/fr.djoko.halo.routes
rm -f $PT/fr.djoko.halo.routes $LD/fr.djoko.halo.routes.plist"
verifier "desinstallation, en essai : aucun sudo" "$(cat "$J" | grep -c '^sudo ')" 0
: > "$J"
avec_faux THREAD_ROUTE_RACINE="$R" sh desinstaller.sh > "$OUT/sortie" 2> "$OUT/erreur"
verifier "desinstallation reelle (sudo simule) : les actions, la fausse racine ignoree" "$(cat "$J")" "sudo launchctl bootout system/fr.djoko.thread.route
sudo rm -f /Library/PrivilegedHelperTools/fr.djoko.thread.route /Library/LaunchDaemons/fr.djoko.thread.route.plist
sudo launchctl bootout system/fr.djoko.halo.routes
sudo rm -f /Library/PrivilegedHelperTools/fr.djoko.halo.routes /Library/LaunchDaemons/fr.djoko.halo.routes.plist"
verifier "desinstallation : le journal de Thread Route est nomme" "$(grep -c '/Library/Logs/fr.djoko.thread.route.log' "$OUT/sortie")" 1
verifier "desinstallation : le journal de halo-routes est nomme" "$(grep -c '/Library/Logs/fr.djoko.halo.routes.log' "$OUT/sortie")" 1
echo "installation et desinstallation : $VERIFS verification(s), $ECHECS echec(s)"
[ "$ECHECS" -eq 0 ]

clang -std=c11 -O2 -Wall -Wextra -Werror -o "$OUT/thread-route" thread-route.c logique.c
"$OUT/thread-route" -n -1 -v
