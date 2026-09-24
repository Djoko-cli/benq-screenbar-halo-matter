import Foundation

// Libelles des enumerations, pour l'interface, dans la langue en vigueur
// (catalogue du framework ; cles : les libelles francais).

extension Lampes {
    public var libelle: String {
        switch self {
        case .avant: tr("avant")
        case .arriere: tr("arrière")
        case .deux: tr("les deux")
        case .inconnu: tr("inconnu")
        }
    }
}

extension ChampConsigne {
    public var libelle: String {
        switch self {
        case .marche: tr("marche")
        case .lum: tr("luminosité")
        case .temp: tr("température")
        case .inconnu: tr("inconnu")
        }
    }
}

extension PhasePilote {
    public var libelle: String {
        switch self {
        case .repos: tr("au repos")
        case .rafale: tr("rafale en cours")
        case .reprise: tr("reprise en attente")
        case .inconnu: tr("inconnue")
        }
    }
}

extension EtatLien {
    public var libelle: String {
        switch self {
        case .inconnu: tr("inconnu")
        case .ok: tr("joignable")
        case .perdu: tr("injoignable")
        }
    }
}

extension ModeRadio {
    public var libelle: String {
        switch self {
        case .inconnu: tr("inconnu")
        case .reset: tr("reset")
        case .emission: tr("émission")
        case .ecoute: tr("écoute")
        case .veille: tr("veille")
        }
    }
}

extension Symptome {
    public var libelle: String {
        switch self {
        case .delais: tr("délais TX")
        case .bruit: tr("bruit (CRC faux)")
        case .sourde: tr("puce sourde")
        case .inconnu: tr("inconnu")
        }
    }
}

extension CauseRelance {
    public var libelle: String {
        switch self {
        case .verif: tr("vérification ratée")
        case .delais: tr("délais TX")
        case .bruit: tr("bruit (CRC faux)")
        case .sourde: tr("puce sourde")
        case .l3: tr("essai L3 (module perdu)")
        case .inconnu: tr("inconnue")
        }
    }
}

extension MotifLed {
    public var libelle: String {
        switch self {
        case .identification: tr("identification")
        case .desappairage: tr("désappairage armé")
        case .redemarrage: tr("redémarrage")
        case .injoignable: tr("lampe injoignable")
        case .panneRadio: tr("panne radio")
        case .livree: tr("consigne livrée")
        case .nonAppaire: tr("non appairé")
        case .horsReseau: tr("hors réseau")
        case .operationnel: tr("opérationnel")
        case .inconnu: tr("inconnu")
        }
    }

    public var description: String {
        switch self {
        case .identification: tr("arc-en-ciel")
        case .desappairage: tr("rouge/violet rapide : relâcher le bouton pour désappairer")
        case .redemarrage: tr("éclat blanc, puis redémarrage")
        case .injoignable: tr("rouge, 3 clignements")
        case .panneRadio: tr("rouge fixe")
        case .livree: tr("éclat vert")
        case .nonAppaire: tr("bleu clignotant")
        case .horsReseau: tr("orange lent")
        case .operationnel: tr("éteint, lueur blanche toutes les 10 s")
        case .inconnu: "?"
        }
    }
}

extension TypeTranche {
    public var libelle: String {
        switch self {
        case .lum: tr("luminosité")
        case .temp: tr("température")
        case .a: tr("bouton A")
        case .brut: tr("brute")
        case .inconnu: tr("inconnue")
        }
    }
}

extension TypeTrame {
    public var libelle: String {
        switch self {
        case .lum: tr("luminosité")
        case .temp: tr("température")
        case .a: tr("bouton A")
        case .accuseLampe: tr("accusé de la lampe")
        case .service: tr("service")
        case .favori: tr("favori")
        case .invalide: tr("invalide")
        case .crcFaux: tr("CRC faux")
        case .inconnu: tr("inconnue")
        }
    }
}

extension VerdictTx {
    public var libelle: String {
        switch self {
        case .ack: tr("accusé")
        case .ackTrame: tr("trame au lieu d'un accusé")
        case .maxRt: tr("MAX_RT (sans accusé)")
        case .delai: tr("délai dépassé")
        case .fifo: tr("FIFO refusée")
        case .inconnu: tr("inconnu")
        }
    }

    public var estEchec: Bool { self != .ack }
}

extension IssueLivraison {
    public var libelle: String {
        switch self {
        case .livree: tr("livrée")
        case .abandon: tr("abandon")
        case .annulee: tr("annulée")
        case .inconnu: tr("inconnue")
        }
    }
}

extension CauseAbandon {
    public var libelle: String {
        switch self {
        case .injoignable: tr("lampe injoignable")
        case .module: tr("module radio perdu")
        case .inconnu: tr("inconnue")
        }
    }
}

extension EtatModule {
    public var libelle: String {
        switch self {
        case .panne: tr("EN PANNE")
        case .retabli: tr("rétabli")
        case .perdu: tr("perdu (muet)")
        case .retrouve: tr("retrouvé")
        case .configRejetee: tr("configuration rejetée")
        case .configVerifiee: tr("configuration vérifiée")
        case .inconnu: tr("inconnu")
        }
    }
}

extension CodeReponse {
    public var libelle: String {
        switch self {
        case .ok: tr("exécutée")
        case .accepte: tr("acceptée, livraison à suivre")
        case .differe: tr("différée (rien à émettre)")
        case .enCours: tr("en cours")
        case .execute: tr("exécutée")
        case .usage: tr("arguments invalides")
        case .refuse: tr("refusée")
        case .radioAbsente: tr("module radio absent")
        case .radioPerdue: tr("module radio perdu")
        case .commandeInconnue: tr("commande inconnue")
        case .tropLong: tr("ligne trop longue")
        case .cadence: tr("cadence dépassée")
        case .interdite: tr("interdite sur ce transport")
        case .dejaTraite: tr("déjà traitée (rien de réexécuté)")
        case .inconnu: tr("code inconnu")
        }
    }
}

extension EtatCommande {
    public var libelle: String {
        switch self {
        case .enFile: tr("en file")
        case .envoyee: tr("envoyée")
        case .enCours: tr("en cours")
        case .terminee: tr("terminée")
        case .attenteLivraison: tr("livraison attendue")
        case .livree: tr("livrée")
        case .abandonnee: tr("abandonnée")
        case .annulee: tr("annulée")
        case .sansReponse: tr("sans réponse")
        case .finPerdue: tr("terminée (fin perdue)")
        case .remplacee: tr("remplacée")
        case .perdue: tr("connexion perdue")
        }
    }
}

extension MoteurSession.Phase {
    public var libelle: String {
        switch self {
        case .ferme: tr("Déconnecté")
        case .attenteHello(let e): e > 1 ? tr("Connexion (essai \(e))…") : tr("Connexion…")
        case .connecte: tr("Connecté")
        case .resynchro: tr("Resynchronisation…")
        case .ancienFirmware: tr("Firmware sans protocole JSON")
        case .sansReponse: tr("Aucune réponse de la carte")
        case .versionInconnue(let v): tr("Protocole v\(String(v)) non géré")
        case .modeHumain: tr("Mode humain")
        }
    }
}

/// Valeurs de texte du firmware prises dans des listes fermees
/// (`docs/PROTOCOLE-JSON.md`), pour l'interface. Une valeur inconnue (firmware
/// plus recent) reste brute.
public enum ValeurFirmware {
    /// `hello.reset` (5.1) : cause du dernier demarrage (`esp_reset_reason()`).
    public static func causeDemarrage(_ v: String) -> String {
        switch v {
        case "mise_sous_tension": tr("mise sous tension")
        case "broche": tr("broche de reset")
        case "logiciel": tr("logiciel")
        case "panique": tr("panique")
        case "chien_int": tr("chien de garde (interruption)")
        case "chien_tache": tr("chien de garde (tâche)")
        case "chien": tr("autre chien de garde")
        case "baisse_tension": tr("baisse de tension")
        case "usb": "USB"
        case "inconnue": tr("inconnue")
        default: v
        }
    }

    /// `hello.build` (5.1) : build produit ou de banc.
    public static func build(_ v: String) -> String {
        v == "produit" ? tr("produit") : v
    }

    /// `intent.a` (7.6) : sort du bouton A.
    public static func boutonA(_ v: String) -> String {
        switch v {
        case "appui": tr("appui")
        case "ignore": tr("ignoré")
        case "refuse": tr("refusé")
        default: v
        }
    }

    /// `abonnement` `etabli` (7.7) : origine de l'abonnement.
    public static func origineAbonnement(_ v: String) -> String {
        switch v {
        case "neuf": tr("neuf")
        case "pont": tr("repris par le pont")
        case "pile": tr("repris par la pile")
        default: v
        }
    }

    /// `abonnement` `reprise` (7.7) : reprise automatique ou demandee.
    public static func modeReprise(_ v: String) -> String {
        v == "manuelle" ? tr("manuelle") : v
    }

    /// Verdict d'une reprise d'abonnements (`reprise`, `reprise_abonne`, 7.7).
    public static func verdictReprise(_ v: String) -> String {
        switch v {
        case "lance": tr("lancée")
        case "rien": tr("rien à reprendre")
        case "sans_stockage": tr("sans stockage")
        case "iterateur_occupe": tr("itérateur occupé")
        case "repris": tr("repris")
        case "deja_servi": tr("déjà servi")
        case "file_pleine": tr("file pleine")
        default: v
        }
    }
}
