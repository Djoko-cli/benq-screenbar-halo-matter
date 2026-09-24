import Foundation

// MARK: - etat (5.3)

/// `etat`, bloc `lampe`.
public struct BlocEtatLampe: Codable, Sendable, Equatable {
    public var boot: String?
    public var upS: Int?
    public var consigne: EtatLampe
    public var cru: EtatLampe
    public var aLivrer: [ChampConsigne]?
    public var confirme: [ChampConsigne]?
    public var version: Int?
    public var phase: PhasePilote?
    public var repriseMs: Int?
    public var echecs: Int?
    public var lien: EtatLien?
    /// Age du dernier accuse de la lampe (null : aucun depuis le demarrage).
    public var accuseMs: Int?
    public var dernierA: Int?
    public var aEntendus: Int?
    public var memoire: Lampes?
    public var livrees: Int?
    public var abandons: Int?
    public var sauveAttente: Bool?
    public var ecoute: Bool?
    public var trace: Bool?
}

/// `etat`, bloc `tranches`.
public struct BlocTranches: Codable, Sendable, Equatable {
    public struct Tranche: Codable, Sendable, Equatable, Hashable {
        public var tranche: TypeTranche?
        public var charge: String?
        public var accuses: Int?
        public var essais: Int?
        public var paquets: Int?
    }

    public var boot: String?
    public var upS: Int?
    public var tranches: [Tranche]?
}

/// `etat`, bloc `sante`.
public struct BlocSante: Codable, Sendable, Equatable {
    public struct Radio: Codable, Sendable, Equatable {
        public var presente: Bool?
        public var perdue: Bool?
        public var mode: ModeRadio?
        public var configuree: Bool?
        public var quartz: Bool?
        public var calib: Bool?
    }

    public struct DerniereRelance: Codable, Sendable, Equatable {
        public var cause: CauseRelance?
        /// `il_y_a_s`.
        public var ilYaS: Int?

        enum CodingKeys: String, CodingKey {
            case cause
            case ilYaS = "ilYAS"
        }
    }

    public struct Surveillance: Codable, Sendable, Equatable {
        public var panne: Bool?
        public var defaut: Bool?
        public var symptome: Symptome?
        public var delaisSuite: Int?
        public var fenTrames: Int?
        public var fenCrcFaux: Int?
        /// `hors_rx_10s`.
        public var horsRx10s: Int?
        public var sansGuerison: Int?
        public var attenteMs: Int?
        public var relances: Int?
        public var derniere: DerniereRelance?

        // convertFromSnakeCase fait "horsRx10S" de "hors_rx_10s".
        enum CodingKeys: String, CodingKey {
            case panne, defaut, symptome, delaisSuite, fenTrames, fenCrcFaux
            case horsRx10s = "horsRx10S"
            case sansGuerison, attenteMs, relances, derniere
        }
    }

    public struct Led: Codable, Sendable, Equatable {
        public var motif: MotifLed?
        public var test: Bool?
    }

    public struct Matter: Codable, Sendable, Equatable {
        public var enService: Bool?
        public var connecte: Bool?
        public var identify: Bool?
    }

    public struct Systeme: Codable, Sendable, Equatable {
        public var heap: Int?
        public var heapMin: Int?
        public var heapBloc: Int?
        public var pileBoucle: Int?
        public var boucleMaxMs: Int?
        public var jsonPerdus: Int?
        public var jsonTropLongs: Int?
        public var rejets: Int?
    }

    public var boot: String?
    public var upS: Int?
    public var radio: Radio?
    public var surveil: Surveillance?
    /// `null` en diag.
    public var led: Led?
    /// `null` en diag.
    public var matter: Matter?
    public var sys: Systeme?
}

// MARK: - compteurs (5.4)

/// `compteurs`, bloc `pilote`.
public struct CompteursPilote: Codable, Sendable, Equatable {
    public struct Emission: Codable, Sendable, Equatable {
        public var consignes: Int?
        public var paquets: Int?
        public var accuses: Int?
        public var ackTrame: Int?
        public var maxRt: Int?
        public var delais: Int?
        public var fifo: Int?
        public var total: Int?
    }

    public struct Tranches: Codable, Sendable, Equatable {
        public var faibles: Int?
        public var preemptees: Int?
        public var annulees: Int?
        public var reprises: Int?
        public var abandons: Int?
        public var attentes: Int?
    }

    public struct BoutonA: Codable, Sendable, Equatable {
        public var livres: Int?
        public var refuses: Int?
    }

    public struct Reception: Codable, Sendable, Equatable {
        public var trames: Int?
        public var etat: Int?
        public var a: Int?
        public var accusesLampe: Int?
        public var service: Int?
        public var favori: Int?
        public var invalides: Int?
        public var crcFaux: Int?
    }

    public struct Divers: Codable, Sendable, Equatable {
        public var sauvegardes: Int?
        public var tracesPerdues: Int?
        public var relancesModule: Int?
    }

    public var raz: Int?
    public var tx: Emission?
    public var tranches: Tranches?
    public var a: BoutonA?
    public var rx: Reception?
    public var divers: Divers?
}

/// `compteurs`, bloc `radio`.
public struct CompteursRadio: Codable, Sendable, Equatable {
    public struct Radio: Codable, Sendable, Equatable {
        public var configs: Int?
        public var reconfSilence: Int?
        public var reconfTx: Int?
        public var verifRatees: Int?
        public var rearm: Int?
        public var rearmHorsRx: Int?
        public var brutes: Int?
        public var bascules: Int?
    }

    public struct Garde: Codable, Sendable, Equatable {
        public var active: Bool?
        public var gardes: Int?
        public var refus: Int?
        public var attentes: Int?
        public var plafonnees: Int?
        public var maxUs: Int?
    }

    public struct Relances: Codable, Sendable, Equatable {
        public var total: Int?
        public var verif: Int?
        public var delais: Int?
        public var bruit: Int?
        public var sourde: Int?
    }

    public var raz: Int?
    public var radio: Radio?
    /// `null` sans garde d'antenne.
    public var garde: Garde?
    public var relances: Relances?
}

/// `compteurs`, bloc `matter` (absent en diag).
public struct CompteursMatter: Codable, Sendable, Equatable {
    public var fenetres: Int?
    public var ignorees: Int?
    public var aAppuis: Int?
    public var aRefuses: Int?
    public var aEntendus: Int?
    public var aPerdus: Int?
    public var reflets: Int?
    public var ecritures: Int?
    public var echecs: Int?
    public var verrou: Int?
    public var tracesPerdues: Int?
    public var identify: Int?
}

// MARK: - reseau (5.5)

/// `reseau`, bloc `thread`.
public struct ReseauThread: Codable, Sendable, Equatable {
    public struct Matter: Codable, Sendable, Equatable {
        public var enService: Bool?
        public var connecte: Bool?
        public var reseau: String?
        public var wifi: Bool?
        public var fabriques: Int?
        public var codeManuel: String?
        public var qr: String?
    }

    public struct Thread: Codable, Sendable, Equatable {
        public struct Mle: Codable, Sendable, Equatable {
            public var attaches: Int?
            public var detache: Int?
            public var enfant: Int?
            public var routeur: Int?
            public var chef: Int?
            public var parentChange: Int?
        }

        public struct Srp: Codable, Sendable, Equatable {
            public var client: Bool?
            public var hote: String?
            public var services: Int?
            public var enregistres: Int?
            public var serveur: String?
            public var port: Int?
        }

        public var role: String?
        public var canal: Int?
        public var mhz: Int?
        public var pan: String?
        public var txDbm: Int?
        public var parentRssi: Int?
        public var mode: String?
        public var typeBoot: String?
        public var typeSuivant: String?
        public var pretMs: Int?
        public var roles: Int?
        public var mle: Mle?
        public var srp: Srp?
    }

    public var fraisMs: Int?
    public var matter: Matter?
    /// Absent dans un build Wi-Fi.
    public var thread: Thread?
}

/// `reseau`, bloc `ip` (build Thread, revision 2) : nom d'hote SRP, adresses du
/// noeud, transport reseau de l'app (section 5.5).
public struct ReseauIp: Codable, Sendable, Equatable {
    public struct Srp: Codable, Sendable, Equatable {
        public var nom: String?
    }

    public struct Adresse: Codable, Sendable, Equatable {
        public var adr: String?
        /// `omr` (joignable du LAN), `ml_eid` (maillage seulement), `autre`.
        public var type: String?
        public var pref: Bool?
    }

    public struct Udp: Codable, Sendable, Equatable {
        public var port: Int?
        public var ouvert: Bool?
        public var empreinte: String?
        public var sessions: Int?
        public var provisoire: Bool?
        public var rx: Int?
        public var rejets: Int?
        public var rxPerdus: Int?
        public var defis: Int?
        public var tx: Int?
        public var txPerdus: Int?
        public var txErreurs: Int?
        public var tamponsLibres: Int?
        public var tamponsMin: Int?
    }

    public var fraisMs: Int?
    public var srp: Srp?
    public var adresses: [Adresse]?
    public var udp: Udp?

    /// Adresse joignable du LAN (OMR), la preferee d'abord.
    public var adresseOmr: String? {
        let omr = (adresses ?? []).filter { $0.type == "omr" }
        return (omr.first { $0.pref == true } ?? omr.first)?.adr
    }
}

/// `reseau`, bloc `abonnements`.
public struct ReseauAbonnements: Codable, Sendable, Equatable {
    public struct Abonnements: Codable, Sendable, Equatable {
        public var actifs: Int?
        public var lectures: Int?
        public var sauves: Int?
        public var demandes: Int?
        public var neufs: Int?
        public var reprisPont: Int?
        public var reprisPile: Int?
        public var termines: Int?
        public var plafondS: Int?
        public var plafonnes: Int?
        public var repriseAuto: Bool?
    }

    public struct Reprise: Codable, Sendable, Equatable {
        public var passages: Int?
        public var auto: Int?
        public var sessions: Int?
        public var ouvertes: Int?
        public var echecs: Int?
        public var sansNouvelles: Int?
        public var reprises: Int?
        public var enCours: ValeurScalaire?
    }

    public var fraisMs: Int?
    public var abonnements: Abonnements?
    public var reprise: Reprise?
}
