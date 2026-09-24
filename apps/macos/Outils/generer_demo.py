#!/usr/bin/env python3
"""Genere la chronologie du mode demo de Halo Compagnon.

Sortie : HaloCompagnon/Ressources/demo-halo.jsonl (JSON-lines, ASCII).

Chaque ligne du fichier est l'une de :
  - une ligne machine de la carte (objet avec "v"), telle que le firmware
    l'emettrait entre RS et LF (docs/PROTOCOLE-JSON.md, section 2.2) ;
  - {"texte": "...", "ms": N} : une ligne de texte humain (annonce, log IDF) ;
  - {"demo": "<directive>", "ms": N, ...} : consigne pour le rejeu
    (lampe_debranchee, lampe_rebranchee, ligne_coupee, saut_n, entete).

Le point de depart reprend mot pour mot les exemples de la section 12.1 ;
la suite fait vivre la carte : Matter, molette de la telecommande, bouton A,
commande en echec (lampe debranchee), puce sourde et relance du module,
changement de role Thread, delais TX puis EN PANNE et retour a la normale.

Les blocs periodiques (etat, compteurs, reseau) ne sont ecrits que lorsqu'ils
changent : le rejeu les re-emet lui-meme a la cadence de la session.

Usage : python3 Outils/generer_demo.py   (depuis apps/macos)
"""

import json
import math
import os
import random

ICI = os.path.dirname(os.path.abspath(__file__))
SORTIE = os.path.join(ICI, "..", "HaloCompagnon", "Ressources", "demo-halo.jsonl")
SPEC = os.path.join(ICI, "..", "..", "..", "docs", "PROTOCOLE-JSON.md")

AIR = bytes([0x63, 0xFD, 0xF0, 0x4F])  # adresse sur l'air
BOOT = "3FA2C901"
T0 = 83512

random.seed(24092026)


# ---------------------------------------------------------------------------
#  Trame sur l'air (portage de halo1_proto.cpp : encodeAir, CRC-16/CCITT)
# ---------------------------------------------------------------------------

def _bits(data):
    for octet in data:
        for i in range(7, -1, -1):
            yield (octet >> i) & 1


def _crc_feed(crc, bit):
    crc ^= bit << 15
    return ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF


def encode_air(pid, no_ack, pay):
    bits = []
    ln = len(pay)
    bits += [(ln >> (5 - i)) & 1 for i in range(6)]
    bits += [(pid >> (1 - i)) & 1 for i in range(2)]
    bits += [1 if no_ack else 0]
    bits += list(_bits(pay))
    crc = 0xFFFF
    for b in _bits(AIR):
        crc = _crc_feed(crc, b)
    for b in bits:
        crc = _crc_feed(crc, b)
    bits += [(crc >> (15 - i)) & 1 for i in range(16)]
    bits += [0] * (64 - len(bits))
    raw = bytearray(8)
    for i, b in enumerate(bits[:64]):
        if b:
            raw[i >> 3] |= 0x80 >> (i & 7)
    return raw.hex().upper(), "%04X" % crc


# ---------------------------------------------------------------------------
#  Correspondances (halo1_map.cpp, gamma 2)
# ---------------------------------------------------------------------------

def _table(gamma=2.0, plancher=4):
    t = [0] * 255
    for l in range(1, 255):
        v = int(math.floor(178.0 * ((l - 1) / 253.0) ** gamma + 0.5))
        v = min(v, 178)
        if l <= plancher:
            v = 0
        t[l] = 0x4C + v
        if l > 1 and t[l] < t[l - 1]:
            t[l] = t[l - 1]
    t[0] = t[1]
    return t


TABLE = _table()


def raw_from_level(level):
    return TABLE[max(0, min(254, level))]


def level_from_raw(raw):
    lo, hi = 4, 254
    while lo < hi:
        mid = (lo + hi) // 2
        if TABLE[mid] >= raw:
            hi = mid
        else:
            lo = mid + 1
    return lo


def mired_from_temp(t):
    return 153 + (min(t, 100) * 217 + 50) // 100


def temp_from_mired(m):
    m = max(153, min(370, m))
    return ((m - 153) * 100 + 108) // 217


LAMP_BITS = {"avant": 0x40, "arriere": 0x01, "deux": 0x41}


def etat(marche, lampes, lum, temp):
    return {"marche": marche, "lampes": lampes, "lum": lum, "niveau": level_from_raw(lum),
            "temp": temp, "mired": mired_from_temp(temp)}


# ---------------------------------------------------------------------------
#  Carte simulee
# ---------------------------------------------------------------------------

class Carte:
    def __init__(self):
        self.ms = T0
        self.n = 0
        self.lignes = []
        self.derniers = {}
        self.pid = 1
        self.num_tx = 21
        # etat.lampe (12.1)
        self.consigne = etat(True, "deux", 165, 53)
        self.cru = dict(self.consigne)
        self.a_livrer = []
        self.version = 12
        self.phase = "repos"
        self.reprise_at = None
        self.echecs = 0
        self.lien = "ok"
        self.accuse_at = T0 + 3 - 41210  # accuse_ms 41210 dans le bloc lampe de 12.1
        self.dernier_a = 0
        self.a_entendus = 0
        self.livrees = 4
        self.abandons = 0
        self.tranches = []
        # etat.sante
        self.radio_mode = "ecoute"
        self.surveil = {"panne": False, "defaut": False, "symptome": None, "delais_suite": 0,
                        "fen_trames": 0, "fen_crc_faux": 0, "hors_rx_10s": 0, "sans_guerison": 0,
                        "attente_ms": 0, "relances": 0, "derniere": None}
        self.derniere_at = None
        self.led = "operationnel"
        self.matter_sante = {"en_service": True, "connecte": True, "identify": False}
        self.sys = {"heap": 112640, "heap_min": 86016, "heap_bloc": 45056, "pile_boucle": 4380,
                    "boucle_max_ms": 3, "json_perdus": 0, "json_trop_longs": 0, "rejets": 0}
        # compteurs (12.1)
        self.tx = {"consignes": 6, "paquets": 21, "accuses": 19, "ack_trame": 0, "max_rt": 2, "delais": 0,
                   "fifo": 0, "total": 21}
        self.tr = {"faibles": 0, "preemptees": 1, "annulees": 0, "reprises": 0, "abandons": 0, "attentes": 0}
        self.a = {"livres": 0, "refuses": 0}
        self.rx = {"trames": 148, "etat": 36, "a": 0, "accuses_lampe": 36, "service": 76, "favori": 0,
                   "invalides": 0, "crc_faux": 0}
        self.divers = {"sauvegardes": 3, "traces_perdues": 0, "relances_module": 0}
        self.radio = {"configs": 161, "reconf_silence": 139, "reconf_tx": 2, "verif_ratees": 0, "rearm": 560,
                      "rearm_hors_rx": 3, "brutes": 148, "bascules": 0}
        self.garde = {"active": True, "gardes": 21, "refus": 0, "attentes": 4, "plafonnees": 0, "max_us": 2380}
        self.relances = {"total": 0, "verif": 0, "delais": 0, "bruit": 0, "sourde": 0}
        self.cm = {"fenetres": 5, "ignorees": 1, "reflets": 11, "ecritures": 14, "echecs": 0, "verrou": 2,
                   "traces_perdues": 0, "identify": 0}
        # reseau (12.1)
        self.thread = {"role": "child", "canal": 25, "mhz": 2475, "pan": "0x1A2B", "tx_dbm": 20,
                       "parent_rssi": -48, "mode": "rn", "type_boot": "med_init", "type_suivant": "med_init",
                       "pret_ms": 21870, "roles": 2,
                       "mle": {"attaches": 1, "detache": 1, "enfant": 1, "routeur": 0, "chef": 0,
                               "parent_change": 0},
                       "srp": {"client": True, "hote": "Registered", "services": 1, "enregistres": 1,
                               "serveur": "fd8e:1c2a:44b0:1::1", "port": 53535}}
        self.matter_reseau = {"en_service": True, "connecte": True, "reseau": "thread", "wifi": False,
                              "fabriques": 1, "code_manuel": None, "qr": None}
        self.abo = {"actifs": 1, "lectures": 0, "sauves": 1, "demandes": 1, "neufs": 1, "repris_pont": 0,
                    "repris_pile": 0, "termines": 0, "plafond_s": 20, "plafonnes": 1, "reprise_auto": True}
        self.reprise = {"passages": 1, "auto": 1, "sessions": 0, "ouvertes": 0, "echecs": 0,
                        "sans_nouvelles": 0, "reprises": 0, "en_cours": False}
        self.totaux_abo = {"demandes": 1, "etablis": 1, "termines": 0, "passages": 1}

    # -- ecriture -------------------------------------------------------------

    def a_t(self, secondes):
        """Place l'horloge a T0 + secondes."""
        cible = T0 + int(round(secondes * 1000))
        assert cible >= self.ms, (secondes, self.ms)
        self.ms = cible

    def avance(self, ms):
        self.ms += ms

    def emet(self, t, champs, bloc=None):
        obj = {"v": 1, "t": t, "n": self.n, "ms": self.ms}
        if bloc:
            obj["bloc"] = bloc
        obj.update(champs)
        self.n += 1
        ligne = json.dumps(obj, separators=(",", ":"), ensure_ascii=True)
        assert all(0x20 <= ord(c) <= 0x7E for c in ligne), ligne
        assert len(ligne) + 2 <= 1024, (t, len(ligne))
        self.lignes.append(ligne)

    def texte(self, texte):
        self.lignes.append(json.dumps({"texte": texte, "ms": self.ms}, separators=(",", ":")))

    def directive(self, nom, **champs):
        obj = {"demo": nom, "ms": self.ms}
        obj.update(champs)
        self.lignes.append(json.dumps(obj, separators=(",", ":")))

    # -- blocs periodiques --------------------------------------------------------

    def up_s(self):
        return self.ms // 1000

    def blocs(self):
        s = dict(self.surveil)
        if self.derniere_at is not None:
            s["derniere"] = {"cause": self.surveil["derniere"], "il_y_a_s": (self.ms - self.derniere_at) // 1000}
        else:
            s["derniere"] = None
        return [
            ("etat", "lampe", {
                "boot": BOOT, "up_s": self.up_s(), "consigne": dict(self.consigne), "cru": dict(self.cru),
                "a_livrer": list(self.a_livrer), "confirme": ["marche", "lum", "temp"], "version": self.version,
                "phase": self.phase,
                "reprise_ms": None if self.reprise_at is None else max(0, self.reprise_at - self.ms),
                "echecs": self.echecs, "lien": self.lien,
                "accuse_ms": None if self.accuse_at is None else self.ms - self.accuse_at,
                "dernier_a": self.dernier_a, "a_entendus": self.a_entendus, "memoire": "deux",
                "livrees": self.livrees, "abandons": self.abandons, "sauve_attente": False, "ecoute": True,
                "trace": False}),
            ("etat", "tranches", {"boot": BOOT, "up_s": self.up_s(), "tranches": [dict(t) for t in self.tranches]}),
            ("etat", "sante", {
                "boot": BOOT, "up_s": self.up_s(),
                "radio": {"presente": True, "perdue": False, "mode": self.radio_mode, "configuree": True,
                          "quartz": True, "calib": True},
                "surveil": s, "led": {"motif": self.led, "test": False}, "matter": dict(self.matter_sante),
                "sys": dict(self.sys)}),
            ("compteurs", "pilote", {"raz": 0, "tx": dict(self.tx), "tranches": dict(self.tr), "a": dict(self.a),
                                     "rx": dict(self.rx), "divers": dict(self.divers)}),
            ("compteurs", "radio", {"raz": 0, "radio": dict(self.radio), "garde": dict(self.garde),
                                    "relances": dict(self.relances)}),
            ("compteurs", "matter", dict(self.cm)),
            ("reseau", "thread", {"frais_ms": 310, "matter": dict(self.matter_reseau),
                                  "thread": json.loads(json.dumps(self.thread))}),
            ("reseau", "abonnements", {"frais_ms": 1210, "abonnements": dict(self.abo),
                                       "reprise": dict(self.reprise)}),
        ]

    @staticmethod
    def _signature(champs):
        c = dict(champs)
        for cle in ("up_s", "accuse_ms", "reprise_ms"):
            c.pop(cle, None)
        if isinstance(c.get("surveil"), dict) and isinstance(c["surveil"].get("derniere"), dict):
            s = dict(c["surveil"])
            s["derniere"] = dict(s["derniere"])
            s["derniere"].pop("il_y_a_s", None)
            c["surveil"] = s
        return json.dumps(c, sort_keys=True)

    def instantane(self, force=False):
        """Ecrit les blocs qui ont change depuis leur derniere ecriture."""
        for t, bloc, champs in self.blocs():
            sig = self._signature(champs)
            if force or self.derniers.get((t, bloc)) != sig:
                self.derniers[(t, bloc)] = sig
                self.emet(t, champs, bloc)

    # -- evenements -----------------------------------------------------------

    def led_evt(self, motif):
        avant = self.led
        self.led = motif
        self.emet("led", {"motif": motif, "avant": avant, "test": False})

    def _rx(self, pay, no_ack=False, crc_faux=False, type_=None, sens=None, source="ecoute"):
        self.pid = (self.pid + 1) & 3
        brut, crc = encode_air(self.pid, no_ack, bytes(pay))
        if crc_faux:
            raw = bytearray.fromhex(brut)
            raw[3] ^= 0x10  # un bit retourne dans le CRC
            brut = raw.hex().upper()
            debut = 9 + 8 * len(pay)  # CRC lu tel quel apres la charge (decodeAir)
            valeur = int.from_bytes(raw, "big")
            crc = "%04X" % ((valeur >> (64 - debut - 16)) & 0xFFFF)
        champs = {"source": source, "brut": brut, "len": len(pay), "pid": self.pid, "no_ack": 1 if no_ack else 0,
                  "charge": bytes(pay).hex().upper(), "crc": crc, "crc_ok": not crc_faux, "type": type_,
                  "sens": sens}
        if crc_faux:
            champs["sautes"] = 0
        self.emet("rx", champs)
        self.rx["trames"] += 1
        self.radio["brutes"] += 1
        self.surveil["fen_trames"] += 1

    def accuse_lampe(self):
        self.avance(2)
        self._rx(b"", type_="accuse_lampe")
        self.rx["accuses_lampe"] += 1
        self.accuse_at = self.ms

    def service(self):
        self._rx([0xFF, 0x00], type_="service")
        self.rx["service"] += 1

    def molette_lum(self, lum):
        f = 0x80 | LAMP_BITS[self.consigne["lampes"]] | 0x04
        self._rx([f, lum], type_="lum", sens={"marche": True, "lampes": self.consigne["lampes"], "lum": lum})
        self.rx["etat"] += 1
        self.consigne = etat(True, self.consigne["lampes"], lum, self.consigne["temp"])
        self.cru = dict(self.consigne)
        self.version += 1
        self.lien = "ok"
        self.accuse_lampe()

    def molette_temp(self, temp):
        f = 0x80 | LAMP_BITS[self.consigne["lampes"]] | 0x02
        self._rx([f, temp], type_="temp", sens={"marche": True, "lampes": self.consigne["lampes"], "temp": temp})
        self.rx["etat"] += 1
        self.consigne = etat(True, self.consigne["lampes"], self.consigne["lum"], temp)
        self.cru = dict(self.consigne)
        self.version += 1
        self.accuse_lampe()

    def appui_a(self, numero):
        for copie in range(3):
            self._rx([0xE0, numero], type_="a", sens={"numero": numero, "copie": copie > 0})
            self.rx["a"] += 1
            if copie == 0:
                self.a_entendus += 1
                self.dernier_a = numero
            self.accuse_lampe()
            self.avance(98)

    def crc_faux(self, pay):
        self._rx(pay, crc_faux=True, type_="crc_faux")
        self.rx["crc_faux"] += 1
        self.surveil["fen_crc_faux"] += 1

    def _charge(self, tranche, e):
        f = (0x80 if e["marche"] else 0) | LAMP_BITS[e["lampes"]]
        if tranche == "lum":
            return "%02X%02X" % (f | 0x04, e["lum"])
        return "%02X%02X" % (f | 0x02, e["temp"])

    def _tx(self, tranche, charge, essai, accuses, verdict):
        us = {"ack": random.randint(1600, 1720), "max_rt": random.randint(11462, 11481),
              "delai": random.randint(30000, 30080)}[verdict]
        regs = {"ack": ("00", "2E", "11"), "max_rt": ("10", "1E", "01"), "delai": ("00", "0E", "0E")}[verdict]
        self.emet("tx", {"num": self.num_tx, "tranche": tranche, "charge": charge, "essai": essai, "paquets": 3,
                         "accuses": accuses, "verdict": verdict, "us": us, "rt2": regs[0], "irq1": regs[1],
                         "status": regs[2]})
        self.num_tx += 1
        self.tx["paquets"] += 1
        self.tx["total"] += 1
        self.garde["gardes"] += 1
        if verdict == "ack":
            self.tx["accuses"] += 1
        elif verdict == "max_rt":
            self.tx["max_rt"] += 1
        elif verdict == "delai":
            self.tx["delais"] += 1

    def rafale_ok(self, tranches, attente_debut):
        """Livre la consigne : 3 paquets accuses par tranche, puis livraison."""
        self.tx["consignes"] += 1
        self.phase = "rafale"
        debut = self.ms
        for tranche in tranches:
            charge = self._charge(tranche, self.consigne)
            self.tranches = [{"tranche": tranche, "charge": charge, "accuses": 0, "essais": 0, "paquets": 3}]
            self.avance(2)
            for essai in range(1, 4):
                self._tx(tranche, charge, essai, essai, "ack")
                self.accuse_at = self.ms
                if essai < 3:
                    self.avance(100)
        self.avance(2)
        self.cru = dict(self.consigne)
        self.a_livrer = []
        self.phase = "repos"
        self.tranches = []
        self.lien = "ok"
        self.livrees += 1
        self.emet("livraison", {"issue": "livree", "derniere": tranches[-1], "version": self.version,
                                "consigne": dict(self.consigne), "cru": dict(self.cru), "a_livrer": [],
                                "ids": [], "ids_perdus": 0, "attente_ms": self.ms - attente_debut,
                                "livrees": self.livrees, "abandons": self.abandons})
        self.avance(1)
        self.led_evt("livree")
        self.avance(150)
        self.led_evt("operationnel")
        return self.ms - debut

    def rafale_echec(self, tranche, attente_debut):
        """Lampe debranchee : 3 tours de 5 MAX_RT, reprises a +1 s et +2 s, abandon."""
        self.tx["consignes"] += 1
        charge = self._charge(tranche, self.consigne)
        for tour in range(3):
            self.phase = "rafale"
            self.tranches = [{"tranche": tranche, "charge": charge, "accuses": 0, "essais": 0, "paquets": 3}]
            self.instantane()
            for essai in range(1, 6):
                self.avance(2 if essai == 1 else 100)
                self._tx(tranche, charge, essai, 0, "max_rt")
                self.tranches[0]["essais"] = essai
            self.tr["faibles"] += 1
            if tour < 2:
                self.echecs = tour + 1
                self.tr["reprises"] += 1
                self.phase = "reprise"
                self.tranches = []
                self.reprise_at = self.ms + 1000 * (tour + 1)
                self.avance(20)
                self.instantane()
                self.ms = self.reprise_at
                self.reprise_at = None
        self.avance(40)
        # abandon : la consigne revient a l'etat cru
        self.consigne = dict(self.cru)
        self.version += 1
        self.a_livrer = []
        self.phase = "repos"
        self.tranches = []
        self.echecs = 0
        self.lien = "perdu"
        self.abandons += 1
        self.tr["abandons"] += 1
        self.emet("livraison", {"issue": "abandon", "cause": "injoignable", "version": self.version,
                                "consigne": dict(self.consigne), "cru": dict(self.cru), "a_livrer": [],
                                "ids": [], "ids_perdus": 0, "attente_ms": self.ms - attente_debut,
                                "livrees": self.livrees, "abandons": self.abandons})
        self.avance(1)
        self.led_evt("injoignable")
        self.texte("[lampe] injoignable : consigne abandonnee")
        self.avance(1200)
        self.led_evt("operationnel")

    def intent(self, recu, champs, nouvelle):
        self.cm["fenetres"] += 1
        self.cm["ecritures"] += 1
        self.cm["reflets"] += 1
        self.version += 1
        self.consigne = nouvelle
        self.a_livrer = list(champs)
        self.emet("intent", {"recu": recu, "fenetre_ms": random.randint(121, 140), "ignore": None,
                             "champs": list(champs), "consigne": dict(nouvelle), "version": self.version,
                             "a": None})

    def relance(self, cause, rang, detail, symptome_suivant=None):
        duree = random.randint(298, 330)
        self.avance(5)
        self.relances["total"] += 1
        self.relances[cause] += 1
        self.divers["relances_module"] += 1
        self.radio["configs"] += 1
        self.surveil["relances"] += 1
        self.surveil["derniere"] = cause
        self.derniere_at = self.ms
        self.surveil["sans_guerison"] = rang
        self.surveil["symptome"] = symptome_suivant
        self.surveil["attente_ms"] = 60000
        self.avance(duree)
        self.emet("relance", {"cause": cause, "rang": rang, "detail": detail, "ok": True, "quartz": True,
                              "calib": True, "duree_ms": duree, "total": self.relances["total"], "panne": False})
        self.sys["boucle_max_ms"] = duree + 2


def entete(c):
    c.directive("entete", titre="Halo Compagnon : demonstration",
                resume="Chronologie construite depuis les exemples de docs/PROTOCOLE-JSON.md (section 12)")


def scenario():
    c = Carte()
    entete(c)

    # 0 s : instantane de connexion (12.1), sans la reponse : le rejeu la fabrique avec l'id de l'app.
    hello_base = {"rev": 0, "fw": "0.4.0-1a2b3c4", "fw_desc": "0.4.0-1a2b3c4", "date": "Sep 24 2026",
                  "heure": "14:02:11", "env": "esp32c6thread", "build": "produit", "reseau_build": "thread",
                  "puce": "esp32c6", "idf": "v5.5.5", "arduino": "3.3.12", "boot": BOOT, "reset": "logiciel",
                  "reset_n": 3, "up_s": 83,
                  "session": {"transport": "usb", "periode_ms": 1000, "compteurs_ms": 1000, "reseau_ms": 5000,
                              "bail_s": 30, "trames": True, "log": False},
                  "limites": {"ligne_max": 1024, "cmd_max": 127}}
    c.emet("hello", hello_base, "base")
    c.avance(1)
    c.emet("hello", {"boot": BOOT, "mac": "F0F5BD012345",
                     "id": {"fabricant": "Djoko-CLI", "produit": "Pont ScreenBar Halo", "serie": "HALO1-F0F5BD012345",
                            "nom": "Halo", "hw": 1, "hw_txt": "ESP32-C6 SuperMini + BM5602"},
                     "caps": ["matter", "thread", "garde", "led", "lampe_async", "trames", "log"]}, "identite")
    c.avance(1)
    c.emet("config", {"lampe": {"adresse": "4FF0FD63", "air": "63FDF04F", "canal": 5, "debit_kbps": 125},
                      "reglages": {"paquets": 3, "accuses_min": 2, "paquets_max": 5, "ecart_ms": 100,
                                   "reprise_ms": 1000, "reprises": 2, "rearm_ms": 100, "silence_ms": 500,
                                   "rearm_fort": False, "leger": False, "garde": True, "gamma_c": 200},
                      "seuils": {"delais_suite": 3, "deluge_trames": 100, "deluge_pct": 90, "fenetre_ms": 10000,
                                 "sourd_hors_rx": 1000, "sans_guerison": 3, "ecart_ms": 60000,
                                 "repli_ms": 600000},
                      "matter": {"endpoints": {"principal": 1, "avant": 2, "arriere": 3},
                                 "lampes_en": "lumieres", "mired_min": 153, "mired_max": 370,
                                 "niveau_plancher": 4, "med": 1, "med_boot": 1, "maxint_s": 20,
                                 "reprise_auto": True}})
    for i in range(len(c.blocs())):
        c.avance(1)
        t, bloc, champs = c.blocs()[i]
        c.derniers[(t, bloc)] = Carte._signature(champs)
        c.emet(t, champs, bloc)

    def repos(jusqua):
        """Fait passer le temps seconde par seconde : derive lente des compteurs."""
        while c.ms + 1000 <= T0 + int(jusqua * 1000):
            c.avance(1000)
            c.radio["rearm"] += 7
            if (c.ms // 1000) % 2 == 0:
                c.radio["reconf_silence"] += 1
            if (c.ms // 1000) % 10 == 0:
                c.surveil["fen_trames"] = 0
                c.surveil["fen_crc_faux"] = 0
                c.sys["boucle_max_ms"] = random.choice([2, 3, 3, 4])
            if (c.ms // 1000) % 5 == 0:
                c.instantane()
        if c.ms < T0 + int(jusqua * 1000):
            c.a_t(jusqua)
        c.instantane()

    # 6 s : Apple Home regle la luminosite (niveau 127 -> lum 78), livree.
    repos(6)
    debut = c.ms
    c.intent({"ep1": True, "niveau": 127}, ["marche", "lum"], etat(True, "deux", 0x78, c.consigne["temp"]))
    c.texte("[matter] EP1 : niveau 127, marche")
    c.rafale_ok(["lum"], debut)
    c.instantane()

    # 14 s : la molette de la telecommande (reveil, puis ~9 trames par seconde).
    repos(14)
    c.service()
    c.avance(90)
    lum = 0x78
    montee = list(range(0x80, 0xFF, 8)) + [0xFE]
    descente = list(range(0xF0, 0x8F, -12))
    for i, v in enumerate(montee + descente):
        c.avance(random.randint(95, 125))
        c.molette_lum(v)
        if i in (6, 13):
            c.avance(7)
            c.crc_faux([0xC5, v])
        if (c.ms - T0) // 1000 != (c.ms - T0 - 120) // 1000:
            c.instantane()
        lum = v
    c.instantane()

    # 22 s : molette de temperature, plus chaude.
    repos(22)
    for v in range(56, 76, 3):
        c.avance(random.randint(100, 120))
        c.molette_temp(v)
    c.instantane()

    # 27 s : bouton A (trois copies du premier appui).
    repos(27)
    c.appui_a(1)
    c.instantane()

    # 33 s : Apple Home monte la luminosite (niveau 200 = lum BA) et la temperature.
    repos(33)
    debut = c.ms
    c.intent({"niveau": 200, "mireds": 268}, ["lum", "temp"], etat(True, "deux", 0xBA, 53))
    c.rafale_ok(["lum", "temp"], debut)
    c.instantane()

    # 40 s : la lampe est debranchee. Commande Matter en echec (12.4), abandon.
    repos(40)
    c.directive("lampe_debranchee")
    c.texte("[lampe] (demo) lampe debranchee : les commandes vont echouer")
    repos(44)
    debut = c.ms
    c.intent({"niveau": 138}, ["lum"], etat(True, "deux", 0x80, c.consigne["temp"]))
    c.rafale_echec("lum", debut)
    c.instantane()

    # 58 s : la lampe est rebranchee.
    repos(58)
    c.directive("lampe_rebranchee")
    c.texte("[lampe] (demo) lampe rebranchee")

    # 64 s : un log IDF coupe une ligne machine (2.1), puis deux lignes perdues.
    repos(64)
    c.texte("E (147210) chip[DL]: Long dispatch time: 212 ms, for event type 2")
    c.directive("ligne_coupee", log="E (147212) chip[EM]: Failed to send Standalone Ack")
    repos(66)
    c.directive("saut_n", nombre=2)

    # 68 s : Matter rallume la lampe au niveau 180, livree : lien retabli.
    repos(68)
    debut = c.ms
    c.intent({"ep1": True, "niveau": 180}, ["marche", "lum"], etat(True, "deux", 0xA5, c.consigne["temp"]))
    c.rafale_ok(["lum"], debut)
    c.instantane()

    # 74 s a 83 s : la puce devient sourde (rearmements hors RX), relance L2.
    repos(74)
    c.texte("[lampe] ecoute : rearmements hors RX en hausse")
    for s in range(75, 84):
        repos(s)
        c.radio["rearm_hors_rx"] += 134
        c.surveil["hors_rx_10s"] += 134
        c.surveil["symptome"] = "sourde" if c.surveil["hors_rx_10s"] >= 600 else None
        c.radio_mode = "ecoute"
        c.instantane()
    c.a_t(83.4)
    c.surveil["hors_rx_10s"] = 1204
    c.radio["rearm_hors_rx"] += 132
    c.instantane()
    c.texte("[lampe] puce sourde : relance du module")
    c.relance("sourde", 1, {"hors_rx": 1204, "ms": 2870})
    c.surveil["hors_rx_10s"] = 0
    c.instantane()
    repos(90)
    c.surveil["sans_guerison"] = 0
    c.surveil["attente_ms"] = 0
    c.instantane()

    # 96 s : le parent Thread disparait, l'abonnement tombe, puis tout revient.
    repos(96)
    c.a_t(96.08)
    c.emet("thread", {"de": "child", "vers": "detached", "a_ms": c.ms - 78, "total": 3})
    c.thread["role"] = "detached"
    c.thread["roles"] = 3
    c.thread["parent_rssi"] = None
    c.thread["mle"]["detache"] += 1
    c.matter_reseau["connecte"] = False
    c.matter_sante["connecte"] = False
    c.led_evt("hors_reseau")
    c.instantane()
    c.a_t(97.2)
    c.abo["actifs"] = 0
    c.abo["termines"] += 1
    c.totaux_abo["termines"] += 1
    c.emet("abonnement", {"quoi": "termine", "totaux": dict(c.totaux_abo)})
    repos(103)
    c.a_t(103.4)
    c.emet("thread", {"de": "detached", "vers": "child", "a_ms": c.ms - 49, "total": 4})
    c.thread["role"] = "child"
    c.thread["roles"] = 4
    c.thread["parent_rssi"] = -55
    c.thread["mle"]["enfant"] += 1
    c.thread["mle"]["attaches"] += 1
    c.thread["mle"]["parent_change"] += 1
    c.matter_reseau["connecte"] = True
    c.matter_sante["connecte"] = True
    c.led_evt("operationnel")
    c.instantane()
    c.a_t(104.9)
    c.abo["demandes"] += 1
    c.totaux_abo["demandes"] += 1
    c.emet("abonnement", {"quoi": "demande", "abonne": "0x000000000001B669", "plancher_s": 0, "max_s": 600,
                          "applique_s": 20, "totaux": dict(c.totaux_abo)})
    c.a_t(105.5)
    c.abo["actifs"] = 1
    c.abo["neufs"] += 1
    c.abo["plafonnes"] += 1
    c.totaux_abo["etablis"] += 1
    c.emet("abonnement", {"quoi": "etabli", "origine": "neuf", "min_s": 0, "max_s": 20,
                          "totaux": dict(c.totaux_abo)})
    c.instantane()
    repos(110)
    c.thread["parent_rssi"] = -51
    c.instantane()

    # 116 s a 150 s : delais TX a chaque commande, trois relances sans guerison, EN PANNE.
    for rang, t in enumerate((116, 128, 140), start=1):
        repos(t)
        debut = c.ms
        mired = [300, 250, 320][rang - 1]
        nouvelle = etat(True, "deux", c.consigne["lum"], temp_from_mired(mired))
        c.intent({"mireds": mired}, ["temp"], nouvelle)
        charge = c._charge("temp", nouvelle)
        c.phase = "rafale"
        for essai in range(1, 4):
            c.avance(2 if essai == 1 else 100)
            c._tx("temp", charge, essai, 0, "delai")
            c.surveil["delais_suite"] = essai
        c.surveil["symptome"] = "delais"
        c.instantane()
        c.texte("[lampe] 3 delais TX de suite : relance du module")
        c.relance("delais", rang, {"suite": 3}, symptome_suivant="delais" if rang == 3 else None)
        c.surveil["delais_suite"] = 0
        # apres la relance, la consigne passe
        c.phase = "repos"
        c.rafale_ok(["temp"], debut)
        c.instantane()

    repos(150)
    c.surveil["panne"] = True
    c.surveil["defaut"] = True
    c.surveil["attente_ms"] = 600000
    c.emet("module", {"etat": "panne", "sans_guerison": 3, "symptome": "delais", "essai_s": 600})
    c.texte("[lampe] module EN PANNE : 3 relances sans guerison (delais TX)")
    c.avance(2)
    c.led_evt("panne_radio")
    c.instantane()
    for s in range(151, 176, 5):
        repos(s)
        c.surveil["attente_ms"] = max(0, 600000 - (c.ms - (T0 + 150000)))
        c.instantane()

    # 176 s : fin de la panne (essai reussi), retour au vert.
    repos(176)
    c.surveil.update({"panne": False, "defaut": False, "symptome": None, "sans_guerison": 0, "attente_ms": 0})
    c.emet("module", {"etat": "retabli"})
    c.texte("[lampe] module retabli")
    c.avance(2)
    c.led_evt("operationnel")
    c.instantane()

    # 182 s : Apple Home eteint la lampe.
    repos(182)
    debut = c.ms
    c.intent({"ep1": False}, ["marche"], etat(False, "deux", c.consigne["lum"], c.consigne["temp"]))
    c.rafale_ok(["lum"], debut)
    c.instantane()

    repos(195)
    return c


def verifier_contre_la_spec(c):
    """Les 11 premieres lignes reprennent les exemples de 12.1 (hors n)."""
    exemples = []
    with open(SPEC, encoding="utf-8") as f:
        for ligne in f:
            if ligne.startswith("<RS>"):
                exemples.append(json.loads(ligne[4:]))
    machines = [json.loads(l) for l in c.lignes if '"v":1' in l[:8]]
    for attendu, obtenu in zip(exemples[:11], machines[:11]):
        for cle in ("n", "ms"):
            attendu.pop(cle, None)
            obtenu.pop(cle, None)
        assert attendu == obtenu, "ecart avec la spec :\n%s\n%s" % (attendu, obtenu)
    # Les trames de 12.3 : meme brut que encodeAir pour la meme charge et le meme pid.
    assert encode_air(1, False, bytes([0xC5, 0xA5])) == ("0962D2D86B000000", "B0D6")
    assert encode_air(0, False, bytes([0xFF, 0x00])) == ("087F8068D3800000", "D1A7")
    assert encode_air(3, False, bytes([0xC3, 0x35])) == ("0B619AA284800000", "4509")


def main():
    c = scenario()
    verifier_contre_la_spec(c)
    with open(SORTIE, "w", encoding="ascii", newline="\n") as f:
        for ligne in c.lignes:
            f.write(ligne + "\n")
    print("%d lignes, %d octets -> %s" % (len(c.lignes), os.path.getsize(SORTIE), os.path.relpath(SORTIE)))


if __name__ == "__main__":
    main()
