#pragma once
// ===========================================================================
//  Transport reseau du protocole JSON (docs/PROTOCOLE-JSON.md, section 10)
//
//  Build Thread seulement (MATTER_NET_THREAD) : socket UDP d'OpenThread sur
//  un port fixe, enveloppe H1 (h1_proto.h), cle partagee en NVS (halo1/cle).
//  L'app (Mac, iPhone) joint le noeud par son adresse OMR, a travers les
//  routeurs de bordure Thread, sans rien demander a Matter.
//
//  Taches et verrous :
//  - reception : rappel d'OpenThread (tache OT, verrou OT tenu) ; il ne fait
//    que copier le datagramme dans une file FreeRTOS (4 places), jamais de
//    verrou CHIP, jamais de Serial ;
//  - tout le reste dans la tache loop : poignee de main, verification, commandes
//    (cli.cpp : cliRunRemote), formatage des lignes, HMAC ;
//  - emission : datagrammes fermes (en-tete H1 + JSON) dans une file de 6
//    places, remis a OpenThread sous verrou OT pris SANS attente (otLockTry(0)
//    de matter_bridge.cpp), verrou occupe : au tour suivant. Sous ce verrou,
//    seulement des appels OpenThread (regle de la garde d'antenne).
//  - debit plafonne (3000 octets/s, priorite basse), et un datagramme ne part
//    que s'il reste assez de tampons OpenThread (65, partages avec Matter)
//    apres lui ; le DEFI d'une poignee de main passe devant, hors plafond.
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#include "json_out.h"

// Port UDP fixe, sous la plage ephemere d'OpenThread (49152..65535).
constexpr uint16_t kHaloUdpPort = 5480;

#if MATTER_NET_THREAD

// setup(), apres matterBridgeBegin() : cle lue en NVS, file de reception.
void netUdpBegin();
// loop(), apres cliPoll() : socket (ouvert tant qu'une cle existe), datagrammes
// recus (poignees de main, commandes), oubli des sessions, emission.
void netUdpPoll();

// Ligne machine (objet JSON sans RS ni LF) pour la session etablie slot
// (0..h1::kSlots-1) : enveloppe H1 et mise en file. false : pas de session,
// ou file pleine (ligne perdue, comptee par l'appelant).
bool netUdpSend(uint8_t slot, const uint8_t *json, size_t len);
// Places libres dans la file d'emission (partagee par les sessions).
uint8_t netUdpFreeSlots();

// Bloc 'reseau' 'ip' (section 5.5) : nom d'hote SRP, adresses, transport UDP.
void netUdpJson(jsonp::Writer &w, uint32_t now);

// Cle (json cle, USB seulement). netUdpKeyNew : cle = HMAC-SHA256(alea de
// l'app, alea de la carte), ecrite en NVS, rendue une fois en hexa (keyHex,
// 64 + 1) avec son empreinte (kid, 8 + 1) ; toutes les sessions tombent.
enum class NetKeyResult : uint8_t {
  Ok,
  Crypto,  // cle non calculee : rien ne change
  Nvs,     // cle non ecrite : rien ne change
  Load,    // cle ecrite en NVS mais pas chargee : transport coupe jusqu'au redemarrage
};
NetKeyResult netUdpKeyNew(const uint8_t appRandom[32], char keyHex[65], char kid[9]);
// Efface la cle (NVS et memoire) : plus de transport reseau. Aussi a la remise
// a zero Matter (matterDecommissionNow). false : NVS en echec (la cle est
// quand meme retiree de la memoire).
bool netUdpKeyErase();
bool netUdpKid(char kid[9]);  // false : aucune cle

#endif
