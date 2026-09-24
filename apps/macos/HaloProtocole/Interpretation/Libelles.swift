import Foundation

// Libelles francais des enumerations, pour l'interface.

extension Lampes {
    public var libelle: String {
        switch self {
        case .avant: "avant"
        case .arriere: "arrière"
        case .deux: "les deux"
        case .inconnu: "inconnu"
        }
    }
}

extension ChampConsigne {
    public var libelle: String {
        switch self {
        case .marche: "marche"
        case .lum: "luminosité"
        case .temp: "température"
        case .inconnu: "inconnu"
        }
    }
}

extension PhasePilote {
    public var libelle: String {
        switch self {
        case .repos: "au repos"
        case .rafale: "rafale en cours"
        case .reprise: "reprise en attente"
        case .inconnu: "inconnue"
        }
    }
}

extension EtatLien {
    public var libelle: String {
        switch self {
        case .inconnu: "inconnu"
        case .ok: "joignable"
        case .perdu: "injoignable"
        }
    }
}

extension ModeRadio {
    public var libelle: String {
        switch self {
        case .inconnu: "inconnu"
        case .reset: "reset"
        case .emission: "émission"
        case .ecoute: "écoute"
        case .veille: "veille"
        }
    }
}

extension Symptome {
    public var libelle: String {
        switch self {
        case .delais: "délais TX"
        case .bruit: "bruit (CRC faux)"
        case .sourde: "puce sourde"
        case .inconnu: "inconnu"
        }
    }
}

extension CauseRelance {
    public var libelle: String {
        switch self {
        case .verif: "vérification ratée"
        case .delais: "délais TX"
        case .bruit: "bruit (CRC faux)"
        case .sourde: "puce sourde"
        case .l3: "essai L3 (module perdu)"
        case .inconnu: "inconnue"
        }
    }
}

extension MotifLed {
    public var libelle: String {
        switch self {
        case .identification: "identification"
        case .desappairage: "désappairage armé"
        case .redemarrage: "redémarrage"
        case .injoignable: "lampe injoignable"
        case .panneRadio: "panne radio"
        case .livree: "consigne livrée"
        case .nonAppaire: "non appairé"
        case .horsReseau: "hors réseau"
        case .operationnel: "opérationnel"
        case .inconnu: "inconnu"
        }
    }

    public var description: String {
        switch self {
        case .identification: "arc-en-ciel"
        case .desappairage: "rouge/violet rapide : relâcher le bouton pour désappairer"
        case .redemarrage: "éclat blanc, puis redémarrage"
        case .injoignable: "rouge, 3 clignements"
        case .panneRadio: "rouge fixe"
        case .livree: "éclat vert"
        case .nonAppaire: "bleu clignotant"
        case .horsReseau: "orange lent"
        case .operationnel: "éteint, lueur blanche toutes les 10 s"
        case .inconnu: "?"
        }
    }
}

extension TypeTranche {
    public var libelle: String {
        switch self {
        case .lum: "luminosité"
        case .temp: "température"
        case .a: "bouton A"
        case .brut: "brute"
        case .inconnu: "inconnue"
        }
    }
}

extension TypeTrame {
    public var libelle: String {
        switch self {
        case .lum: "luminosité"
        case .temp: "température"
        case .a: "bouton A"
        case .accuseLampe: "accusé de la lampe"
        case .service: "service"
        case .favori: "favori"
        case .invalide: "invalide"
        case .crcFaux: "CRC faux"
        case .inconnu: "inconnue"
        }
    }
}

extension VerdictTx {
    public var libelle: String {
        switch self {
        case .ack: "accusé"
        case .ackTrame: "trame au lieu d'un accusé"
        case .maxRt: "MAX_RT (sans accusé)"
        case .delai: "délai dépassé"
        case .fifo: "FIFO refusée"
        case .inconnu: "inconnu"
        }
    }

    public var estEchec: Bool { self != .ack }
}

extension IssueLivraison {
    public var libelle: String {
        switch self {
        case .livree: "livrée"
        case .abandon: "abandon"
        case .annulee: "annulée"
        case .inconnu: "inconnue"
        }
    }
}

extension CauseAbandon {
    public var libelle: String {
        switch self {
        case .injoignable: "lampe injoignable"
        case .module: "module radio perdu"
        case .inconnu: "inconnue"
        }
    }
}

extension EtatModule {
    public var libelle: String {
        switch self {
        case .panne: "EN PANNE"
        case .retabli: "rétabli"
        case .perdu: "perdu (muet)"
        case .retrouve: "retrouvé"
        case .configRejetee: "configuration rejetée"
        case .configVerifiee: "configuration vérifiée"
        case .inconnu: "inconnu"
        }
    }
}

extension CodeReponse {
    public var libelle: String {
        switch self {
        case .ok: "exécutée"
        case .accepte: "acceptée, livraison à suivre"
        case .differe: "différée (rien à émettre)"
        case .enCours: "en cours"
        case .execute: "exécutée"
        case .usage: "arguments invalides"
        case .refuse: "refusée"
        case .radioAbsente: "module radio absent"
        case .radioPerdue: "module radio perdu"
        case .commandeInconnue: "commande inconnue"
        case .tropLong: "ligne trop longue"
        case .cadence: "cadence dépassée"
        case .interdite: "interdite sur ce transport"
        case .inconnu: "code inconnu"
        }
    }
}

extension EtatCommande {
    public var libelle: String {
        switch self {
        case .enFile: "en file"
        case .envoyee: "envoyée"
        case .enCours: "en cours"
        case .terminee: "terminée"
        case .attenteLivraison: "livraison attendue"
        case .livree: "livrée"
        case .abandonnee: "abandonnée"
        case .annulee: "annulée"
        case .sansReponse: "sans réponse"
        case .finPerdue: "terminée (fin perdue)"
        case .remplacee: "remplacée"
        case .perdue: "connexion perdue"
        }
    }
}

extension MoteurSession.Phase {
    public var libelle: String {
        switch self {
        case .ferme: "Déconnecté"
        case .attenteHello(let e): e > 1 ? "Connexion (essai \(e))…" : "Connexion…"
        case .connecte: "Connecté"
        case .resynchro: "Resynchronisation…"
        case .ancienFirmware: "Firmware sans protocole JSON"
        case .sansReponse: "Aucune réponse de la carte"
        case .versionInconnue(let v): "Protocole v\(v) non géré"
        case .modeHumain: "Mode humain"
        }
    }
}
