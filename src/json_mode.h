#pragma once
// ===========================================================================
//  Mode machine : protocole JSON du pont avec l'app compagnon
//  (docs/PROTOCOLE-JSON.md, v1 ; transports USB et, en build Thread, UDP)
//
//  Session ('json 1', bail renouvele par 'json ping', 'json 0'), lignes
//  periodiques (etat, compteurs, reseau, battement) par la file des
//  periodiques, evenements du pilote, du pont Matter et de la LED, reponses
//  aux lignes portant un id, observateur de livraison.
//
//  Une session par transport (origine, jsonp::kUsb puis une par session
//  reseau etablie, net_udp.cpp) : ses reglages, son n, sa file, ses pertes.
//  Les evenements partent vers chaque session en mode machine, formates pour
//  elle ; une reponse, vers l'origine de sa commande seulement.
//
//  Regles tenues ici (section 2.3) :
//  - un seul producteur, la tache loop ; un seul tampon de formatage, jamais
//    deux lignes en cours (une demande imbriquee est refusee et comptee) ;
//  - jamais d'attente : une ligne qui ne tient pas dans le tampon d'emission
//    de HWCDC est perdue et comptee (json_perdus), jamais ecrite a moitie ;
//  - lignes periodiques : une par tour de loop() et par transport, et
//    seulement s'il reste ensuite la place d'un evenement (1024 octets sur
//    l'USB, un datagramme sur le reseau) ; perdues apres 500 ms de retard
//    (3 s sur le reseau) ;
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
void jsonNoteRx();   // un octet recu de l'hote USB (bail)

// --- Transport reseau (net_udp.cpp, cli.cpp) -------------------------------

// Origine de la commande en cours : jsonp::kUsb hors cliRunRemote().
void jsonSetOrigin(uint8_t origin);
uint8_t jsonOrigin();
// Session reseau neuve dans cet emplacement (origine), ou partie (oubliee,
// remplacee, cle changee) : son etat de session repart de zero, sans rien
// emettre ; ses id en attente de livraison sortent.
void jsonRemoteReset(uint8_t origin);
void jsonNoteRemoteRx(uint8_t origin);  // message au MAC juste recu (bail)
// Origine reseau, ligne avec id, avant tout le reste (cadence comprise), shown
// etant la commande telle que la reponse la montre (reponse.cmd) :
//  - meme id, meme commande, reponse en cache : elle repart, rien n'est execute ;
//  - reponse differee de cet id encore en file : rien (elle partira) ;
//  - id deja traite (hors cache, ou autre commande) : reponse deja_traite ;
//  - sinon l'id est neuf (le plus haut id de la session avance) : false, a traiter.
// Les id croissent dans une session H1 ; seule une commande sans reponse est
// renvoyee avec le meme id.
bool jsonRemoteAdmit(uint32_t id, const char *shown);
// Ligne refusee sans reponse possible (reseau, sans id) : comptee (rejets).
void jsonCountRejected();

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
// Au plus 20 lignes par seconde et par transport (mode machine, ou ligne avec id).
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
// Mode 'json log 1' : la ligne part en message log (src lampe|matter|bouton, niv
// notice|trace) ; true si elle est prise (emise, plafonnee ou perdue), false
// pour l'afficher en texte comme avant. Plafond de 20 par seconde, sauf pour
// src bouton (quelques lignes par appui, jamais plafonnees).
bool jsonLog(const char *src, const char *niv, const char *txt);
