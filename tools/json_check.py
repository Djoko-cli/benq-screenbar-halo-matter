#!/usr/bin/env python3
"""Verifie des lignes machine du pont Halo contre docs/PROTOCOLE-JSON.md (v1).

Entree : une capture brute du port serie (octets tels quels : texte humain,
logs, lignes machine RS + JSON + LF), par exemple un fichier
platformio-device-monitor-*.log, ou la sortie de 'cat /dev/cu.usbmodem*'.
Le decoupage suit la section 2.4 : dernier RS de chaque ligne, CR final
tolere, texte hors RS compte a part.

Pour chaque ligne machine :
  - tramage (2.2) : RS, '{"v":', ASCII imprimable, 1024 octets au plus RS et
    LF compris (au-dela de 896 : avertissement, budget de pire cas), forme
    compacte (aucun espace hors des chaines : la ligne doit etre identique a
    sa reecriture compacte) ;
  - enveloppe (4) : v, t, n, ms en tete et dans cet ordre, puis bloc pour
    hello, etat, compteurs, reseau ; entiers seulement, jamais de flottant ;
  - contenu (5 a 7) : champs obligatoires, types, bornes, enumerations,
    hexadecimal, tailles des chaines, coherences simples (etape et code
    d'une reponse, ids, adresse et adresse sur l'air...) ; un champ inconnu
    de la v1 est un avertissement (9.1 : un ajout garde v, l'app l'ignore) ;
  - continuite : trous de n (pertes), n qui recule (redemarrage).

Usage :
  python3 tools/json_check.py capture.log [autre.log ...]
  python3 tools/json_check.py --exemples docs/PROTOCOLE-JSON.md
  options : --strict (avertissements comptes comme erreurs), -q (resume seul),
            --independantes (lignes sans suite : pas de controle de n)

Code de sortie : 0 si aucune erreur, 1 sinon, 2 si l'entree est illisible.
"""
import argparse
import json
import re
import sys

sys.dont_write_bytecode = True

RS = 0x1E
LINE_MAX = 1024
BUDGET = 896
U32_MAX = 2**32 - 1

# ---------------------------------------------------------------------------
#  Petit langage de schema
# ---------------------------------------------------------------------------


class Int:
    def __init__(self, lo=0, hi=U32_MAX):
        self.lo, self.hi = lo, hi


class Bool:
    pass


class Str:
    def __init__(self, maxlen=255, pattern=None):
        self.maxlen, self.pattern = maxlen, re.compile(pattern) if pattern else None


class Enum:
    def __init__(self, *values):
        self.values = set(values)


class Hex:
    """Chaine hexadecimale majuscule sans 0x, de lo a hi octets."""

    def __init__(self, lo, hi=None):
        self.lo, self.hi = lo, hi if hi is not None else lo


class Obj:
    def __init__(self, fields, extra_ok=False):
        self.fields, self.extra_ok = fields, extra_ok


class Arr:
    def __init__(self, item, maxlen=None):
        self.item, self.maxlen = item, maxlen


class Null:
    """Valeur pouvant valoir null."""

    def __init__(self, spec):
        self.spec = spec


class Opt:
    """Champ pouvant etre absent."""

    def __init__(self, spec):
        self.spec = spec


U32 = Int()
U8 = Int(0, 255)
I32 = Int(-(2**31), 2**31 - 1)
BOOL = Bool()
BOOT = Hex(4)
NODE = Str(18, r"^0x[0-9A-F]{16}$")
LAMPS = Enum("avant", "arriere", "deux")
CODES = Arr(Enum("marche", "lum", "temp"), 3)
ETAT = Obj(
    {
        "marche": BOOL,
        "lampes": LAMPS,
        "lum": Int(0x4C, 0xFE),
        "niveau": Int(4, 254),
        "temp": Int(0, 100),
        "mired": Int(153, 370),
    }
)
LED = Enum(
    "identification",
    "desappairage",
    "redemarrage",
    "injoignable",
    "panne_radio",
    "livree",
    "non_appaire",
    "hors_reseau",
    "operationnel",
)
RELAUNCH = Enum("verif", "delais", "bruit", "sourde")
SLOT = Enum("lum", "temp", "a", "brut")
RESET = Enum(
    "mise_sous_tension", "broche", "logiciel", "panique", "chien_int", "chien_tache", "chien",
    "baisse_tension", "usb", "inconnue",
)
MED = Enum("routeur", "med_init", "med_tard")
OK_CODES = {"ok", "accepte", "differe", "en_cours", "execute"}
KO_CODES = {"usage", "refuse", "radio_absente", "radio_perdue", "inconnue", "trop_long", "cadence", "interdite",
            "deja_traite"}


def counters(*names):
    return Obj({n: U32 for n in names})


SCHEMAS = {
    ("hello", "base"): Obj(
        {
            "rev": U32,
            "fw": Str(64),
            "fw_desc": Str(32),
            "date": Str(16),
            "heure": Str(16),
            "env": Str(64),
            "build": Enum("produit", "diag"),
            "reseau_build": Enum("thread", "wifi", "aucun"),
            "puce": Str(32),
            "idf": Str(32),
            "arduino": Str(32),
            "boot": BOOT,
            "reset": RESET,
            "reset_n": U32,
            "up_s": U32,
            "session": Obj(
                {
                    "transport": Enum("usb", "udp"),
                    "periode_ms": U32,
                    "compteurs_ms": U32,
                    "reseau_ms": U32,
                    "bail_s": U32,
                    "trames": BOOL,
                    "log": BOOL,
                }
            ),
            "limites": Obj({"ligne_max": U32, "cmd_max": U32}),
        }
    ),
    ("hello", "identite"): Obj(
        {
            "boot": BOOT,
            "mac": Null(Hex(6)),
            "id": Obj(
                {
                    "fabricant": Str(32),
                    "produit": Str(32),
                    "serie": Null(Str(32)),
                    "nom": Str(32),
                    "hw": Int(0, 0xFFFF),
                    "hw_txt": Str(64),
                }
            ),
            "caps": Arr(Enum("matter", "thread", "garde", "ep4", "led", "lampe_async", "trames", "log", "udp", "cle")),
        }
    ),
    ("config", None): Obj(
        {
            "lampe": Obj({"adresse": Hex(4), "air": Hex(4), "canal": U8, "debit_kbps": U32}),
            "reglages": Obj(
                {
                    "paquets": U8,
                    "accuses_min": U8,
                    "paquets_max": U8,
                    "ecart_ms": U32,
                    "reprise_ms": U32,
                    "reprises": U8,
                    "rearm_ms": U32,
                    "silence_ms": U32,
                    "rearm_fort": BOOL,
                    "leger": BOOL,
                    "garde": Null(BOOL),
                    "gamma_c": U32,
                }
            ),
            "seuils": counters(
                "delais_suite", "deluge_trames", "deluge_pct", "fenetre_ms", "sourd_hors_rx", "sans_guerison",
                "ecart_ms", "repli_ms",
            ),
            "matter": Null(
                Obj(
                    {
                        "endpoints": Obj({"principal": U32, "avant": U32, "arriere": U32, "auto": Opt(U32)}),
                        "lampes_en": Enum("lumieres", "prises"),
                        "mired_min": U32,
                        "mired_max": U32,
                        "niveau_plancher": U32,
                        "impulsion_ms": Opt(U32),
                        "med": Null(Int(0, 2)),
                        "med_boot": Null(Int(0, 2)),
                        "maxint_s": Null(U32),
                        "reprise_auto": Null(BOOL),
                    }
                )
            ),
        }
    ),
    ("etat", "lampe"): Obj(
        {
            "boot": BOOT,
            "up_s": U32,
            "consigne": ETAT,
            "cru": ETAT,
            "a_livrer": CODES,
            "confirme": CODES,
            "version": U32,
            "phase": Enum("repos", "rafale", "reprise"),
            "reprise_ms": Null(U32),
            "echecs": U8,
            "lien": Enum("inconnu", "ok", "perdu"),
            "accuse_ms": Null(U32),
            "dernier_a": U8,
            "a_entendus": U32,
            "memoire": LAMPS,
            "livrees": U32,
            "abandons": U32,
            "sauve_attente": BOOL,
            "ecoute": BOOL,
            "trace": BOOL,
        }
    ),
    ("etat", "tranches"): Obj(
        {
            "boot": BOOT,
            "up_s": U32,
            "tranches": Arr(
                Obj({"tranche": SLOT, "charge": Hex(2), "accuses": U8, "essais": U8, "paquets": U8}),
                4,
            ),
        }
    ),
    ("etat", "sante"): Obj(
        {
            "boot": BOOT,
            "up_s": U32,
            "radio": Obj(
                {
                    "presente": BOOL,
                    "perdue": BOOL,
                    "mode": Enum("inconnu", "reset", "emission", "ecoute", "veille"),
                    "configuree": BOOL,
                    "quartz": Null(BOOL),
                    "calib": Null(BOOL),
                }
            ),
            "surveil": Obj(
                {
                    "panne": BOOL,
                    "defaut": BOOL,
                    "symptome": Null(RELAUNCH),
                    "delais_suite": U8,
                    "fen_trames": U32,
                    "fen_crc_faux": U32,
                    "hors_rx_10s": U32,
                    "sans_guerison": U8,
                    "attente_ms": U32,
                    "relances": U32,
                    "derniere": Null(Obj({"cause": Enum("verif", "delais", "bruit", "sourde", "l3"), "il_y_a_s": U32})),
                }
            ),
            "led": Obj({"motif": Null(LED), "test": Null(BOOL)}),
            "matter": Null(Obj({"en_service": BOOL, "connecte": BOOL, "identify": BOOL})),
            "sys": counters(
                "heap", "heap_min", "heap_bloc", "pile_boucle", "boucle_max_ms", "json_perdus", "json_trop_longs",
                "rejets",
            ),
        }
    ),
    ("compteurs", "pilote"): Obj(
        {
            "raz": U32,
            "tx": counters("consignes", "paquets", "accuses", "ack_trame", "max_rt", "delais", "fifo", "total"),
            "tranches": counters("faibles", "preemptees", "annulees", "reprises", "abandons", "attentes"),
            "a": counters("livres", "refuses"),
            "rx": counters("trames", "etat", "a", "accuses_lampe", "service", "favori", "invalides", "crc_faux"),
            "divers": counters("sauvegardes", "traces_perdues", "relances_module"),
        }
    ),
    ("compteurs", "radio"): Obj(
        {
            "raz": U32,
            "radio": counters(
                "configs", "reconf_silence", "reconf_tx", "verif_ratees", "rearm", "rearm_hors_rx", "brutes", "bascules"
            ),
            "garde": Null(
                Obj(
                    {
                        "active": BOOL,
                        "gardes": U32,
                        "refus": U32,
                        "attentes": U32,
                        "plafonnees": U32,
                        "max_us": U32,
                    }
                )
            ),
            "relances": counters("total", "verif", "delais", "bruit", "sourde"),
        }
    ),
    ("compteurs", "matter"): Obj(
        {
            "fenetres": U32,
            "ignorees": U32,
            "a_appuis": Opt(U32),
            "a_refuses": Opt(U32),
            "a_entendus": Opt(U32),
            "a_perdus": Opt(U32),
            "reflets": U32,
            "ecritures": U32,
            "echecs": U32,
            "verrou": U32,
            "traces_perdues": U32,
            "identify": U32,
        }
    ),
    ("reseau", "thread"): Obj(
        {
            "frais_ms": Null(U32),
            "matter": Obj(
                {
                    "en_service": BOOL,
                    "connecte": BOOL,
                    "reseau": Enum("thread", "wifi"),
                    "wifi": BOOL,
                    "fabriques": Null(U32),
                    "code_manuel": Null(Str(32)),
                    "qr": Null(Str(64, r"^MT:")),
                }
            ),
            "thread": Opt(
                Null(
                    Obj(
                        {
                            "role": Enum("disabled", "detached", "child", "router", "leader"),
                            "canal": U8,
                            "mhz": Null(U32),
                            "pan": Str(6, r"^0x[0-9A-F]{4}$"),
                            "tx_dbm": I32,
                            "parent_rssi": Null(I32),
                            "mode": Str(3, r"^(-|r?d?n?)$"),
                            "type_boot": MED,
                            "type_suivant": MED,
                            "pret_ms": Null(U32),
                            "roles": U32,
                            "mle": counters("attaches", "detache", "enfant", "routeur", "chef", "parent_change"),
                            "srp": Obj(
                                {
                                    "client": BOOL,
                                    "hote": Null(Str(16)),
                                    "services": U32,
                                    "enregistres": U32,
                                    "serveur": Null(Str(46)),
                                    "port": Null(Int(0, 65535)),
                                }
                            ),
                        }
                    )
                )
            ),
        }
    ),
    ("reseau", "ip"): Obj(
        {
            "frais_ms": Null(U32),
            "srp": Obj({"nom": Null(Str(63))}),
            "adresses": Arr(
                Obj({"adr": Str(45), "type": Enum("omr", "ml_eid", "autre"), "pref": BOOL}),
                4,
            ),
            "udp": Obj(
                {
                    "port": Int(1, 65535),
                    "ouvert": BOOL,
                    "empreinte": Null(Str(8, r"^[0-9A-F]{8}$")),
                    "sessions": Int(0, 2),
                    "provisoire": BOOL,
                    "rx": U32,
                    "rejets": U32,
                    "rx_perdus": U32,
                    "defis": U32,
                    "tx": U32,
                    "tx_perdus": U32,
                    "tx_erreurs": U32,
                    "tampons_libres": Null(U32),
                    "tampons_min": Null(U32),
                }
            ),
        }
    ),
    ("reseau", "abonnements"): Obj(
        {
            "frais_ms": Null(U32),
            "abonnements": Obj(
                {
                    "actifs": Null(U32),
                    "lectures": Null(U32),
                    "sauves": Null(U32),
                    "demandes": U32,
                    "neufs": U32,
                    "repris_pont": U32,
                    "repris_pile": U32,
                    "termines": U32,
                    "plafond_s": U32,
                    "plafonnes": U32,
                    "reprise_auto": BOOL,
                }
            ),
            "reprise": Obj(
                {
                    "passages": U32,
                    "auto": U32,
                    "sessions": U32,
                    "ouvertes": U32,
                    "echecs": U32,
                    "sans_nouvelles": U32,
                    "reprises": U32,
                    "en_cours": BOOL,
                }
            ),
        }
    ),
    ("hb", None): Obj({"boot": BOOT, "up_s": U32, "json_perdus": U32}),
    ("fin", None): Obj({"cause": Enum("commande", "bail")}),
    ("reponse", None): Obj(
        {
            "id": Int(1, 999999999),
            "etape": Enum("debut", "fin"),
            "cmd": Str(40),
            "ok": BOOL,
            "code": Enum(*(OK_CODES | KO_CODES)),
            "msg": Opt(Str(120)),
            "duree_ms": Opt(U32),
            "suite": Opt(Enum("livraison", "aucune")),
            "consigne": Opt(ETAT),
            "a_livrer": Opt(CODES),
            "version": Opt(U32),
            "bail_s": Opt(U32),
            "up_s": Opt(U32),
            # 'json cle' (USB, rev 2) : cle rendue une seule fois, empreinte (null sans cle).
            "cle": Opt(Hex(32)),
            "empreinte": Opt(Null(Str(8, r"^[0-9A-F]{8}$"))),
        }
    ),
    ("rx", None): Obj(
        {
            "source": Enum("ecoute", "accuse"),
            "brut": Null(Hex(8)),
            "len": Int(0, 63),
            "pid": Null(Int(0, 3)),
            "no_ack": Null(Int(0, 1)),
            "charge": Hex(0, 4),
            "crc": Null(Hex(2)),
            "crc_ok": BOOL,
            "type": Enum("lum", "temp", "a", "accuse_lampe", "service", "favori", "invalide", "crc_faux"),
            "sens": Null(
                Obj(
                    {
                        "marche": Opt(BOOL),
                        "lampes": Opt(Enum("avant", "arriere", "deux", "aucune")),
                        "lum": Opt(U8),
                        "temp": Opt(U8),
                        "numero": Opt(U8),
                        "copie": Opt(BOOL),
                    }
                )
            ),
            "sautes": Opt(U32),
        }
    ),
    ("tx", None): Obj(
        {
            "num": U32,
            "tranche": SLOT,
            "charge": Hex(2),
            "essai": U8,
            "paquets": U8,
            "accuses": U8,
            "verdict": Enum("ack", "ack_trame", "max_rt", "delai", "fifo"),
            "us": Int(0, 65535),
            "rt2": Hex(1),
            "irq1": Hex(1),
            "status": Hex(1),
            "sautes": Opt(U32),
        }
    ),
    ("livraison", None): Obj(
        {
            "issue": Enum("livree", "abandon", "annulee"),
            "cause": Opt(Enum("injoignable", "module")),
            "derniere": Opt(Null(Enum("lum", "temp", "a"))),
            "version": U32,
            "consigne": ETAT,
            "cru": ETAT,
            "a_livrer": CODES,
            "ids": Arr(Int(1, 999999999), 8),
            "ids_perdus": U32,
            "attente_ms": Null(U32),
            "livrees": U32,
            "abandons": U32,
        }
    ),
    ("relance", None): Obj(
        {
            "cause": Enum("verif", "delais", "bruit", "sourde", "l3"),
            "rang": Null(U32),
            "detail": Obj(
                {
                    "suite": Opt(U32),
                    "trames": Opt(U32),
                    "crc_faux": Opt(U32),
                    "ms": Opt(U32),
                    "hors_rx": Opt(U32),
                    "verif_ratees": Opt(U32),
                }
            ),
            "ok": BOOL,
            "quartz": Null(BOOL),
            "calib": Null(BOOL),
            "duree_ms": U32,
            "total": U32,
            "panne": BOOL,
        }
    ),
    ("module", None): Obj(
        {
            "etat": Enum("panne", "retabli", "perdu", "retrouve", "config_rejetee", "config_verifiee"),
            "sans_guerison": Opt(U32),
            "symptome": Opt(Null(RELAUNCH)),
            "essai_s": Opt(U32),
            "rfch": Opt(Null(Hex(1))),
            "dm1": Opt(Null(Hex(1))),
            "rt1": Opt(Null(Hex(1))),
        }
    ),
    ("intent", None): Obj(
        {
            "recu": Obj(
                {
                    "ep1": Opt(BOOL),
                    "niveau": Opt(Int(0, 254)),
                    "mireds": Opt(Int(0, 65535)),
                    "avant": Opt(BOOL),
                    "arriere": Opt(BOOL),
                    "a": Opt(BOOL),
                }
            ),
            "fenetre_ms": U32,
            "ignore": Null(Enum("demarrage")),
            "champs": Opt(CODES),
            "consigne": Opt(ETAT),
            "version": Opt(U32),
            "a": Opt(Null(Enum("appui", "ignore", "refuse"))),
        }
    ),
    ("abonnement", None): Obj(
        {
            "quoi": Enum("demande", "etabli", "termine", "reprise", "session", "reprise_abonne"),
            "abonne": Opt(NODE),
            "plancher_s": Opt(U32),
            "max_s": Opt(U32),
            "applique_s": Opt(U32),
            "origine": Opt(Enum("neuf", "pont", "pile")),
            "min_s": Opt(U32),
            "mode": Opt(Enum("auto", "manuelle")),
            "verdict": Opt(
                Enum(
                    "lance", "rien", "sans_stockage", "iterateur_occupe", "repris", "deja_servi", "file_pleine"
                )
            ),
            "sauves": Opt(U32),
            "abonnes": Opt(U32),
            "lances": Opt(U32),
            "servis": Opt(U32),
            "en_cours": Opt(U32),
            "ok": Opt(BOOL),
            "erreur": Opt(Null(Str(18, r"^0x[0-9A-F]+$"))),
            "duree_ms": Opt(U32),
            "repris": Opt(U32),
            "sans_readhandler": Opt(U32),
            "rates": Opt(U32),
            "totaux": counters("demandes", "etablis", "termines", "passages"),
        }
    ),
    ("thread", None): Obj(
        {
            "de": Enum("disabled", "detached", "child", "router", "leader"),
            "vers": Enum("disabled", "detached", "child", "router", "leader"),
            "a_ms": U32,
            "total": U32,
        }
    ),
    ("led", None): Obj({"motif": LED, "avant": LED, "test": BOOL}),
    ("log", None): Obj(
        {
            "src": Enum("lampe", "matter", "bouton"),
            "niv": Enum("notice", "trace"),
            "txt": Str(191),
            "sautes": Opt(U32),
        }
    ),
}

BLOCKED = {"hello", "etat", "compteurs", "reseau"}
BLOCS = {t: {b for (tt, b) in SCHEMAS if tt == t} for t in BLOCKED}

# Champs exiges selon une valeur du message (sinon facultatifs dans le schema).
REQUIRED_BY = {
    ("abonnement", "quoi"): {
        "demande": ("abonne", "plancher_s", "max_s", "applique_s"),
        "etabli": ("origine", "min_s", "max_s"),
        "termine": (),
        "reprise": ("mode", "verdict", "sauves", "abonnes", "lances", "servis", "en_cours"),
        "session": ("abonne", "ok", "erreur", "duree_ms"),
        "reprise_abonne": ("abonne", "verdict", "repris", "sans_readhandler", "rates"),
    },
    ("module", "etat"): {
        "panne": ("sans_guerison", "symptome", "essai_s"),
        "config_rejetee": ("rfch", "dm1", "rt1"),
    },
    ("relance", "cause"): {
        "delais": (),
        "bruit": (),
        "sourde": (),
        "verif": (),
        "l3": (),
    },
}
RELAUNCH_DETAIL = {
    "delais": ("suite",),
    "bruit": ("trames", "crc_faux", "ms"),
    "sourde": ("hors_rx", "ms"),
    "verif": ("verif_ratees",),
    "l3": (),
}

# ---------------------------------------------------------------------------
#  Verification
# ---------------------------------------------------------------------------


def check(spec, value, path, errs, warns):
    """Verifie value contre spec. Champ inconnu : avertissement (9.1 : les ajouts
    gardent v et l'app les ignore) ; --strict en fait une erreur."""
    if isinstance(spec, Opt):
        spec = spec.spec
    if isinstance(spec, Null):
        if value is None:
            return
        spec = spec.spec
    if value is None:
        errs.append(f"{path} : null interdit")
        return
    if isinstance(spec, Int):
        if isinstance(value, bool) or not isinstance(value, int):
            errs.append(f"{path} : entier attendu, {type(value).__name__} ({value!r})")
        elif not spec.lo <= value <= spec.hi:
            errs.append(f"{path} : {value} hors de {spec.lo}..{spec.hi}")
    elif isinstance(spec, Bool):
        if not isinstance(value, bool):
            errs.append(f"{path} : booleen attendu ({value!r})")
    elif isinstance(spec, Str):
        if not isinstance(value, str):
            errs.append(f"{path} : chaine attendue ({value!r})")
        else:
            if len(value) > spec.maxlen:
                errs.append(f"{path} : {len(value)} caracteres, {spec.maxlen} au plus")
            if spec.pattern and not spec.pattern.search(value):
                errs.append(f"{path} : {value!r} ne suit pas {spec.pattern.pattern}")
    elif isinstance(spec, Enum):
        if not isinstance(value, str):
            errs.append(f"{path} : chaine d'enumeration attendue ({value!r})")
        elif value not in spec.values:
            errs.append(f"{path} : valeur inconnue {value!r} (attendu : {', '.join(sorted(spec.values))})")
    elif isinstance(spec, Hex):
        if not isinstance(value, str) or not re.fullmatch(r"(?:[0-9A-F]{2})*", value):
            errs.append(f"{path} : hexa majuscule sans 0x attendu ({value!r})")
        elif not spec.lo <= len(value) // 2 <= spec.hi:
            errs.append(f"{path} : {len(value) // 2} octet(s), {spec.lo}..{spec.hi} attendu(s)")
    elif isinstance(spec, Arr):
        if not isinstance(value, list):
            errs.append(f"{path} : tableau attendu ({value!r})")
            return
        if spec.maxlen is not None and len(value) > spec.maxlen:
            errs.append(f"{path} : {len(value)} elements, {spec.maxlen} au plus")
        for i, v in enumerate(value):
            check(spec.item, v, f"{path}[{i}]", errs, warns)
    elif isinstance(spec, Obj):
        if not isinstance(value, dict):
            errs.append(f"{path} : objet attendu ({value!r})")
            return
        for k, sub in spec.fields.items():
            if k not in value:
                if not isinstance(sub, Opt):
                    errs.append(f"{path}.{k} : champ obligatoire absent")
                continue
            check(sub, value[k], f"{path}.{k}", errs, warns)
        if not spec.extra_ok:
            for k in value:
                if k not in spec.fields:
                    warns.append(f"{path}.{k} : champ inconnu de la v1 (ignore par l'app)")


def find_floats(value, path, errs):
    if isinstance(value, float):
        errs.append(f"{path} : flottant interdit ({value!r})")
    elif isinstance(value, dict):
        for k, v in value.items():
            find_floats(v, f"{path}.{k}", errs)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            find_floats(v, f"{path}[{i}]", errs)


def coherence(t, obj, errs, warns):
    """Regles qui lient plusieurs champs."""
    if t == "reponse":
        code, ok, etape = obj.get("code"), obj.get("ok"), obj.get("etape")
        if code in OK_CODES and ok is not True:
            errs.append(f"reponse : code {code} avec ok {ok}")
        if code in KO_CODES and ok is not False:
            errs.append(f"reponse : code {code} avec ok {ok}")
        if etape == "debut" and code != "en_cours":
            errs.append(f"reponse : etape debut avec code {code}")
        if etape == "fin" and "duree_ms" not in obj:
            errs.append("reponse : etape fin sans duree_ms")
        if etape == "debut" and "duree_ms" in obj:
            warns.append("reponse : etape debut avec duree_ms")
        if code == "accepte" and obj.get("suite") != "livraison":
            errs.append("reponse : code accepte sans suite livraison")
        if code in ("differe", "ok") and obj.get("suite") == "livraison":
            errs.append(f"reponse : code {code} avec suite livraison")
        trio = [k in obj for k in ("consigne", "a_livrer", "version")]
        if any(trio) and not all(trio):
            errs.append("reponse : consigne, a_livrer et version vont ensemble")
        if ("bail_s" in obj) != ("up_s" in obj):
            errs.append("reponse : bail_s et up_s vont ensemble")
    elif t == "rx":
        if obj.get("source") == "accuse":
            for k in ("brut", "pid", "no_ack", "crc"):
                if obj.get(k) is not None:
                    errs.append(f"rx accuse : {k} doit valoir null")
            if obj.get("crc_ok") is not True:
                errs.append("rx accuse : crc_ok doit valoir true")
        elif obj.get("source") == "ecoute":
            for k in ("brut", "pid", "no_ack", "crc"):
                if obj.get(k) is None:
                    errs.append(f"rx ecoute : {k} ne doit pas valoir null")
        charge, length = obj.get("charge"), obj.get("len")
        if isinstance(charge, str) and isinstance(length, int) and length <= 4 and len(charge) != 2 * length:
            errs.append(f"rx : charge de {len(charge) // 2} octet(s) pour len {length}")
        typ, sens = obj.get("type"), obj.get("sens")
        if typ in ("lum", "temp"):
            if not isinstance(sens, dict) or not {"marche", "lampes", typ} <= set(sens):
                errs.append(f"rx {typ} : sens doit porter marche, lampes et {typ}")
        elif typ == "a":
            if not isinstance(sens, dict) or not {"numero", "copie"} <= set(sens):
                errs.append("rx a : sens doit porter numero et copie")
        elif sens is not None:
            errs.append(f"rx {typ} : sens doit valoir null")
        if (typ == "crc_faux") == (obj.get("crc_ok") is True):
            errs.append("rx : type crc_faux si et seulement si crc_ok faux")
    elif t == "livraison":
        issue = obj.get("issue")
        if issue == "abandon" and "cause" not in obj:
            warns.append("livraison abandon sans cause")
        if issue != "abandon" and "cause" in obj:
            errs.append(f"livraison {issue} avec cause")
        if issue == "livree" and "derniere" not in obj:
            errs.append("livraison livree sans derniere")
        if issue != "livree" and obj.get("derniere") is not None:
            errs.append(f"livraison {issue} avec derniere non nulle")
    elif t == "relance":
        cause, detail = obj.get("cause"), obj.get("detail")
        if isinstance(detail, dict) and cause in RELAUNCH_DETAIL:
            want = set(RELAUNCH_DETAIL[cause])
            if set(detail) != want:
                errs.append(f"relance {cause} : detail {sorted(detail)} au lieu de {sorted(want)}")
        if (cause == "l3") != (obj.get("rang") is None):
            errs.append("relance : rang null si et seulement si cause l3")
        if obj.get("ok") is False and (obj.get("quartz") is not None or obj.get("calib") is not None):
            errs.append("relance ratee : quartz et calib doivent valoir null")
    elif t == "config":
        lampe = obj.get("lampe")
        if isinstance(lampe, dict):
            a, air = lampe.get("adresse"), lampe.get("air")
            if isinstance(a, str) and isinstance(air, str) and len(a) == 8 and len(air) == 8:
                if "".join(reversed([a[i : i + 2] for i in range(0, 8, 2)])) != air:
                    errs.append(f"config : air {air} n'est pas l'adresse {a} a l'envers")
    elif t == "hello" and obj.get("bloc") == "base":
        if obj.get("fw") != obj.get("fw_desc"):
            warns.append(f"hello : fw {obj.get('fw')!r} different de fw_desc {obj.get('fw_desc')!r}")
    elif t == "intent":
        booted = obj.get("ignore") == "demarrage"
        for k in ("champs", "consigne", "version", "a"):
            if booted and k in obj:
                errs.append(f"intent ignore au demarrage : {k} doit etre absent")
            if not booted and k not in obj:
                errs.append(f"intent : {k} absent")
    for (tt, key), table in REQUIRED_BY.items():
        if t != tt:
            continue
        for k in table.get(obj.get(key), ()):
            if k not in obj:
                errs.append(f"{t} {key}={obj.get(key)} : champ {k} absent")


def check_line(raw):
    """raw : octets de RS (compris) a LF (exclu). Rend (objet ou None, erreurs, avertissements)."""
    errs, warns = [], []
    size = len(raw) + 1  # LF compris
    if size > LINE_MAX:
        errs.append(f"{size} octets, {LINE_MAX} au plus (RS et LF compris)")
    elif size > BUDGET:
        warns.append(f"{size} octets : au-dela du budget de pire cas de {BUDGET}")
    body = raw[1:]
    bad = [b for b in body if b < 0x20 or b > 0x7E]
    if bad:
        errs.append(f"{len(bad)} octet(s) hors ASCII imprimable (premier : 0x{bad[0]:02X})")
    text = body.decode("ascii", "replace")
    if not text.startswith('{"v":'):
        errs.append("ne commence pas par {\"v\":")
    if not text.endswith("}"):
        errs.append("ne finit pas par }")
    try:
        pairs = []

        def hook(items):
            pairs.append([k for k, _ in items])
            keys = [k for k, _ in items]
            if len(keys) != len(set(keys)):
                errs.append(f"cle en double : {keys}")
            return dict(items)

        obj = json.loads(text, object_pairs_hook=hook)
    except ValueError as e:
        errs.append(f"JSON invalide : {e}")
        return None, errs, warns
    if not isinstance(obj, dict):
        errs.append("pas un objet JSON")
        return None, errs, warns
    compact = json.dumps(obj, separators=(",", ":"), ensure_ascii=True)
    if compact != text:
        errs.append("forme non compacte (espace hors chaine, ou echappement inattendu)")
    keys = list(obj)
    if keys[:4] != ["v", "t", "n", "ms"]:
        errs.append(f"champs de tete {keys[:4]} au lieu de v, t, n, ms")
    if obj.get("v") != 1 or isinstance(obj.get("v"), bool):
        errs.append(f"v = {obj.get('v')!r}, version geree : 1")
    t = obj.get("t")
    for k in ("n", "ms"):
        v = obj.get(k)
        if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= U32_MAX:
            errs.append(f"{k} = {v!r} : entier 0..4294967295 attendu")
    if not isinstance(t, str):
        errs.append(f"t = {t!r} : chaine attendue")
        return obj, errs, warns
    find_floats(obj, t, errs)
    bloc = obj.get("bloc")
    if t in BLOCKED:
        if len(keys) < 5 or keys[4] != "bloc":
            errs.append(f"{t} : bloc attendu en 5e champ")
        if bloc not in BLOCS[t]:
            errs.append(f"{t} : bloc {bloc!r} inconnu (attendu : {', '.join(sorted(BLOCS[t]))})")
            return obj, errs, warns
    elif "bloc" in obj:
        errs.append(f"{t} : pas de bloc pour ce type")
    schema = SCHEMAS.get((t, bloc if t in BLOCKED else None))
    if schema is None:
        warns.append(f"type inconnu de la v1 : {t!r} (ignore par l'app)")
        return obj, errs, warns
    content = {k: v for k, v in obj.items() if k not in ("v", "t", "n", "ms", "bloc")}
    check(schema, content, f"{t}" + (f"/{bloc}" if bloc else ""), errs, warns)
    coherence(t, obj, errs, warns)
    return obj, errs, warns


# ---------------------------------------------------------------------------
#  Lecture d'une capture
# ---------------------------------------------------------------------------


def split_lines(data):
    """Section 2.4 : lignes terminees par LF, CR final retire."""
    lines = data.split(b"\n")
    tail = lines.pop()  # sans LF : ligne pas finie
    out = [l[:-1] if l.endswith(b"\r") else l for l in lines]
    return out, tail


def examples(path):
    """Lignes <RS>{...} des exemples de la specification (section 12)."""
    data = open(path, "rb").read().decode("utf-8")
    out = []
    for line in data.splitlines():
        if line.startswith("<RS>{"):
            out.append(b"\x1e" + line[4:].encode("ascii"))
    return out


class Report:
    def __init__(self, strict, quiet, continuity=True):
        self.strict, self.quiet, self.continuity = strict, quiet, continuity
        self.lines = self.errors = self.warnings = self.text = self.broken = self.fragments = 0
        self.by_type = {}
        self.worst = (0, None)
        self.gaps = self.lost = self.restarts = 0
        self.last_n = None
        self.after_broken = False

    def say(self, where, level, msg):
        if not self.quiet:
            print(f"{where} : {level} : {msg}")

    def feed(self, where, line):
        i = line.rfind(bytes([RS]))
        if i < 0:
            if line.strip().endswith(b"}") and self.after_broken:
                self.fragments += 1
            else:
                self.text += 1
            self.after_broken = False
            return
        machine = line[i:]
        obj, errs, warns = check_line(machine)
        self.lines += 1
        size = len(machine) + 1
        if size > self.worst[0]:
            self.worst = (size, where)
        if obj is None or errs and not isinstance(obj, dict):
            self.broken += 1
        self.after_broken = obj is None
        if isinstance(obj, dict):
            key = obj.get("t"), obj.get("bloc")
            self.by_type[key] = self.by_type.get(key, 0) + 1
            n = obj.get("n")
            if self.continuity and isinstance(n, int) and not isinstance(n, bool):
                if self.last_n is not None:
                    if n == self.last_n + 1:
                        pass
                    elif n > self.last_n + 1:
                        self.gaps += 1
                        self.lost += n - self.last_n - 1
                        warns.append(f"n saute de {self.last_n} a {n} : {n - self.last_n - 1} ligne(s) perdue(s)")
                    else:
                        self.restarts += 1
                        self.say(where, "info", f"n recule ({self.last_n} -> {n}) : redemarrage ou lignes anciennes")
                self.last_n = n
        for e in errs:
            self.errors += 1
            self.say(where, "ERREUR", e)
        for w in warns:
            if self.strict:
                self.errors += 1
                self.say(where, "ERREUR (strict)", w)
            else:
                self.warnings += 1
                self.say(where, "avertissement", w)

    def summary(self):
        print(
            f"{self.lines} ligne(s) machine, {self.errors} erreur(s), {self.warnings} avertissement(s) ; "
            f"{self.text} ligne(s) de texte, {self.fragments} fragment(s), {self.broken} ligne(s) abimee(s) ; "
            f"n : {self.gaps} trou(s) ({self.lost} ligne(s) perdue(s)), {self.restarts} recul(s)"
        )
        if self.worst[1]:
            print(f"plus longue ligne : {self.worst[0]} octets ({self.worst[1]})")
        if self.by_type:
            parts = [f"{t}{'/' + b if b else ''} {c}" for (t, b), c in sorted(self.by_type.items(), key=str)]
            print("par type : " + ", ".join(parts))


def main():
    ap = argparse.ArgumentParser(description="Verifie les lignes machine du pont Halo (docs/PROTOCOLE-JSON.md).")
    ap.add_argument("captures", nargs="*", help="captures brutes du port serie")
    ap.add_argument("--exemples", metavar="MD", help="verifier les exemples <RS>{...} d'un document (section 12)")
    ap.add_argument("--strict", action="store_true", help="avertissements comptes comme erreurs")
    ap.add_argument("-q", "--quiet", action="store_true", help="resume seulement")
    ap.add_argument(
        "--independantes", action="store_true", help="lignes independantes (exemples, tests) : pas de controle de n"
    )
    args = ap.parse_args()
    if not args.captures and not args.exemples:
        ap.error("donner au moins une capture, ou --exemples")
    rep = Report(args.strict, args.quiet, not args.independantes)
    try:
        if args.exemples:
            continuity, rep.continuity = rep.continuity, False  # exemples independants
            for k, line in enumerate(examples(args.exemples), 1):
                rep.feed(f"{args.exemples} exemple {k}", line)
            rep.continuity = continuity
        for path in args.captures:
            with open(path, "rb") as f:
                data = f.read()
            lines, tail = split_lines(data)
            rep.last_n = None
            rep.after_broken = True  # debut de capture : une suite de ligne est un fragment
            for k, line in enumerate(lines, 1):
                rep.feed(f"{path}:{k}", line)
            if tail.strip():
                rep.say(path, "info", f"derniere ligne sans LF ({len(tail)} octets) ignoree")
    except OSError as e:
        print(f"lecture impossible : {e}", file=sys.stderr)
        return 2
    rep.summary()
    return 1 if rep.errors else 0


if __name__ == "__main__":
    sys.exit(main())
