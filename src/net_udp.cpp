#include "net_udp.h"

#if MATTER_NET_THREAD

#include <Arduino.h>
#include <Preferences.h>
#include <esp_openthread.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/srp_client.h>
#include <openthread/udp.h>
#include <string.h>

#include "cli.h"
#include "h1_proto.h"
#include "json_mode.h"
#include "matter_bridge.h"

// Une origine JSON par session etablie, apres l'USB (json_mode.cpp).
static_assert(jsonp::kOrigins == h1::kSlots + 1, "une origine JSON par session H1 etablie");

static const char *const kNvsNs = "halo1";
static const char *const kNvsKey = "cle";

// Commande de 127 octets et son en-tete de 56 : 256 laisse de la marge ; au-dela,
// le datagramme n'est pas pour nous (compte, jete).
static constexpr size_t kRxMax = 256;
static constexpr uint8_t kRxN = 4;
// En-tete H1 et ligne machine sans RS ni LF : 56 + 1022 octets.
static constexpr size_t kTxMax = h1::kHeaderMax + jsonp::kLineMax - 2;
static constexpr uint8_t kTxN = 6;
static constexpr uint32_t kTxStaleMs = 4000;   // pas parti dans ce delai : perdu
static constexpr uint8_t kTxPerTurn = 2;       // datagrammes remis a OpenThread par tour de loop()
// Debit moyen plafonne : chaque datagramme fait des trames 802.15.4 a quelques
// cm du BM5602 (et partage le canal avec Matter). 3000 octets/s, ~10 % du canal ;
// credit de 2400 octets (deux lignes d'un Ko d'affilee). Le profil distant en
// consomme ~0,8 Ko/s ; un instantane de 'json 1' (~6 Ko) part en ~1,5 s.
static constexpr uint32_t kTxBytesPerS = 3000;
static constexpr uint32_t kTxBurst = 2400;
// Tampons OpenThread (128 octets, 65 en tout, partages avec Matter) laisses
// libres apres un envoi : une ligne de ~1 Ko en prend ~11 jusqu'a son depart.
static constexpr uint16_t kBufReserve = 24;
static constexpr uint32_t kIpReadMs = 5000;    // adresses et nom SRP relus au plus toutes les 5 s
static constexpr uint32_t kExpireMs = 1000;
static constexpr uint32_t kOpenRetryMs = 1000;
static constexpr uint8_t kAddrMax = 4;

struct RxItem {
  uint16_t len;
  uint16_t port;
  uint8_t peer[16];
  uint8_t local[16];
  uint8_t data[kRxMax];
};

struct TxItem {
  uint32_t at;
  bool errCounted;  // tx_erreurs compte une fois par datagramme
  uint8_t slot;     // session qui l'a scelle, kRawSlot : DEFI (sans session)
  uint16_t len;
  uint16_t port;
  uint8_t peer[16];
  uint8_t local[16];
  uint8_t data[kTxMax];
};

static constexpr uint8_t kRawSlot = 0xFF;

static QueueHandle_t sRxQ = nullptr;
static RxItem sRxOt;    // tampon du rappel (tache OT seulement)
static RxItem sRxLoop;  // tampon de la tache loop
static TxItem sTx[kTxN];
static uint8_t sTxHead = 0, sTxN = 0;

static h1::Table sTable;
static otUdpSocket sSock;
static bool sOpen = false;
static uint32_t sOpenTryAt = 0, sExpireAt = 0;
static uint32_t sTxCredit = kTxBurst, sTxCreditAt = 0;

static struct {
  volatile uint32_t rxTooBig, rxQueueFull;  // ecrits par la tache OT
  uint32_t rxClosed;                        // lus socket ferme (cle effacee entre-temps)
  uint32_t rx, rejected, defis, tx, txLost, txErr;
  uint16_t bufMin;
} sSt = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFFFF};

enum : uint8_t { kAddrOmr, kAddrMlEid, kAddrOther };

static struct {
  bool known;
  uint32_t at;
  bool hostKnown;
  char host[64];
  uint8_t n;
  struct {
    uint8_t a[16];
    uint8_t kind;
    bool pref;
  } addr[kAddrMax];
  bool bufKnown;
  uint16_t bufFree;
} sIp = {};

// ===========================================================================
//  Reception (tache OT, verrou OT tenu)
// ===========================================================================

// Jamais de verrou CHIP ni de Serial ici : copie dans la file, rien d'autre.
static void onRx(void *, otMessage *m, const otMessageInfo *info) {
  const uint16_t off = otMessageGetOffset(m), total = otMessageGetLength(m);
  if (total < off || (size_t)(total - off) > kRxMax) {
    sSt.rxTooBig = sSt.rxTooBig + 1;
    return;
  }
  sRxOt.len = otMessageRead(m, off, sRxOt.data, (uint16_t)(total - off));
  sRxOt.port = info->mPeerPort;
  memcpy(sRxOt.peer, info->mPeerAddr.mFields.m8, 16);
  memcpy(sRxOt.local, info->mSockAddr.mFields.m8, 16);
  if (!sRxQ || xQueueSend(sRxQ, &sRxOt, 0) != pdTRUE) sSt.rxQueueFull = sSt.rxQueueFull + 1;
}

// ===========================================================================
//  File d'emission (tache loop)
// ===========================================================================

static TxItem *txTail() { return sTxN < kTxN ? &sTx[(sTxHead + sTxN) % kTxN] : nullptr; }

static void txPop() {
  if (!sTxN) return;
  sTxHead = (uint8_t)((sTxHead + 1) % kTxN);
  sTxN--;
}

static void txClear() {
  sSt.txLost += sTxN;
  sTxHead = sTxN = 0;
}

// Session partie (remplacee, oubliee) : ses datagrammes deja scelles ne
// prennent plus le debit de la suivante. Les autres gardent leur ordre.
static void txDropSlot(uint8_t slot) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < sTxN; i++) {
    const uint8_t from = (uint8_t)((sTxHead + i) % kTxN);
    if (sTx[from].slot == slot) {
      sSt.txLost++;
      continue;
    }
    const uint8_t to = (uint8_t)((sTxHead + kept) % kTxN);
    if (to != from) sTx[to] = sTx[from];
    kept++;
  }
  sTxN = kept;
}

// Datagramme sans session (DEFI) vers l'expediteur du SALUT : en tete de file,
// hors plafond de debit (82 octets, 2 par seconde au plus), pour qu'une
// poignee de main n'attende pas derriere l'instantane d'une autre session.
static void queueRaw(const h1::Peer &to, const uint8_t *d, size_t n, uint32_t now) {
  if (sTxN >= kTxN || n > kTxMax) {
    sSt.txLost++;
    return;
  }
  sTxHead = (uint8_t)((sTxHead + kTxN - 1) % kTxN);
  sTxN++;
  TxItem *t = &sTx[sTxHead];
  t->at = now;
  t->errCounted = false;
  t->slot = kRawSlot;
  t->len = (uint16_t)n;
  t->port = to.port;
  memcpy(t->peer, to.ip, 16);
  memcpy(t->local, to.local, 16);
  memcpy(t->data, d, n);
}

bool netUdpSend(uint8_t slot, const uint8_t *json, size_t len) {
  if (slot >= h1::kSlots || len > jsonp::kLineMax - 2) return false;
  const h1::Session &s = sTable.slot(slot);
  TxItem *t = txTail();
  if (!s.used || !t) return false;
  char hdr[h1::kHeaderMax + 1];
  const size_t hn = sTable.seal(slot, json, len, hdr);
  if (!hn) return false;
  t->at = millis();
  t->errCounted = false;
  t->slot = slot;
  t->len = (uint16_t)(hn + len);
  t->port = s.peer.port;
  memcpy(t->peer, s.peer.ip, 16);
  memcpy(t->local, s.peer.local, 16);
  memcpy(t->data, hdr, hn);
  memcpy(t->data + hn, json, len);
  sTxN++;
  return true;
}

uint8_t netUdpFreeSlots() { return sOpen ? (uint8_t)(kTxN - sTxN) : 0; }

// Remet a OpenThread les datagrammes en tete, sous verrou pris sans attente.
// Sous ce verrou : seulement des appels OpenThread.
static void txFlush() {
  // Heure lue ici, apres la mise en file : un datagramme date d'une
  // milliseconde plus tard que l'heure du debut de netUdpPoll() ne doit pas
  // paraitre vieux de 49 jours (comparaison signee).
  const uint32_t now = millis();
  while (sTxN && (int32_t)(now - sTx[sTxHead].at) > (int32_t)kTxStaleMs) {
    txPop();
    sSt.txLost++;
  }
  // Credit de debit (au plus 10 s rattrapees : pas de debordement du produit).
  const uint32_t elapsed = now - sTxCreditAt;
  sTxCreditAt = now;
  const uint32_t add = (elapsed > 10000 ? 10000 : elapsed) * kTxBytesPerS / 1000;
  sTxCredit = sTxCredit + add > kTxBurst ? kTxBurst : sTxCredit + add;
  if (!sTxN) return;
  if (!sOpen) {
    txClear();
    return;
  }
  auto paced = [](const TxItem &t) { return t.slot != kRawSlot && sTxCredit < t.len; };
  if (paced(sTx[sTxHead])) return;
  if (!matterOtTryLock(0)) return;
  otInstance *ot = esp_openthread_get_instance();
  for (uint8_t sent = 0; sTxN && sent < kTxPerTurn && !paced(sTx[sTxHead]); sent++) {
    TxItem &t = sTx[sTxHead];
    otBufferInfo bi;
    otMessageGetBufferInfo(ot, &bi);
    if (bi.mFreeBuffers != 0xFFFF) {
      if (bi.mFreeBuffers < sSt.bufMin) sSt.bufMin = bi.mFreeBuffers;
      // Premier tampon 76 octets utiles, les suivants 124 (firmware.elf) :
      // len / 100 + 2 couvre toujours 1 + (len - 20) / 124 arrondi.
      const uint16_t need = (uint16_t)(t.len / 100 + 2);
      if (bi.mFreeBuffers < need + kBufReserve) break;  // au tour suivant
    }
    // Priorite basse : devant un manque de tampons, OpenThread evince nos
    // messages avant ceux de Matter, jamais l'inverse.
    otMessageSettings ms;
    ms.mLinkSecurityEnabled = true;
    ms.mPriority = OT_MESSAGE_PRIORITY_LOW;
    otMessage *m = otUdpNewMessage(ot, &ms);
    if (!m || otMessageAppend(m, t.data, t.len) != OT_ERROR_NONE) {
      if (m) otMessageFree(m);
      if (!t.errCounted) sSt.txErr++;  // nouvel essai au tour suivant, jusqu'a kTxStaleMs
      t.errCounted = true;
      break;
    }
    otMessageInfo mi;
    memset(&mi, 0, sizeof(mi));
    memcpy(mi.mPeerAddr.mFields.m8, t.peer, 16);
    mi.mPeerPort = t.port;
    // Reponse depuis l'adresse que l'app a visee (une socket UDP connectee ne
    // garde que celle-la), si elle est encore a nous ; sinon OpenThread choisit.
    otIp6Address local;
    memcpy(local.mFields.m8, t.local, 16);
    if (!otIp6IsAddressUnspecified(&local) && otIp6HasUnicastAddress(ot, &local)) mi.mSockAddr = local;
    mi.mSockPort = kHaloUdpPort;
    if (otUdpSend(ot, &sSock, m, &mi) != OT_ERROR_NONE) {
      otMessageFree(m);  // refuse : il nous reste ; datagramme perdu
      if (!t.errCounted) sSt.txErr++;
      sSt.txLost++;
    } else {
      sSt.tx++;
      if (t.slot != kRawSlot) sTxCredit -= t.len;
    }
    txPop();
  }
  matterOtUnlock();
}

// ===========================================================================
//  Socket, adresses (tache loop, verrou OT sans attente)
// ===========================================================================

static void rxClear() {
  if (!sRxQ) return;
  sSt.rxClosed += (uint32_t)uxQueueMessagesWaiting(sRxQ);  // lus nulle part : comptes
  xQueueReset(sRxQ);
}

// Ouvert tant qu'une cle existe : sans cle, le port reste a lwIP, qui repond
// "port injoignable" comme a tout port ferme.
static void syncSocket(uint32_t now) {
  const bool want = sTable.hasKey();
  if (want == sOpen) return;
  if (want && sOpenTryAt && now - sOpenTryAt < kOpenRetryMs) return;
  sOpenTryAt = now ? now : 1;
  if (!matterOtTryLock(0)) return;
  otInstance *ot = esp_openthread_get_instance();
  if (want) {
    memset(&sSock, 0, sizeof(sSock));
    if (otUdpOpen(ot, &sSock, onRx, nullptr) == OT_ERROR_NONE) {
      otSockAddr a;
      memset(&a, 0, sizeof(a));
      a.mPort = kHaloUdpPort;
      // Sans UDP de plateforme dans ce build (aucun otPlatUdp* lie), l'interface
      // ne change rien ; interne si un jour il l'etait (pas de socket lwIP en double).
      // Un port tenu par OpenThread n'est plus remis a lwIP (Udp::IsPortInUse).
      if (otUdpBind(ot, &sSock, &a, OT_NETIF_THREAD_INTERNAL) == OT_ERROR_NONE) sOpen = true;
      else otUdpClose(ot, &sSock);
    }
  } else {
    otUdpClose(ot, &sSock);
    sOpen = false;
  }
  matterOtUnlock();
  if (!sOpen) {
    rxClear();
    txClear();
  }
}

static void readIp(uint32_t now) {
  if (sIp.known && now - sIp.at < kIpReadMs) return;
  if (!matterOtTryLock(0)) return;
  otInstance *ot = esp_openthread_get_instance();
  const otSrpClientHostInfo *h = otSrpClientGetHostInfo(ot);
  sIp.hostKnown = h && h->mName && h->mName[0];
  if (sIp.hostKnown) {
    strncpy(sIp.host, h->mName, sizeof(sIp.host) - 1);
    sIp.host[sizeof(sIp.host) - 1] = 0;
  }
  sIp.n = 0;
  for (const otNetifAddress *a = otIp6GetUnicastAddresses(ot); a && sIp.n < kAddrMax; a = a->mNext) {
    // Ni RLOC, ni ALOC, ni lien local : rien que le LAN puisse viser.
    if (!a->mValid || a->mRloc || otIp6IsLinkLocalUnicast(&a->mAddress)) continue;
    auto &e = sIp.addr[sIp.n++];
    memcpy(e.a, a->mAddress.mFields.m8, 16);
    e.kind = a->mMeshLocal ? kAddrMlEid : a->mAddressOrigin == OT_ADDRESS_ORIGIN_SLAAC ? kAddrOmr : kAddrOther;
    e.pref = a->mPreferred;
  }
  otBufferInfo bi;
  otMessageGetBufferInfo(ot, &bi);
  matterOtUnlock();
  sIp.bufKnown = bi.mFreeBuffers != 0xFFFF;
  sIp.bufFree = bi.mFreeBuffers;
  sIp.known = true;
  sIp.at = now;
}

// ===========================================================================
//  Datagrammes recus (tache loop)
// ===========================================================================

// Aleatoire de la plateforme pour nc et sid (tire seulement pour un SALUT
// admis). IDF ne garantit un vrai aleatoire qu'avec une radio Wi-Fi ou BT ;
// ici l'IEEE 802.15.4 : au pire un pseudo-aleatoire, suffisant pour des nonces
// qui ne doivent pas se repeter (la cle, elle, melange l'alea de l'app).
static void fillRandom(void *p, size_t n) { esp_fill_random(p, n); }

// Chaque session partie (oubliee, remplacee, cle changee) : son etat JSON repart de zero.
static void resetOrigins(uint8_t mask) {
  for (uint8_t i = 0; i < h1::kSlots; i++)
    if (mask & (1u << i)) {
      txDropSlot(i);
      jsonRemoteReset((uint8_t)(i + 1));
    }
}

static void handle(const RxItem &it, uint32_t now) {
  const h1::Parsed p = h1::parse(it.data, it.len);
  h1::Peer from;
  memcpy(from.ip, it.peer, 16);
  from.port = it.port;
  memcpy(from.local, it.local, 16);

  if (p.kind == h1::Kind::Salut) {
    // Sans place pour le DEFI, la poignee de main en cours (celle d'un autre
    // client peut-etre) n'est pas remplacee.
    if (sTxN >= kTxN) {
      sSt.rejected++;  // pas encore authentifie : ne compte pas comme datagramme perdu
      return;
    }
    char defi[h1::kDefiLen + 1];
    if (sTable.onSalut(p, fillRandom, from, now, defi) != h1::Verdict::Ok) {
      sSt.rejected++;
      return;
    }
    sSt.defis++;
    queueRaw(from, (const uint8_t *)defi, h1::kDefiLen, now);
    return;
  }

  uint8_t slot = 0;
  bool fresh = false;
  if (sTable.onData(p, from, now, &slot, &fresh) != h1::Verdict::Ok) {
    sSt.rejected++;  // forme, sid inconnu, MAC faux, rejeu : silence
    return;
  }
  sSt.rx++;
  const uint8_t origin = (uint8_t)(slot + 1);
  if (fresh) resetOrigins((uint8_t)(1u << slot));  // session neuve, ou a la place d'une autre
  jsonNoteRemoteRx(origin);
  // La charge est une ligne de la CLI : 127 octets au plus (au-dela, refusee
  // avec son id, trop_long), ASCII imprimable (tout autre octet devient '?',
  // comme dans les chaines emises).
  char line[jsonp::kCmdMax + 1];
  const bool tooLong = p.payloadLen > jsonp::kCmdMax;
  const size_t n = tooLong ? jsonp::kCmdMax : p.payloadLen;
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = p.payload[i];
    line[i] = c >= 0x20 && c <= 0x7E ? (char)c : '?';
  }
  line[n] = 0;
  cliRunRemote(origin, line, tooLong);
}

// ===========================================================================
//  Cle (NVS halo1/cle)
// ===========================================================================

static void loadKey() {
  Preferences p;
  if (!p.begin(kNvsNs, true)) return;
  uint8_t k[h1::kKeyLen];
  // isKey() d'abord : interroger une cle absente logue une erreur NVS.
  const bool ok = p.isKey(kNvsKey) && p.getBytes(kNvsKey, k, sizeof(k)) == sizeof(k);
  p.end();
  if (ok) sTable.setKey(k);
  h1::wipe(k, sizeof(k));
}

// Toutes les sessions tombent (nouvelle cle, cle effacee) : leurs datagrammes aussi.
static void dropAll() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < h1::kSlots; i++)
    if (sTable.slot(i).used) mask |= (uint8_t)(1u << i);
  resetOrigins(mask);
  txClear();
  rxClear();
}

NetKeyResult netUdpKeyNew(const uint8_t appRandom[32], char keyHex[65], char kid[9]) {
  uint8_t card[32], key[h1::kKeyLen];
  esp_fill_random(card, sizeof(card));
  const h1::Part part = {card, sizeof(card)};
  char newKid[h1::kKidHex + 1];
  const bool made = h1::hmacSha256(appRandom, 32, &part, 1, key) && h1::keyId(key, newKid);
  h1::wipe(card, sizeof(card));
  if (!made) {
    h1::wipe(key, sizeof(key));
    return NetKeyResult::Crypto;
  }
  Preferences p;
  const bool written = p.begin(kNvsNs, false) && p.putBytes(kNvsKey, key, sizeof(key)) == sizeof(key);
  p.end();
  if (!written) {  // NVS : ecriture atomique par entree, l'ancienne cle reste
    h1::wipe(key, sizeof(key));
    return NetKeyResult::Nvs;
  }
  dropAll();
  const bool loaded = sTable.setKey(key);
  h1::toHex(key, sizeof(key), keyHex);
  memcpy(kid, newKid, h1::kKidHex + 1);
  h1::wipe(key, sizeof(key));
  return loaded ? NetKeyResult::Ok : NetKeyResult::Load;
}

bool netUdpKeyErase() {
  Preferences p;
  bool ok = p.begin(kNvsNs, false);
  if (ok && p.isKey(kNvsKey)) ok = p.remove(kNvsKey);
  p.end();
  // Meme si la NVS refuse : plus de cle en memoire, plus de transport jusqu'au redemarrage.
  dropAll();
  sTable.setKey(nullptr);
  return ok;
}

bool netUdpKid(char kid[9]) {
  if (!sTable.hasKey()) return false;
  memcpy(kid, sTable.kid(), h1::kKidHex + 1);
  return true;
}

// ===========================================================================
//  Cycle de vie
// ===========================================================================

void netUdpBegin() {
  sRxQ = xQueueCreate(kRxN, sizeof(RxItem));
  loadKey();
}

void netUdpPoll() {
  const uint32_t now = millis();
  syncSocket(now);
  // Au plus kRxN datagrammes par tour : une commande de banc ne peut pas
  // arriver par ici (liste blanche), chaque ligne tient en quelques ms.
  for (uint8_t i = 0; i < kRxN && sRxQ && xQueueReceive(sRxQ, &sRxLoop, 0) == pdTRUE; i++) {
    if (sOpen) handle(sRxLoop, now);
    else sSt.rxClosed++;
  }
  if (now - sExpireAt >= kExpireMs) {
    sExpireAt = now;
    resetOrigins(sTable.expire(now));
  }
  txFlush();
  // Meme sans cle : l'app lit nom SRP et adresses par l'USB avant d'en regler une.
  readIp(now);
}

// ===========================================================================
//  JSON : bloc reseau ip
// ===========================================================================

void netUdpJson(jsonp::Writer &w, uint32_t now) {
  if (sIp.known) w.u32("frais_ms", now - sIp.at);
  else w.null("frais_ms");
  w.obj("srp");
  w.str("nom", sIp.known && sIp.hostKnown ? sIp.host : nullptr, 63);
  w.end();
  w.arr("adresses");
  for (uint8_t i = 0; sIp.known && i < sIp.n; i++) {
    static const char *const kKind[] = {"omr", "ml_eid", "autre"};
    char text[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6Address a;
    memcpy(a.mFields.m8, sIp.addr[i].a, 16);
    otIp6AddressToString(&a, text, sizeof(text));
    w.obj(nullptr);
    w.str("adr", text);
    w.str("type", kKind[sIp.addr[i].kind < 3 ? sIp.addr[i].kind : 2]);
    w.boolean("pref", sIp.addr[i].pref);
    w.end();
  }
  w.end();
  w.obj("udp");
  w.u32("port", kHaloUdpPort);
  w.boolean("ouvert", sOpen);
  char kid[9];
  const bool hasKey = netUdpKid(kid);
  w.str("empreinte", hasKey ? kid : nullptr);  // l'app verifie qu'elle a la bonne cle
  w.u32("sessions", sTable.established());
  w.boolean("provisoire", sTable.provisional().used);
  w.u32("rx", sSt.rx);
  w.u32("rejets", sSt.rejected);
  w.u32("rx_perdus", sSt.rxTooBig + sSt.rxQueueFull + sSt.rxClosed);
  w.u32("defis", sSt.defis);
  w.u32("tx", sSt.tx);
  w.u32("tx_perdus", sSt.txLost);
  w.u32("tx_erreurs", sSt.txErr);
  if (sIp.bufKnown) w.u32("tampons_libres", sIp.bufFree);
  else w.null("tampons_libres");
  if (sSt.bufMin != 0xFFFF) w.u32("tampons_min", sSt.bufMin);
  else w.null("tampons_min");
  w.end();
}

#endif  // MATTER_NET_THREAD
