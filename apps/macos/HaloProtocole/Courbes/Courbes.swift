import Foundation

/// Compteurs cumulatifs suivis pour les courbes (section 8).
public enum Grandeur: String, CaseIterable, Sendable, Hashable {
    // bloc compteurs.pilote
    case paquets, accuses, maxRt, delais, fifo
    case abandons
    case trames, crcFaux
    // bloc compteurs.radio
    case rearmHorsRx, gardeRefus
    case relances, relancesVerif, relancesDelais, relancesBruit, relancesSourde
}

/// Un bloc `compteurs` date : les valeurs cumulatives presentes.
public struct Echantillon: Sendable, Equatable {
    public var date: Date
    public var boot: String?
    public var raz: Int?
    public var valeurs: [Grandeur: Int]

    public init(date: Date, boot: String?, raz: Int?, valeurs: [Grandeur: Int]) {
        self.date = date
        self.boot = boot
        self.raz = raz
        self.valeurs = valeurs
    }

    public static func pilote(_ c: CompteursPilote, date: Date, boot: String?) -> Echantillon {
        var v: [Grandeur: Int] = [:]
        v[.paquets] = c.tx?.paquets
        v[.accuses] = c.tx?.accuses
        v[.maxRt] = c.tx?.maxRt
        v[.delais] = c.tx?.delais
        v[.fifo] = c.tx?.fifo
        v[.abandons] = c.tranches?.abandons
        v[.trames] = c.rx?.trames
        v[.crcFaux] = c.rx?.crcFaux
        return Echantillon(date: date, boot: boot, raz: c.raz, valeurs: v)
    }

    public static func radio(_ c: CompteursRadio, date: Date, boot: String?) -> Echantillon {
        var v: [Grandeur: Int] = [:]
        v[.rearmHorsRx] = c.radio?.rearmHorsRx
        v[.gardeRefus] = c.garde?.refus
        v[.relances] = c.relances?.total
        v[.relancesVerif] = c.relances?.verif
        v[.relancesDelais] = c.relances?.delais
        v[.relancesBruit] = c.relances?.bruit
        v[.relancesSourde] = c.relances?.sourde
        return Echantillon(date: date, boot: boot, raz: c.raz, valeurs: v)
    }
}

/// Differences entre deux echantillons d'un meme segment.
public struct Difference: Sendable, Equatable {
    public var debut: Date
    public var fin: Date
    public var segment: Int
    public var deltas: [Grandeur: Int]

    public var duree: TimeInterval { fin.timeIntervalSince(debut) }

    public subscript(_ g: Grandeur) -> Int? { deltas[g] }
}

/// Point d'une courbe cumulative (escalier).
public struct PointCumul: Sendable, Equatable {
    public var date: Date
    public var segment: Int
    public var valeur: Int
}

/// Calculs de la section 8 : differences par fenetre de 10 s ou 1 min ; une
/// difference negative, un `raz`, un `boot` qui change ou un trou dans les
/// echantillons (app suspendue, veille du Mac) ouvre un nouveau segment (pas
/// de valeur aberrante).
public enum Courbes {
    /// Plus grand ecart tolere entre deux echantillons d'un meme segment.
    public static let ecartMaxDefaut: TimeInterval = 30

    /// Ecart maximal pour une periode `compteurs_ms` : 3 periodes, 30 s au moins.
    public static func ecartMax(compteursMs: Int?) -> TimeInterval {
        guard let p = compteursMs, p > 0 else { return ecartMaxDefaut }
        return max(ecartMaxDefaut, 3 * Double(p) / 1000)
    }

    /// Decoupe en segments continus.
    public static func segmenter(_ echantillons: [Echantillon],
                                 ecartMax: TimeInterval = ecartMaxDefaut) -> [[Echantillon]] {
        var segments: [[Echantillon]] = []
        var courant: [Echantillon] = []
        for e in echantillons {
            if let p = courant.last, rupture(p, e, ecartMax: ecartMax) {
                segments.append(courant)
                courant = []
            }
            courant.append(e)
        }
        if !courant.isEmpty { segments.append(courant) }
        return segments
    }

    /// Vrai si `b` ne peut pas suivre `a` dans un meme segment. Un trou de
    /// plus de `ecartMax` en ferait une seule difference geante, tracee comme
    /// une valeur "par fenetre" (fausse surdite, abandons gonfles).
    public static func rupture(_ a: Echantillon, _ b: Echantillon, ecartMax: TimeInterval = ecartMaxDefaut) -> Bool {
        if a.boot != nil, b.boot != nil, a.boot != b.boot { return true }
        if a.raz != b.raz { return true }
        if b.date < a.date { return true }
        if b.date.timeIntervalSince(a.date) > ecartMax { return true }
        for (g, v) in b.valeurs {
            if let w = a.valeurs[g], v < w { return true }
        }
        return false
    }

    /// Differences par fenetre alignee sur l'horloge (`fenetre` secondes) :
    /// pour chaque fenetre, dernier echantillon de la fenetre moins dernier
    /// echantillon de la fenetre precedente du meme segment (ou le premier
    /// echantillon du segment).
    public static func differences(_ echantillons: [Echantillon], fenetre: TimeInterval,
                                   ecartMax: TimeInterval = ecartMaxDefaut) -> [Difference] {
        precondition(fenetre > 0)
        var sortie: [Difference] = []
        for (numero, segment) in segmenter(echantillons, ecartMax: ecartMax).enumerated() {
            guard var reference = segment.first else { continue }
            var i = 0
            while i < segment.count {
                let seau = floor(segment[i].date.timeIntervalSinceReferenceDate / fenetre)
                var dernier = segment[i]
                var j = i + 1
                while j < segment.count, floor(segment[j].date.timeIntervalSinceReferenceDate / fenetre) == seau {
                    dernier = segment[j]
                    j += 1
                }
                if dernier.date > reference.date {
                    sortie.append(Difference(debut: reference.date, fin: dernier.date, segment: numero,
                                             deltas: soustraire(dernier, reference)))
                }
                reference = dernier
                i = j
            }
        }
        return sortie
    }

    static func soustraire(_ b: Echantillon, _ a: Echantillon) -> [Grandeur: Int] {
        var d: [Grandeur: Int] = [:]
        for (g, v) in b.valeurs {
            if let w = a.valeurs[g] { d[g] = v - w }
        }
        return d
    }

    /// Valeurs cumulatives brutes, avec leur segment.
    public static func cumul(_ echantillons: [Echantillon], _ g: Grandeur,
                             ecartMax: TimeInterval = ecartMaxDefaut) -> [PointCumul] {
        var sortie: [PointCumul] = []
        for (numero, segment) in segmenter(echantillons, ecartMax: ecartMax).enumerated() {
            for e in segment {
                if let v = e.valeurs[g] { sortie.append(PointCumul(date: e.date, segment: numero, valeur: v)) }
            }
        }
        return sortie
    }

    // MARK: - Courbes de la section 8

    /// Taux de perte TX : `(d max_rt + d delais + d fifo) / d paquets`.
    public static func tauxPerte(_ d: Difference) -> Double? {
        guard let p = d[.paquets], p > 0 else { return nil }
        let pertes = (d[.maxRt] ?? 0) + (d[.delais] ?? 0) + (d[.fifo] ?? 0)
        return Double(pertes) / Double(p)
    }

    /// Complement : `1 - d accuses / d paquets`.
    public static func tauxSansAccuse(_ d: Difference) -> Double? {
        guard let p = d[.paquets], p > 0, let a = d[.accuses] else { return nil }
        return 1 - Double(a) / Double(p)
    }

    /// Une grandeur ramenee a la minute.
    public static func parMinute(_ d: Difference, _ g: Grandeur) -> Double? {
        guard let v = d[g], d.duree > 0 else { return nil }
        return Double(v) * 60 / d.duree
    }

    /// `d rx.crc_faux / d rx.trames`.
    public static func partCrcFaux(_ d: Difference) -> Double? {
        guard let t = d[.trames], t > 0, let c = d[.crcFaux] else { return nil }
        return Double(c) / Double(t)
    }

    /// Seuil du deluge ramene a la minute, pour tracer la ligne de
    /// declenchement sur la courbe des CRC faux : le firmware declenche a
    /// `deluge_trames` trames dont `deluge_pct` % de CRC faux sur `fenetre_ms`
    /// (`halo1_watch.cpp`), soit au moins trames x pct / 100 CRC faux par fenetre
    /// (100 et 90 % sur 10 s : 540 par minute).
    public static func seuilDelugeParMinute(trames: Int, pct: Int, fenetreMs: Int) -> Double? {
        guard fenetreMs > 0 else { return nil }
        return Double(trames) * Double(pct) / 100 * 60_000 / Double(fenetreMs)
    }

    /// Seuil de surdite (`sourd_hors_rx` sur 10 s) ramene a la minute.
    public static func seuilSurditeParMinute(horsRx: Int, fenetreS: Double = 10) -> Double {
        Double(horsRx) * 60 / fenetreS
    }
}
