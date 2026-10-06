# Notes de version · Release notes

Halo Compagnon : une section par version publiée, en français puis en anglais. `apps/macos/Outils/publier.sh` en
tire les notes de la version publiée sur GitHub et celles de la fenêtre de mise à jour.

Halo Compagnon: one section per published version, in French then in English. `apps/macos/Outils/publier.sh` takes
from it the notes of the GitHub release and those of the update window.

## 1.0.0

**Français**

- Première version publiée : l'app qui supervise le pont de la BenQ ScreenBar Halo 1, par l'USB ou par le réseau
  Thread (tableau de bord, trames en direct, graphiques, commandes et console), en français et en anglais, avec
  son mode démo.
- Mises à jour automatiques (Sparkle 2) : recherche au démarrage puis toutes les 24 heures, téléchargement et
  installation à la fermeture de l'app, ou tout de suite par « Installer et relancer ». « Rechercher les mises à
  jour… » est dans le menu Halo Compagnon ; Réglages, Général, « Mises à jour », permet de les arrêter.
- Thread Route, l'assistant système qui garde la route du Mac vers le réseau Thread (anciennement halo-routes) :
  son état est dans Réglages, Général ; il s'installe par `sh tools/macos/thread-route/installer.sh`.

**English**

- First published version: the app that supervises the BenQ ScreenBar Halo 1 bridge, over USB or over the Thread
  network (dashboard, live frames, charts, controls and console), in French and English, with its demo mode.
- Automatic updates (Sparkle 2): a check at launch and then every 24 hours, download, and installation when the
  app quits, or right away with "Install and Relaunch". "Check for Updates…" is in the Halo Compagnon menu;
  Settings, General, "Updates", can turn them off.
- Thread Route, the system helper that keeps the Mac's route to the Thread network (formerly halo-routes): its
  status is in Settings, General; it installs with `sh tools/macos/thread-route/installer.sh`.
