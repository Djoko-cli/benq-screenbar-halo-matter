import Foundation

// Evenements (section 7) : des indices, la verite est l'instantane periodique.

/// `rx` : trame entendue (7.1).
public struct TrameRx: Codable, Sendable, Equatable {
    /// `sens` : `lum`/`temp` : marche, lampes, et lum ou temp ; `a` : numero, copie.
    public struct Sens: Codable, Sendable, Equatable {
        public var marche: Bool?
        public var lampes: Lampes?
        public var lum: Int?
        public var temp: Int?
        public var numero: Int?
        public var copie: Bool?
    }

    public var source: SourceTrame?
    public var brut: String?
    public var len: Int?
    public var pid: Int?
    public var noAck: Int?
    public var charge: String?
    public var crc: String?
    public var crcOk: Bool?
    public var type: TypeTrame
    public var sens: Sens?
    public var sautes: Int?
}

/// `tx` : paquet emis (7.2).
public struct PaquetTx: Codable, Sendable, Equatable {
    public var num: Int?
    public var tranche: TypeTranche?
    public var charge: String?
    public var essai: Int?
    public var paquets: Int?
    public var accuses: Int?
    public var verdict: VerdictTx
    public var us: Int?
    public var rt2: String?
    public var irq1: String?
    public var status: String?
    public var sautes: Int?
}

/// `livraison` : fin d'une consigne (7.3).
public struct Livraison: Codable, Sendable, Equatable {
    public var issue: IssueLivraison
    public var cause: CauseAbandon?
    public var derniere: TypeTranche?
    public var version: Int?
    public var consigne: EtatLampe?
    public var cru: EtatLampe?
    public var aLivrer: [ChampConsigne]?
    public var ids: [Int]?
    public var idsPerdus: Int?
    public var attenteMs: Int?
    public var livrees: Int?
    public var abandons: Int?
}

/// `relance` : relance du module BM5602 (7.4).
public struct Relance: Codable, Sendable, Equatable {
    public struct Detail: Codable, Sendable, Equatable {
        public var suite: Int?
        public var trames: Int?
        public var crcFaux: Int?
        public var ms: Int?
        public var horsRx: Int?
        public var verifRatees: Int?
    }

    public var cause: CauseRelance
    public var rang: Int?
    public var detail: Detail?
    public var ok: Bool?
    public var quartz: Bool?
    public var calib: Bool?
    public var dureeMs: Int?
    public var total: Int?
    public var panne: Bool?
}

/// `module` : etat du module (7.5).
public struct EvenementModule: Codable, Sendable, Equatable {
    public var etat: EtatModule
    public var sansGuerison: Int?
    public var symptome: Symptome?
    public var essaiS: Int?
    public var rfch: String?
    public var dm1: String?
    public var rt1: String?
}

/// `intent` : ordres Matter resolus (7.6).
public struct IntentMatter: Codable, Sendable, Equatable {
    public struct Recu: Codable, Sendable, Equatable {
        public var ep1: Bool?
        public var niveau: Int?
        public var mireds: Int?
        public var avant: Bool?
        public var arriere: Bool?
        public var a: Bool?
    }

    public var recu: Recu?
    public var fenetreMs: Int?
    public var ignore: String?
    public var champs: [ChampConsigne]?
    public var consigne: EtatLampe?
    public var version: Int?
    public var a: String?
}

/// `abonnement` : abonnements Matter (7.7). Les champs dependent de `quoi`.
public struct EvenementAbonnement: Codable, Sendable, Equatable {
    public struct Totaux: Codable, Sendable, Equatable {
        public var demandes: Int?
        public var etablis: Int?
        public var termines: Int?
        public var passages: Int?
    }

    public var quoi: QuoiAbonnement
    public var abonne: String?
    public var plancherS: Int?
    public var maxS: Int?
    public var appliqueS: Int?
    public var origine: String?
    public var minS: Int?
    public var mode: String?
    public var verdict: String?
    public var sauves: Int?
    public var abonnes: Int?
    public var lances: Int?
    public var servis: Int?
    public var enCours: ValeurScalaire?
    public var ok: Bool?
    public var erreur: String?
    public var dureeMs: Int?
    public var repris: Int?
    public var sansReadhandler: Int?
    public var rates: Int?
    public var totaux: Totaux?
}

/// `thread` : changement de role (7.8).
public struct ChangementRole: Codable, Sendable, Equatable {
    public var de: String?
    public var vers: String
    public var aMs: Int?
    public var total: Int?
}

/// `led` : motif du voyant (7.9).
public struct ChangementLed: Codable, Sendable, Equatable {
    public var motif: MotifLed
    public var avant: MotifLed?
    public var test: Bool?
}

/// `log` : annonces et traces du firmware (7.10).
public struct MessageLog: Codable, Sendable, Equatable {
    public var src: String?
    public var niv: String?
    public var txt: String
}
