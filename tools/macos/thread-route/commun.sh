# Fonctions partagees par installer.sh et desinstaller.sh, et sourcees par tests.sh (a sourcer, pas
# a lancer : `. ./commun.sh`). Elles lisent PLAN (0 ou 1) et RACINE (vide : la vraie racine ; une
# fausse racine n'existe qu'avec --plan, pour les tests).

# Nombre de relectures de l'etat d'un demon, une par seconde, avant de renoncer.
DECHARGE_RELECTURES=25

# Attend qu'un demon ait quitte launchd, avant que ses fichiers soient retires ou remplaces : sinon
# il resterait en marche, root, sans ses fichiers. A appeler apres le `bootout`, qui peut rendre la
# main avant la fin de l'arret du demon (il retire d'abord ses routes). $1 : l'etiquette.
# launchctl print (lecture seule, sans sudo) :
#   113 : inconnu de launchd, on passe aussitot ;
#   0   : encore charge, on relit toutes les secondes jusqu'a DECHARGE_RELECTURES fois, puis arret ;
#   autre code : on ne sait pas, arret immediat, sans relire.
# Un arret est un `exit 1`, avec un message : rien n'est retire, la relance reprend ou on en etait.
# Avec --plan, launchd n'a rien arrete et rien n'attend : la ligne est montree ; elle n'est evaluee
# (une seule fois) que sur une fausse racine, ou launchctl est simule par tests.sh. THREAD_ROUTE_RACINE
# hors de tests.sh interrogerait le vrai launchd : reservee aux tests.
attendre_decharge() {
  [ "$PLAN" -eq 0 ] || echo "launchctl print system/$1 [doit repondre introuvable (au plus $DECHARGE_RELECTURES s d'attente), sinon arret]"
  if [ "$PLAN" -eq 1 ] && [ -z "$RACINE" ]; then return 0; fi
  decharge_relues=0
  while :; do
    decharge_code=0
    launchctl print "system/$1" >/dev/null 2>&1 || decharge_code=$?
    if [ "$decharge_code" -eq 113 ]; then return 0; fi
    if [ "$decharge_code" -ne 0 ]; then
      echo "launchctl print system/$1 a repondu avec le code $decharge_code : impossible de savoir si $1 est arrete ; ses fichiers ne sont pas retires, rien d'autre n'est fait." >&2
      break
    fi
    if [ "$PLAN" -eq 1 ]; then
      echo "$1 est encore charge par launchd : ses fichiers ne sont pas retires, rien d'autre n'est fait." >&2
      break
    fi
    if [ "$decharge_relues" -ge "$DECHARGE_RELECTURES" ]; then
      echo "$1 est encore charge par launchd apres $DECHARGE_RELECTURES s d'attente : ses fichiers ne sont pas retires, rien d'autre n'est fait." >&2
      break
    fi
    [ "$decharge_relues" -gt 0 ] || echo "Attente de l'arret de $1 (au plus $DECHARGE_RELECTURES s)..." >&2
    decharge_relues=$((decharge_relues + 1))
    sleep 1
  done
  echo "Arreter le demon (sudo launchctl bootout system/$1), puis relancer le script." >&2
  exit 1
}
