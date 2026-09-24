#pragma once
// ===========================================================================
//  Mode machine : protocole JSON du pont avec l'app compagnon
//  (docs/PROTOCOLE-JSON.md, v1, transport USB)
//
//  Session ('json 1', bail renouvele par 'json ping', 'json 0'), lignes
//  periodiques (etat, compteurs, reseau, battement) par la file des
//  periodiques, evenements du pilote, du pont Matter et de la LED, reponses
//  aux lignes portant un id, observateur de livraison.
//
//  Regles tenues ici (section 2.3) :
//  - un seul producteur, la tache loop ; un seul tampon de formatage, jamais
//    deux lignes en cours (une demande imbriquee est refusee et comptee) ;
//  - jamais d'attente : une ligne qui ne tient pas dans le tampon d'emission
//    de HWCDC est perdue et comptee (json_perdus), jamais ecrite a moitie ;
//  - lignes periodiques : une par tour de loop(), et seulement s'il reste
//    ensuite 1024 octets libres (place d'un evenement) ; perdues apres 500 ms
//    de retard ;
//  - reponses differees (apres un instantane, ou fin d'une commande
//    historique dont le texte a rempli le tampon) : par la meme file, jamais
//    perdues pour retard, parties des que 1024 octets sont libres ;
//  - rien n'est persiste : chaque demarrage repart en mode humain.
//
//  Le texte humain des commandes historiques reste ecrit comme avant, par
//  Serial (bloquant). Sans mode machine et sans id, rien ne change.
// ===========================================================================
#include <Arduino.h>

#include "json_out.h"

// Debut de setup(), avant toute radio : identifiant de ce demarrage (hello.boot).
void jsonBegin();
// Apres lamp.begin() : branche les crochets du pilote et de la LED d'etat.
void jsonAttach();
// A chaque tour de loop(), apres cliPoll() (et donc apres toute consigne de la
// CLI ou de Matter), avant statusLedPoll() : observateur de livraison, bail,
// lignes periodiques.
void jsonPoll();

uint32_t jsonBootId();
bool jsonMachine();  // mode machine en cours sur l'USB
void jsonNoteRx();   // un octet recu de l'hote (bail)

// --- CLI (cli.cpp) ----------------------------------------------------------

struct JsonCmd {
  bool hasId;
  uint32_t id;
  const char *cmd;  // commande sans le prefixe, tronquee (reponse.cmd)
  uint32_t t0;      // debut de l'execution (duree_ms)
};
// Famille 'json ...' ; avec id, la reponse part d'ici (immediate, ou apres les
// lignes d'un instantane).
void jsonCommand(char *arg, const JsonCmd &c);
// Au plus 20 lignes par seconde (mode machine, ou ligne avec id).
bool jsonCadenceOk(uint32_t now);
// Ligne refusee sans execution (code trop_long ou cadence) : reponse avec id,
// texte en mode humain sans id ; comptee (sante.sys.rejets).
void jsonRefuse(const JsonCmd &c, const char *code, const char *msg);
// Reponse immediate (evenement, non bloquante), en tout mode.
void jsonReply(const jsonp::Reply &r);
// Reponse fin d'une commande historique (sans msg) : tout de suite si une
// ligne entiere tient, sinon par la file des qu'elle tient (le texte bloquant
// de la commande a pu remplir le tampon d'emission de HWCDC).
void jsonReplyEnd(const jsonp::Reply &r);
// Fin d'une commande : bail, et config renvoyee si un reglage a change.
void jsonAfterCommand();
// Observateur de livraison tout de suite : une consigne finie avant une
// commande lampe asynchrone part avant que son id ne rejoigne la liste.
void jsonDeliveryFlush();
// id d'une commande lampe acceptee (code accepte) : porte par la livraison
// suivante, qui viendra toujours (annulee si la periode occupee finit sans
// compteur change). 8 au plus, les plus anciens sortent (ids_perdus).
void jsonPendingId(uint32_t id);

// --- Pont Matter (matter_bridge.cpp) -----------------------------------------

// Ouvre une ligne d'evenement dans le tampon unique : nullptr hors mode
// machine (ou ligne deja en cours). Sinon, la remplir puis jsonEventSend().
jsonp::Writer *jsonEventOpen(const char *type);
void jsonEventSend();
// Mode 'json log 1' : la ligne part en message log (src lampe|matter, niv
// notice|trace) ; true si elle est prise (emise, plafonnee ou perdue), false
// pour l'afficher en texte comme avant.
bool jsonLog(const char *src, const char *niv, const char *txt);
