import Foundation

/// Ponts deja vus par l'app, retenus par MAC : leur modele, tire du numero de
/// serie de la carte (`hello` `identite` : `id.serie` = `HALO1-<MAC>`, 5.1),
/// et leur nom SRP (bloc `ip`, 5.5). Sert a nommer un port USB avant la
/// connexion (son numero de serie USB est la MAC) et l'entree reseau du meme
/// pont : "HALO1 · 58:E6:C5:66:5B:CE".
public struct RepertoirePonts: Codable, Sendable, Equatable {
    public struct Entree: Codable, Sendable, Equatable {
        /// "HALO1" : le numero de serie sans sa MAC.
        public var modele: String?
        /// Nom SRP, sans `.local`.
        public var srp: String?
    }

    /// Cle : MAC en 12 hexa majuscules.
    public private(set) var parMac: [String: Entree] = [:]

    public init() {}

    /// MAC en 12 hexa majuscules, separateurs (":" ou "-") retires ; nil si le
    /// texte n'en est pas une.
    public static func mac(_ texte: String?) -> String? {
        guard let texte else { return nil }
        let h = texte.uppercased().filter { $0 != ":" && $0 != "-" }
        guard h.count == 12, h.allSatisfy(\.isHexDigit) else { return nil }
        return h
    }

    /// "58:E6:C5:66:5B:CE" pour "58E6C5665BCE".
    public static func macLisible(_ mac: String) -> String {
        var s = ""
        for (i, c) in mac.enumerated() {
            if i > 0, i % 2 == 0 { s.append(":") }
            s.append(c)
        }
        return s
    }

    /// Modele du numero de serie "HALO1-58E6C5665BCE" : "HALO1". nil si la
    /// serie ne finit pas par "-" et la MAC de cette carte.
    public static func modele(serie: String?, mac: String) -> String? {
        guard let serie, let tiret = serie.lastIndex(of: "-") else { return nil }
        let avant = String(serie[..<tiret])
        guard !avant.isEmpty, Self.mac(String(serie[serie.index(after: tiret)...])) == mac else { return nil }
        return avant
    }

    /// Note ce que la carte dit d'elle (`hello` `identite`, bloc `ip`) ; vrai si
    /// le repertoire change. Un nom SRP n'appartient qu'a un pont : retire a
    /// celui qui l'avait (carte remplacee, nouvelle mise en service).
    @discardableResult
    public mutating func noter(mac texte: String?, serie: String?, srp: String?) -> Bool {
        guard let mac = Self.mac(texte) else { return false }
        let avant = self
        var e = parMac[mac] ?? Entree()
        if let m = Self.modele(serie: serie, mac: mac) { e.modele = m }
        if let srp, !srp.isEmpty {
            for (autre, x) in parMac where autre != mac && x.srp == srp { parMac[autre]?.srp = nil }
            e.srp = srp
        }
        parMac[mac] = e
        return self != avant
    }

    /// MAC du pont qui porte ce nom SRP.
    public func mac(pourSrp srp: String) -> String? {
        parMac.first { $0.value.srp == srp }?.key
    }

    /// "HALO1 · 58:E6:C5:66:5B:CE" ; modele encore inconnu (carte jamais
    /// connectee) : "ESP32 · 58:E6:C5:66:5B:CE".
    public func titre(mac: String) -> String {
        "\(parMac[mac]?.modele ?? "ESP32") · \(Self.macLisible(mac))"
    }
}
