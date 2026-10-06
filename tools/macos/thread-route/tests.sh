#!/bin/sh
# Tests de Thread Route sans rien installer : decision (test_logique.c), ordre de l'installation et
# de la migration depuis halo-routes (installer.sh --plan, sur une fausse racine), puis compilation
# du demon et un passage en essai (-n : lit le noyau, ne change rien).
#   sh tests.sh
set -eu
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -o "$OUT/test_logique" logique.c test_logique.c
"$OUT/test_logique"

# Installation : l'ancien demon (halo-routes) est arrete et retire avant que le nouveau soit pose.
R="$OUT/racine"
mkdir -p "$R/Library/LaunchDaemons" "$R/Library/PrivilegedHelperTools"
# Jamais une vraie installation depuis les tests : un installateur sans --plan n'est pas lance.
plan() {
  if grep -q -e '--plan' installer.sh; then THREAD_ROUTE_RACINE="$R" sh installer.sh --plan; else echo "installer.sh sans --plan"; fi
}
attendu_neuf="launchctl bootout system/fr.djoko.thread.route
install -d -m 1755 -o root -g wheel $R/Library/PrivilegedHelperTools
install -m 755 -o root -g wheel <programme compile> $R/Library/PrivilegedHelperTools/fr.djoko.thread.route
install -m 644 -o root -g wheel fr.djoko.thread.route.plist $R/Library/LaunchDaemons/fr.djoko.thread.route.plist
launchctl bootstrap system $R/Library/LaunchDaemons/fr.djoko.thread.route.plist"
attendu_migration="launchctl bootout system/fr.djoko.halo.routes
rm -f $R/Library/PrivilegedHelperTools/fr.djoko.halo.routes $R/Library/LaunchDaemons/fr.djoko.halo.routes.plist
$attendu_neuf"
VERIFS=0
ECHECS=0
verifier() {
  VERIFS=$((VERIFS + 1))
  if [ "$2" != "$3" ]; then
    ECHECS=$((ECHECS + 1))
    printf 'ECHEC installation (%s) :\n%s\n-- attendu :\n%s\n' "$1" "$2" "$3"
  fi
}
verifier "sans ancien demon" "$(plan)" "$attendu_neuf"
touch "$R/Library/LaunchDaemons/fr.djoko.halo.routes.plist" "$R/Library/PrivilegedHelperTools/fr.djoko.halo.routes"
verifier "migration depuis halo-routes" "$(plan)" "$attendu_migration"
rm "$R/Library/LaunchDaemons/fr.djoko.halo.routes.plist"
verifier "ancien programme seul" "$(plan)" "$attendu_migration"
rm "$R/Library/PrivilegedHelperTools/fr.djoko.halo.routes"
touch "$R/Library/LaunchDaemons/fr.djoko.halo.routes.plist"
verifier "ancien plist seul" "$(plan)" "$attendu_migration"
echo "installation : $VERIFS verification(s), $ECHECS echec(s)"
[ "$ECHECS" -eq 0 ]

clang -std=c11 -O2 -Wall -Wextra -Werror -o "$OUT/thread-route" thread-route.c logique.c
"$OUT/thread-route" -n -1 -v
