import Foundation

/// Classe d'une ligne de texte (hors RS), section 2.5.
public enum ClasseTexte: String, Sendable, Equatable, CaseIterable {
    /// `E (12345) tag: ...`
    case logIDF
    /// `[    1234][E][fichier.cpp:12] ...`
    case logArduino
    /// `[lampe] ...`, `[matter] ...`
    case annonce
    /// ROM, banniere : indice de redemarrage.
    case demarrage
    /// `> ` : invite de la CLI en mode humain (ignoree).
    case invite
    /// Tout le reste : texte des commandes.
    case commande

    /// Filtre "logs systeme" de la console.
    public var estLogSysteme: Bool { self == .logIDF || self == .logArduino }
}

/// Ligne de texte classee.
public struct LigneTexte: Sendable, Equatable {
    /// Texte sans les sequences ANSI.
    public var texte: String
    public var classe: ClasseTexte
    /// Niveau d'un log (`E`, `W`, `I`, `D`, `V`).
    public var niveau: Character?

    public init(texte: String, classe: ClasseTexte, niveau: Character? = nil) {
        self.texte = texte
        self.classe = classe
        self.niveau = niveau
    }
}

public enum ClasseurTexte {
    private static let niveaux: Set<Character> = ["E", "W", "I", "D", "V"]

    public static func classer(_ brut: String) -> LigneTexte {
        let texte = sansANSI(brut)
        if let niv = niveauLogIDF(texte) { return LigneTexte(texte: texte, classe: .logIDF, niveau: niv) }
        if let niv = niveauLogArduino(texte) { return LigneTexte(texte: texte, classe: .logArduino, niveau: niv) }
        if texte.hasPrefix("[lampe] ") || texte.hasPrefix("[matter] ") {
            return LigneTexte(texte: texte, classe: .annonce)
        }
        if estDemarrage(texte) { return LigneTexte(texte: texte, classe: .demarrage) }
        if texte == ">" || texte == "> " { return LigneTexte(texte: texte, classe: .invite) }
        return LigneTexte(texte: texte, classe: .commande)
    }

    /// Retire les sequences `ESC [ ... lettre`.
    public static func sansANSI(_ s: String) -> String {
        guard s.contains("\u{1B}") else { return s }
        var sortie = String.UnicodeScalarView()
        var it = s.unicodeScalars.makeIterator()
        while let c = it.next() {
            if c == "\u{1B}" {
                guard let suivant = it.next() else { break }
                if suivant == "[" {
                    while let x = it.next() {
                        if (x.value >= 0x41 && x.value <= 0x5A) || (x.value >= 0x61 && x.value <= 0x7A) { break }
                    }
                }
                continue
            }
            sortie.append(c)
        }
        return String(sortie)
    }

    /// `^[EWIDV] \(\d+\) [^:]+: `
    static func niveauLogIDF(_ s: String) -> Character? {
        let c = Array(s)
        guard c.count >= 8, niveaux.contains(c[0]), c[1] == " ", c[2] == "(" else { return nil }
        var i = 3
        var chiffres = 0
        while i < c.count, c[i].isASCII, c[i].isNumber { i += 1; chiffres += 1 }
        guard chiffres > 0, i + 1 < c.count, c[i] == ")", c[i + 1] == " " else { return nil }
        i += 2
        var tag = 0
        while i < c.count, c[i] != ":" { i += 1; tag += 1 }
        guard tag > 0, i + 1 < c.count, c[i] == ":", c[i + 1] == " " else { return nil }
        return c[0]
    }

    /// `^\[\s*\d+\]\[[EWIDV]\]\[`
    static func niveauLogArduino(_ s: String) -> Character? {
        let c = Array(s)
        guard c.first == "[" else { return nil }
        var i = 1
        while i < c.count, c[i] == " " || c[i] == "\t" { i += 1 }
        var chiffres = 0
        while i < c.count, c[i].isASCII, c[i].isNumber { i += 1; chiffres += 1 }
        guard chiffres > 0, i + 4 < c.count, c[i] == "]", c[i + 1] == "[", niveaux.contains(c[i + 2]),
              c[i + 3] == "]", c[i + 4] == "[" else { return nil }
        return c[i + 2]
    }

    static func estDemarrage(_ s: String) -> Bool {
        s.hasPrefix("ESP-ROM:") || s.hasPrefix("rst:0x") || s.hasPrefix("boot:0x")
            || s.hasPrefix("=== BenQ ScreenBar Halo")
    }

    /// Ancien firmware (3.3) : `^Commande inconnue : "id=\d+"`.
    public static func estRefusIdAncienFirmware(_ s: String) -> Bool {
        let prefixe = "Commande inconnue : \"id="
        guard s.hasPrefix(prefixe) else { return false }
        let reste = s.dropFirst(prefixe.count)
        let chiffres = reste.prefix { $0.isASCII && $0.isNumber }
        guard !chiffres.isEmpty else { return false }
        return reste.dropFirst(chiffres.count).first == "\""
    }
}
