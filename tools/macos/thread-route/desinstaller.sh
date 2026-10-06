#!/bin/sh
# Desinstalle Thread Route (et halo-routes, son ancien nom, s'il reste). A l'arret, le demon retire
# les routes qu'il avait posees ; les routes du noyau et celles posees a la main restent.
#   sh desinstaller.sh
set -eu
for ETIQ in fr.djoko.thread.route fr.djoko.halo.routes; do
  sudo launchctl bootout "system/$ETIQ" 2>/dev/null || true
  sudo rm -f "/Library/PrivilegedHelperTools/$ETIQ" "/Library/LaunchDaemons/$ETIQ.plist"
done
echo "Desinstalle (journal garde : /Library/Logs/fr.djoko.thread.route.log)"
