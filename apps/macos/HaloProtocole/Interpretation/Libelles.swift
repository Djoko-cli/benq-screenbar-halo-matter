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
