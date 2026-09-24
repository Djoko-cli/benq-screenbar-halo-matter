#!/bin/sh
# Desinstalle halo-routes. A l'arret, le demon retire les routes qu'il avait
# posees ; les routes du noyau et celles posees a la main restent.
#   sh tools/macos/halo-routes/desinstaller.sh
set -eu
ETIQ=fr.djoko.halo.routes
sudo launchctl bootout "system/$ETIQ" 2>/dev/null || true
sudo rm -f "/Library/PrivilegedHelperTools/$ETIQ" "/Library/LaunchDaemons/$ETIQ.plist"
echo "Desinstalle (journal garde : /Library/Logs/$ETIQ.log)"
