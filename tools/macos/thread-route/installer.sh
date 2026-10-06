#!/bin/sh
# Installe Thread Route : demon launchd (root) qui garde la route du reseau Thread sur ce Mac
# (README.md). A lancer sous son compte, sans sudo : le programme est compile et teste ici, seules
# la copie et la mise en service demandent le mot de passe administrateur.
#   sh installer.sh          (depuis ce dossier, ou avec son chemin)
#   sh installer.sh --plan   n'installe rien : dit, dans l'ordre, ce que l'installation ferait
# L'ancien demon, halo-routes (fr.djoko.halo.routes), est arrete et retire avant que Thread Route
# soit pose : les deux ne tournent jamais ensemble (il doit avoir quitte launchd avant que ses
# fichiers soient retires : attendre_decharge (commun.sh) attend jusqu'a 25 s, sinon l'installation
# s'arrete, rien n'etant retire ni pose). Il en va de meme du nouveau avant son remplacement. Le
# journal de l'ancien est garde.
set -eu
cd "$(dirname "$0")"
. ./commun.sh
ETIQ=fr.djoko.thread.route
ANCIEN=fr.djoko.halo.routes
PLAN=0
[ "${1:-}" = "--plan" ] && PLAN=1
# Pour les tests (tests.sh) : une fausse racine, avec --plan seulement. Une installation reelle vise
# toujours la vraie racine. Hors de tests.sh, un --plan sur une fausse racine interrogerait le vrai
# launchd (attendre_decharge) : cette variable n'est pas faite pour servir.
RACINE=
if [ "$PLAN" -eq 1 ]; then
  RACINE=${THREAD_ROUTE_RACINE:-}
elif [ -n "${THREAD_ROUTE_RACINE:-}" ]; then
  echo "THREAD_ROUTE_RACINE ignoree : elle ne vaut qu'avec --plan." >&2
fi
BIN=$RACINE/Library/PrivilegedHelperTools/$ETIQ
PLIST=$RACINE/Library/LaunchDaemons/$ETIQ.plist
JOURNAL=/Library/Logs/$ETIQ.log

# Une action d'administrateur : ecrite avec --plan, faite par sudo sinon.
faire() {
  if [ "$PLAN" -eq 1 ]; then echo "$*"; else sudo "$@"; fi
}

# L'installation, dans l'ordre. $1 : le programme compile.
installer() {
  if [ -e "$RACINE/Library/LaunchDaemons/$ANCIEN.plist" ] || [ -e "$RACINE/Library/PrivilegedHelperTools/$ANCIEN" ]; then
    faire launchctl bootout "system/$ANCIEN" 2>/dev/null || true
    attendre_decharge "$ANCIEN"
    faire rm -f "$RACINE/Library/PrivilegedHelperTools/$ANCIEN" "$RACINE/Library/LaunchDaemons/$ANCIEN.plist"
  fi
  faire launchctl bootout "system/$ETIQ" 2>/dev/null || true
  attendre_decharge "$ETIQ"
  [ -d "$RACINE/Library/PrivilegedHelperTools" ] ||
    faire install -d -m 1755 -o root -g wheel "$RACINE/Library/PrivilegedHelperTools"
  faire install -m 755 -o root -g wheel "$1" "$BIN"
  faire install -m 644 -o root -g wheel "$ETIQ.plist" "$PLIST"
  faire launchctl bootstrap system "$PLIST"
}

if [ "$PLAN" -eq 1 ]; then
  installer "<programme compile>"
  exit 0
fi

if [ "$(id -u)" -eq 0 ]; then
  echo "A lancer sans sudo (le mot de passe sera demande pour l'installation seulement)." >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "Compilation et tests..."
clang -std=c11 -Wall -Wextra -Werror -o "$TMP/test_logique" logique.c test_logique.c
"$TMP/test_logique"
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$TMP/thread-route" thread-route.c logique.c
echo "Ce que Thread Route ferait maintenant (essai, rien n'est change) :"
if ! "$TMP/thread-route" -n -1 -v > "$TMP/essai.txt" 2>&1; then
  sed 's/^/  /' "$TMP/essai.txt"
  echo "L'essai a echoue : rien n'est installe." >&2
  exit 1
fi
sed 's/^/  /' "$TMP/essai.txt"

echo "Installation (mot de passe administrateur)..."
installer "$TMP/thread-route"
echo "Installe : $BIN (journal : $JOURNAL)"

# Une route posee a la main pour un prefixe annonce reste a son proprietaire : Thread Route ne la
# garde pas. La signaler.
# (prefixe ULA /64 via un routeur en lien local, statique, sans la marque 1 de Thread Route)
netstat -rn -f inet6 | awk '$1 ~ /^f[cd][0-9a-f][0-9a-f]:.*\/64$/ && $2 ~ /^fe80:/ && $3 ~ /S/ && $3 !~ /1/ {print $1, $2}' |
while read -r dst gw; do
  echo "Route posee a la main : $dst via $gw. Pour que Thread Route en prenne la garde :"
  echo "  sudo route -n delete -inet6 -prefixlen 64 ${dst%/64}"
done
