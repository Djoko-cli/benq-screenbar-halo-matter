#!/bin/sh
# Desinstalle Thread Route (et halo-routes, son ancien nom, s'il reste). A l'arret, le demon retire
# les routes qu'il avait posees ; les routes du noyau et celles posees a la main restent. Les
# journaux sont gardes.
#   sh desinstaller.sh
#   sh desinstaller.sh --plan   ne desinstalle rien : dit, dans l'ordre, ce que la desinstallation ferait
set -eu
PLAN=0
[ "${1:-}" = "--plan" ] && PLAN=1
# Pour les tests (tests.sh) : une fausse racine, avec --plan seulement.
RACINE=
if [ "$PLAN" -eq 1 ]; then
  RACINE=${THREAD_ROUTE_RACINE:-}
elif [ -n "${THREAD_ROUTE_RACINE:-}" ]; then
  echo "THREAD_ROUTE_RACINE ignoree : elle ne vaut qu'avec --plan." >&2
fi

# Une action d'administrateur : ecrite avec --plan, faite par sudo sinon.
faire() {
  if [ "$PLAN" -eq 1 ]; then echo "$*"; else sudo "$@"; fi
}

for ETIQ in fr.djoko.thread.route fr.djoko.halo.routes; do
  faire launchctl bootout "system/$ETIQ" 2>/dev/null || true
  faire rm -f "$RACINE/Library/PrivilegedHelperTools/$ETIQ" "$RACINE/Library/LaunchDaemons/$ETIQ.plist"
done
if [ "$PLAN" -eq 0 ]; then
  echo "Desinstalle (journaux gardes : /Library/Logs/fr.djoko.thread.route.log et, s'il existe, /Library/Logs/fr.djoko.halo.routes.log)"
fi
