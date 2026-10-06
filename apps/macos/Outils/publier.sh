#!/bin/sh
# Publie une version de Halo Compagnon sur GitHub (spec du deploiement, section 3) : verifications, numeros,
# compilation Release signee par le certificat de Djoko, .dmg signe par Sparkle (cle du trousseau), controle
# d'anonymisation, version publiee (etiquette compagnon-vX.Y.Z, avec le .dmg), puis le flux des mises a jour
# (apps/macos/appcast.xml, qui garde toutes les versions) commite sur main et pousse aussitot, .dmg sur le Bureau.
# Le .dmg porte aussi la licence de Sparkle 2.10.0 (Outils/Sparkle-LICENSE.txt, le fichier LICENSE de l'etiquette
# 2.10.0, entier) ; le commit du flux est signe Djoko-cli, a l'adresse noreply de GitHub.
# La logique est dans Outils/publication.py, ses tests dans Outils/tests.
#   apps/macos/Outils/publier.sh X.Y.Z [--sans-bureau]
# La repetition, sans GitHub ni Bureau (spec, section 4), avec la cle du trousseau, ou une paire d'essai :
#   apps/macos/Outils/publier.sh X.Y.Z --repetition DOSSIER --url-base URL [--cle-privee FICHIER --cle-publique CLE]
#                                      [--trousseau TROUSSEAU] [--sans-tests]
# SPARKLE_BIN : le dossier bin de l'archive de Sparkle 2.10.0 (sign_update, generate_keys), obligatoire hors repetition.
# NOTARISER=1 (desactive par defaut) : notarisation du .dmg, avec PROFIL_NOTARISATION, le profil que
# notarytool store-credentials a range dans le trousseau ; il faut alors un Developer ID pour IDENTITE_SIGNATURE.
# Produits : apps/macos/build/publication/X.Y.Z/ ; compilation dans DD (par defaut, hors de ~/Documents :
# DerivedData/halo-compagnon-publication). Le numero de compilation compte les commits de tout le depot.
set -eu
cd "$(dirname "$0")/.."
# L'identite de signature de la version publiee, a ce seul endroit : le certificat auto-signe de Djoko, trouve par
# son nom dans le trousseau (les compilations de travail et les tests restent ad hoc).
IDENTITE_SIGNATURE=${IDENTITE_SIGNATURE:-Djoko-cli Code Signing}
DD=${DD:-$HOME/Library/Developer/Xcode/DerivedData/halo-compagnon-publication}
export DD
exec /usr/bin/python3 Outils/publication.py publier "$@" --identite "$IDENTITE_SIGNATURE" \
  --auteur Djoko-cli --etiquette compagnon-v --flux appcast.xml --licence Outils/Sparkle-LICENSE.txt \
  --nom-app "Halo Compagnon" --fichier Halo-Compagnon --depot-github Djoko-cli/benq-screenbar-halo-matter \
  --projet HaloCompagnon.xcodeproj --schema HaloCompagnon --cible HaloCompagnon \
  --test 'xcodegen generate --quiet && xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination platform=macOS -derivedDataPath "$DD" test' \
  --test 'xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination platform=macOS -derivedDataPath "$DD" -testLanguage en -testRegion US test' \
  --test '/usr/bin/python3 -m unittest discover -s Outils/tests' \
  --test 'cd ../.. && sh tools/test_halo1.sh && /usr/bin/python3 tools/test_halo_udp.py && sh tools/macos/thread-route/tests.sh' \
  --textes HaloCompagnon/Ressources/Localizable.xcstrings --textes HaloCompagnon/Ressources/InfoPlist.xcstrings \
  --textes HaloCompagnon/Ressources/Titres.xcstrings
