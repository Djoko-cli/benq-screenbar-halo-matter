import Foundation

/// Derniere valeur d'un (`t`, `bloc`), avec sa date.
public struct Instantane<Valeur: Sendable & Equatable>: Sendable, Equatable {
    public var valeur: Valeur
    public var n: UInt32
    public var ms: UInt32?
    /// Date deduite de `ms` et de l'ancre du `hello` (jamais l'heure d'arrivee,
    /// sauf avant le premier `hello`).
    public var date: Date
}

/// Ce que l'app sait de la carte : la derniere valeur de chaque instantane
/// (section 5 : l'app remplace les valeurs d'un (`t`, `bloc`) a chaque
/// reception) et les indices tires des evenements.
public struct EtatPont: Sendable, Equatable {
    public private(set) var helloBase: Instantane<HelloBase>?
    public private(set) var identite: Instantane<HelloIdentite>?
    public private(set) var config: Instantane<ConfigCarte>?
    public private(set) var lampe: Instantane<BlocEtatLampe>?
    public private(set) var tranches: Instantane<BlocTranches>?
    public private(set) var sante: Instantane<BlocSante>?
    public private(set) var pilote: Instantane<CompteursPilote>?
    public private(set) var radio: Instantane<CompteursRadio>?
    public private(set) var compteursMatter: Instantane<CompteursMatter>?
    public private(set) var thread: Instantane<ReseauThread>?
    public private(set) var abonnements: Instantane<ReseauAbonnements>?
    public private(set) var ip: Instantane<ReseauIp>?
    public private(set) var battement: Instantane<Battement>?

    /// Motif du voyant : bloc `sante` ou evenement `led`, le plus recent.
    public private(set) var motifLed: MotifLed?
    public private(set) var motifLedDepuis: Date?
    public private(set) var ledTest = false
    public private(set) var derniereLivraison: Instantane<Livraison>?
    public private(set) var derniereRelance: Instantane<Relance>?
    public private(set) var dernierModule: Instantane<EvenementModule>?

    /// Ancre du temps : heure locale <-> `ms` de la carte, posee a la reception d'un `hello`.
    public private(set) var ancre: (date: Date, ms: UInt32)?

    public init() {}

    public static func == (a: EtatPont, b: EtatPont) -> Bool {
        a.helloBase == b.helloBase && a.identite == b.identite && a.config == b.config && a.lampe == b.lampe
            && a.tranches == b.tranches && a.sante == b.sante && a.pilote == b.pilote && a.radio == b.radio
            && a.compteursMatter == b.compteursMatter && a.thread == b.thread && a.abonnements == b.abonnements && a.ip == b.ip
            && a.battement == b.battement && a.motifLed == b.motifLed && a.ledTest == b.ledTest
            && a.derniereLivraison == b.derniereLivraison && a.derniereRelance == b.derniereRelance
            && a.dernierModule == b.dernierModule && a.ancre?.date == b.ancre?.date && a.ancre?.ms == b.ancre?.ms
    }

    /// Date d'une ligne : `ms` rapporte a l'ancre (ecart signe sur 32 bits,
    /// juste a travers le retour a zero de `millis()`).
    public func dater(ms: UInt32?, recueA: Date) -> Date {
        guard let ms, let ancre else { return recueA }
        let ecart = Int32(bitPattern: ms &- ancre.ms)
        return ancre.date.addingTimeInterval(Double(ecart) / 1000)
    }

    /// Applique une ligne machine ; renvoie sa date.
    @discardableResult
    public mutating func appliquer(_ l: LigneMachine, recueA: Date) -> Date {
        let e = l.enveloppe
        if case .helloBase = l.message, let ms = e.ms { ancre = (recueA, ms) }
        let date = dater(ms: e.ms, recueA: recueA)
        func inst<T>(_ v: T) -> Instantane<T> { Instantane(valeur: v, n: e.n, ms: e.ms, date: date) }
        switch l.message {
        case .helloBase(let v): helloBase = inst(v)
        case .helloIdentite(let v): identite = inst(v)
        case .config(let v): config = inst(v)
        case .etatLampe(let v): lampe = inst(v)
        case .etatTranches(let v): tranches = inst(v)
        case .etatSante(let v):
            sante = inst(v)
            if let m = v.led?.motif, m != motifLed {
                motifLed = m
                motifLedDepuis = date
            }
            ledTest = v.led?.test ?? false
        case .compteursPilote(let v): pilote = inst(v)
        case .compteursRadio(let v): radio = inst(v)
        case .compteursMatter(let v): compteursMatter = inst(v)
        case .reseauThread(let v): thread = inst(v)
        case .reseauAbonnements(let v): abonnements = inst(v)
        case .reseauIp(let v): ip = inst(v)
        case .battement(let v): battement = inst(v)
        case .led(let v):
            motifLed = v.motif
            motifLedDepuis = date
            ledTest = v.test ?? ledTest
        case .livraison(let v): derniereLivraison = inst(v)
        case .relance(let v): derniereRelance = inst(v)
        case .module(let v): dernierModule = inst(v)
        default: break
        }
        return date
    }

    /// Redemarrage : les etats derives sont vides (3.6) ; l'ancre reste
    /// jusqu'au prochain `hello`.
    public mutating func viderDerives() {
        let a = ancre
        self = EtatPont()
        ancre = a
    }

    // MARK: - Lectures pratiques

    public var boot: String? {
        helloBase?.valeur.boot ?? lampe?.valeur.boot ?? battement?.valeur.boot
    }

    /// Secondes depuis le demarrage : la plus recente des valeurs connues.
    public var upS: Int? {
        [helloBase?.valeur.upS, lampe?.valeur.upS, sante?.valeur.upS, battement?.valeur.upS]
            .compactMap { $0 }.max()
    }

    public var capacites: Set<String> { Set(identite?.valeur.caps ?? []) }

    /// Correspondance niveau <-> brut au gamma de la carte (`config.reglages.gamma_c`).
    public var correspondance: CorrespondanceLuminosite {
        CorrespondanceLuminosite(gammaC: config?.valeur.reglages?.gammaC,
                                 plancher: config?.valeur.matter?.niveauPlancher)
    }

    public var enPanne: Bool { sante?.valeur.surveil?.panne ?? false }
}
