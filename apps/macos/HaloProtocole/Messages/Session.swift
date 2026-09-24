import Foundation

/// Objet **Etat** (section 4) : consigne ou etat cru de la lampe.
public struct EtatLampe: Codable, Sendable, Equatable, Hashable {
    public var marche: Bool?
    public var lampes: Lampes?
    /// Luminosite brute 76..254 (0x4C..0xFE), comme sur l'air.
    public var lum: Int?
    /// Niveau Matter canonique 4..254.
    public var niveau: Int?
    /// Temperature brute 0 (froid)..100 (chaud).
    public var temp: Int?
    /// Temperature en mireds 153..370.
    public var mired: Int?

    public init(marche: Bool? = nil, lampes: Lampes? = nil, lum: Int? = nil,
                niveau: Int? = nil, temp: Int? = nil, mired: Int? = nil) {
        self.marche = marche
        self.lampes = lampes
        self.lum = lum
        self.niveau = niveau
        self.temp = temp
        self.mired = mired
    }
}

/// Valeur scalaire dont le type n'est pas fige par la specification.
public enum ValeurScalaire: Codable, Sendable, Equatable, Hashable, CustomStringConvertible {
    case entier(Int)
    case booleen(Bool)
    case texte(String)
    case nul

    public init(from decoder: any Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() {
            self = .nul
        } else if let b = try? c.decode(Bool.self) {
            self = .booleen(b)
        } else if let i = try? c.decode(Int.self) {
            self = .entier(i)
        } else {
            self = .texte(try c.decode(String.self))
        }
    }

    public func encode(to encoder: any Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .entier(let i): try c.encode(i)
        case .booleen(let b): try c.encode(b)
        case .texte(let s): try c.encode(s)
        case .nul: try c.encodeNil()
        }
    }

    public var description: String {
        switch self {
        case .entier(let i): String(i)
        case .booleen(let b): b ? "oui" : "non"
        case .texte(let s): s
        case .nul: "-"
        }
    }
}

// MARK: - hello (5.1)

/// `hello`, bloc `base`.
public struct HelloBase: Codable, Sendable, Equatable {
    public struct ReglagesSession: Codable, Sendable, Equatable {
        public var transport: String?
        public var periodeMs: Int?
        public var compteursMs: Int?
        public var reseauMs: Int?
        public var bailS: Int?
        public var trames: Bool?
        public var log: Bool?
    }

    public struct Limites: Codable, Sendable, Equatable {
        public var ligneMax: Int?
        public var cmdMax: Int?
    }

    public var rev: Int?
    public var fw: String?
    public var fwDesc: String?
    public var date: String?
    public var heure: String?
    public var env: String?
    public var build: String?
    public var reseauBuild: String?
    public var puce: String?
    public var idf: String?
    public var arduino: String?
    public var boot: String?
    public var reset: String?
    public var resetN: Int?
    public var upS: Int?
    public var session: ReglagesSession?
    public var limites: Limites?
}

/// `hello`, bloc `identite`.
public struct HelloIdentite: Codable, Sendable, Equatable {
    public struct Identite: Codable, Sendable, Equatable {
        public var fabricant: String?
        public var produit: String?
        public var serie: String?
        public var nom: String?
        public var hw: Int?
        public var hwTxt: String?
    }

    public var boot: String?
    public var mac: String?
    public var id: Identite?
    public var caps: [String]?
}

// MARK: - config (5.2)

public struct ConfigCarte: Codable, Sendable, Equatable {
    public struct Lampe: Codable, Sendable, Equatable {
        public var adresse: String?
        public var air: String?
        public var canal: Int?
        public var debitKbps: Int?
    }

    public struct Reglages: Codable, Sendable, Equatable {
        public var paquets: Int?
        public var accusesMin: Int?
        public var paquetsMax: Int?
        public var ecartMs: Int?
        public var repriseMs: Int?
        public var reprises: Int?
        public var rearmMs: Int?
        public var silenceMs: Int?
        public var rearmFort: Bool?
        public var leger: Bool?
        /// `null` sans garde d'antenne (diag, Wi-Fi).
        public var garde: Bool?
        /// Gamma en centiemes (200 = 2,00).
        public var gammaC: Int?
    }

    public struct Seuils: Codable, Sendable, Equatable {
        public var delaisSuite: Int?
        public var delugeTrames: Int?
        public var delugePct: Int?
        public var fenetreMs: Int?
        public var sourdHorsRx: Int?
        public var sansGuerison: Int?
        public var ecartMs: Int?
        public var repliMs: Int?
    }

    public struct Matter: Codable, Sendable, Equatable {
        public struct Endpoints: Codable, Sendable, Equatable {
            public var principal: Int?
            public var avant: Int?
            public var arriere: Int?
            public var auto: Int?
        }

        public var endpoints: Endpoints?
        public var lampesEn: String?
        public var miredMin: Int?
        public var miredMax: Int?
        public var niveauPlancher: Int?
        public var impulsionMs: Int?
        public var med: Int?
        public var medBoot: Int?
        public var maxintS: Int?
        public var repriseAuto: Bool?
    }

    public var lampe: Lampe?
    public var reglages: Reglages?
    public var seuils: Seuils?
    /// `null` dans le build diag.
    public var matter: Matter?
}

// MARK: - hb, fin (5.6)

public struct Battement: Codable, Sendable, Equatable {
    public var boot: String?
    public var upS: Int?
    public var jsonPerdus: Int?
}

public struct FinSession: Codable, Sendable, Equatable {
    public var cause: CauseFin?
}

// MARK: - reponse (6.3)

public struct Reponse: Codable, Sendable, Equatable {
    public var id: Int
    public var etape: EtapeReponse
    public var cmd: String?
    public var ok: Bool
    public var code: CodeReponse
    public var msg: String?
    public var dureeMs: Int?
    public var suite: SuiteReponse?
    public var consigne: EtatLampe?
    public var aLivrer: [ChampConsigne]?
    public var version: Int?
    public var bailS: Int?
    public var upS: Int?
    /// `json cle nouvelle` : la cle, rendue une seule fois (masquee par l'app).
    public var cle: String?
    /// `json cle` : empreinte de la cle.
    public var empreinte: String?
}
