import Foundation

/// Correspondance niveau Matter <-> luminosite brute, et mireds <-> temperature
/// brute : portage exact de `src/halo1_map.cpp` (`mapInit`, `rawFromLevel`,
/// `levelFromRaw`, `tempFromMired`, `miredFromTemp`).
public struct CorrespondanceLuminosite: Sendable, Equatable {
    public static let brutMin = 0x4C
    public static let brutMax = 0xFE
    public static let tempMax = 100
    public static let miredFroid = 153
    public static let miredChaud = 370
    public static let plancherParDefaut = 4

    public let gamma: Double
    public let plancher: Int
    /// Indice = niveau Matter 0..254.
    private let table: [Int]

    /// `gammaC` : gamma en centiemes (`config.reglages.gamma_c`), 200 = 2,00.
    public init(gammaC: Int?, plancher: Int? = nil) {
        self.init(gamma: Double(gammaC ?? 200) / 100.0, plancher: plancher ?? Self.plancherParDefaut)
    }

    public init(gamma: Double = 2.0, plancher: Int = CorrespondanceLuminosite.plancherParDefaut) {
        let g = gamma > 0 ? gamma : 1.0
        self.gamma = g
        self.plancher = max(1, min(254, plancher))
        var t = [Int](repeating: 0, count: 255)
        for l in 1...254 {
            var v: Int
            if g == 1.0 {
                v = ((l - 1) * 178 + 126) / 253
            } else {
                v = Int(floor(178.0 * pow(Double(l - 1) / 253.0, g) + 0.5))
            }
            if v > 178 { v = 178 }
            if l <= self.plancher { v = 0 }
            t[l] = Self.brutMin + v
            if l > 1 && t[l] < t[l - 1] { t[l] = t[l - 1] }
        }
        t[0] = t[1]
        table = t
    }

    /// Niveau Matter 0..254 -> luminosite brute 0x4C..0xFE.
    public func brut(niveau: Int) -> Int {
        table[max(0, min(254, niveau))]
    }

    /// Niveau rapporte : plus petit L >= plancher tel que brut(L) >= brut.
    public func niveau(brut: Int) -> Int {
        var lo = plancher
        var hi = 254
        while lo < hi {
            let mid = (lo + hi) / 2
            if table[mid] >= brut { hi = mid } else { lo = mid + 1 }
        }
        return lo
    }

    /// `((clamp(m,153,370) - 153) * 100 + 108) / 217`
    public static func temp(mired: Int) -> Int {
        let m = max(miredFroid, min(miredChaud, mired))
        return ((m - miredFroid) * 100 + 108) / 217
    }

    /// `153 + (min(t,100) * 217 + 50) / 100`
    public static func mired(temp: Int) -> Int {
        let t = max(0, min(tempMax, temp))
        return miredFroid + (t * 217 + 50) / 100
    }

    /// Kelvin nominaux (non mesures) d'une valeur en mireds.
    public static func kelvin(mired: Int) -> Int {
        guard mired > 0 else { return 0 }
        return Int((1_000_000.0 / Double(mired)).rounded())
    }

    /// Pourcentage affiche par Apple Home (approximation L/254).
    public static func pourcent(niveau: Int) -> Int {
        Int((Double(niveau) * 100.0 / 254.0).rounded())
    }
}
