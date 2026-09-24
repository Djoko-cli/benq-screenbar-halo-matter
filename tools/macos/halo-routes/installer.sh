#!/bin/sh
# Installe halo-routes : demon launchd (root) qui garde la route du reseau
# Thread sur ce Mac (README.md). A lancer sous son compte, sans sudo : le
# programme est compile et teste ici, seules la copie et la mise en service
# demandent le mot de passe administrateur.
#   sh tools/macos/halo-routes/installer.sh
set -eu
cd "$(dirname "$0")"
ETIQ=fr.djoko.halo.routes
BIN=/Library/PrivilegedHelperTools/$ETIQ
PLIST=/Library/LaunchDaemons/$ETIQ.plist
JOURNAL=/Library/Logs/$ETIQ.log

if [ "$(id -u)" -eq 0 ]; then
  echo "A lancer sans sudo (le mot de passe sera demande pour l'installation seulement)." >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "Compilation et tests..."
clang -std=c11 -Wall -Wextra -Werror -o "$TMP/test_logique" logique.c test_logique.c
"$TMP/test_logique"
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$TMP/halo-routes" halo-routes.c logique.c
echo "Ce que l'assistant ferait maintenant (essai, rien n'est change) :"
if ! "$TMP/halo-routes" -n -1 -v > "$TMP/essai.txt" 2>&1; then
  sed 's/^/  /' "$TMP/essai.txt"
  echo "L'essai a echoue : rien n'est installe." >&2
  exit 1
fi
sed 's/^/  /' "$TMP/essai.txt"

echo "Installation (mot de passe administrateur)..."
sudo launchctl bootout "system/$ETIQ" 2>/dev/null || true
[ -d /Library/PrivilegedHelperTools ] || sudo install -d -m 1755 -o root -g wheel /Library/PrivilegedHelperTools
sudo install -m 755 -o root -g wheel "$TMP/halo-routes" "$BIN"
sudo install -m 644 -o root -g wheel "$ETIQ.plist" "$PLIST"
sudo launchctl bootstrap system "$PLIST"
echo "Installe : $BIN (journal : $JOURNAL)"

# Une route posee a la main pour un prefixe annonce reste a son proprietaire :
# l'assistant ne la garde pas. La signaler.
# (prefixe ULA /64 via un routeur en lien local, statique, sans la marque 1 de l'assistant)
netstat -rn -f inet6 | awk '$1 ~ /^f[cd][0-9a-f][0-9a-f]:.*\/64$/ && $2 ~ /^fe80:/ && $3 ~ /S/ && $3 !~ /1/ {print $1, $2}' |
while read -r dst gw; do
  echo "Route posee a la main : $dst via $gw. Pour que l'assistant en prenne la garde :"
  echo "  sudo route -n delete -inet6 -prefixlen 64 ${dst%/64}"
done
