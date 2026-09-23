# Pistes futures

Idees notees en cours de projet, hors du perimetre du produit actuel. Aucune n'est
commencee sauf mention contraire.

## Pour la communaute : un seul CC2500 pour decouvrir ET piloter (23/09/2026)

Le produit utilise un BM5602 (Holtek BC5602), qui fait en materiel tout le format
de trame de la lampe (adresse, en-tete de 9 bits, CRC-16/CCITT, accuse, renvois),
mais qui ne sait ecouter qu'une adresse connue. Chaque Halo 1 a sa propre adresse
de lien : un autre possesseur devrait refaire notre enquete pour la trouver.

Un **CC2500** (TI) saurait faire les deux :
- **decouvrir** l'adresse sans la connaitre : c'est exactement ce que nos outils
  `cctrig` + `tools/pairing/ana.py` ont fait (capture brute sur porteuse, puis
  recherche de trames au CRC juste a toute position) ;
- **piloter** la lampe en emulant le format BC5602 en logiciel : synchro sur
  16 bits d'adresse, reste de l'adresse, en-tete de 9 bits (decalage d'un bit),
  charge et CRC-16/CCITT (init FFFF) construits bit a bit ; accuse, PID et renvois
  geres par le firmware (bascule TX -> RX en ~21 us, l'accuse de la lampe arrive
  150-250 us apres la trame).

Precedent : le projet DIY Multiprotocol emule deja sur CC2500 les puces de la
famille nRF24 / XN297 (meme en-tete de 9 bits, meme principe d'accuse).

Atouts : un seul module pour tout, meilleure portee (le module 24TRGC5-V4 a un
PA/LNA RFX2402E), puce tres documentee. Couts : davantage de logiciel radio a
timing serre, module plus gros avec antenne u.FL. Ideal pour une version
« grand public » du projet.

## Telecommande Halo dans Apple Home

Exposer les boutons de la vraie telecommande (A, favori) comme **Generic Switch**
Matter (boutons sans etat) : Maison pourrait declencher des automatisations sur
un appui, comme avec un interrupteur Hue. Le pilote entend deja ces trames (A :
`E0/E1 nn` ; favori : salve contenant `91`/`89`).

## Retour rapide de « Halo auto » apres un appui dans l'app (en attente)

En attente tant qu'EP4 est desactive (`HALO1_EXPOSE_AUTO 0` depuis la 0.3.0,
decision du 23/09) : a reprendre s'il est remis. Maison garde l'etat demande ~10 s apres un appui dans l'app, alors que les
rapports de la carte s'affichent en 1 s. Chercher une astuce (tache separee
proposee le 23/09).

## Signature LED du produit (implementee le 23/09 : src/status_led.*, README « LED d'etat »)

WS2812 (IO8) : bleu clignotant = pas appaire ; orange lent = pas de Thread ;
eteinte + breve lueur blanche toutes les 10 s = OK ; flash vert = ordre envoye ;
rouge x3 = lampe injoignable ; « Identifier » depuis Maison = arc-en-ciel.

## Re-appairage de la lampe par l'ESP32

Rejouer la balise d'appairage (`59 01 00 B0`, canal 5, `5A 5A / F5 C3 / CF 49`,
accuse demande) pendant la fenetre d'appairage de la lampe (debranchee, capteur
couvert, rebranchee) : la lampe retrouverait l'adresse `63 FD F0 4F` sans la
telecommande.
