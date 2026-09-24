import Foundation

/// Petit ecrivain JSON compact qui garde l'ordre des champs, comme le
/// firmware (`v`, `t`, `n`, `ms` d'abord), pour les lignes fabriquees par le
/// mode demo.
enum JSONValeur: Sendable {
    case entier(Int)
    case booleen(Bool)
    case texte(String)
    case nul
    case tableau([JSONValeur])
    case objet([(String, JSONValeur)])

    var texteJSON: String {
        switch self {
        case .entier(let i): return String(i)
        case .booleen(let b): return b ? "true" : "false"
        case .nul: return "null"
        case .texte(let s): return JSONValeur.chaine(s)
        case .tableau(let a): return "[" + a.map(\.texteJSON).joined(separator: ",") + "]"
        case .objet(let o): return "{" + o.map { JSONValeur.chaine($0.0) + ":" + $0.1.texteJSON }.joined(separator: ",") + "}"
        }
    }

    /// Chaine ASCII : `"` et `\` echappes, tout octet hors 0x20..0x7E remplace par `?` (2.2, regle 3).
    static func chaine(_ s: String) -> String {
        var sortie = "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": sortie += "\\\""
            case "\\": sortie += "\\\\"
            default: sortie += (u.value >= 0x20 && u.value <= 0x7E) ? String(u) : "?"
            }
        }
        return sortie + "\""
    }
}

extension JSONValeur: ExpressibleByIntegerLiteral, ExpressibleByBooleanLiteral, ExpressibleByStringLiteral {
    init(integerLiteral value: Int) { self = .entier(value) }
    init(booleanLiteral value: Bool) { self = .booleen(value) }
    init(stringLiteral value: String) { self = .texte(value) }
}

enum LigneJSON {
    /// Ligne machine : `n` et `ms` valent 0 et sont reecrits a l'emission.
    static func machine(_ t: String, _ champs: [(String, JSONValeur)]) -> String {
        JSONValeur.objet([("v", 1), ("t", .texte(t)), ("n", 0), ("ms", 0)] + champs).texteJSON
    }

    /// Remplace la valeur entiere de la premiere occurrence de `"cle":`.
    static func remplacerEntier(_ ligne: String, _ cle: String, par transformation: (Int) -> Int) -> String {
        let motif = "\"\(cle)\":"
        guard let r = ligne.range(of: motif) else { return ligne }
        var fin = r.upperBound
        if fin < ligne.endIndex, ligne[fin] == "-" { fin = ligne.index(after: fin) }
        while fin < ligne.endIndex, ligne[fin].isASCII, ligne[fin].isNumber { fin = ligne.index(after: fin) }
        guard let valeur = Int(ligne[r.upperBound..<fin]) else { return ligne }
        return ligne.replacingCharacters(in: r.upperBound..<fin, with: String(transformation(valeur)))
    }

    static func remplacerEntier(_ ligne: String, _ cle: String, valeur: Int) -> String {
        remplacerEntier(ligne, cle) { _ in valeur }
    }

    /// Remplace la valeur (objet plat, tableau plat, booleen ou chaine) de la premiere occurrence de `"cle":`.
    static func remplacerValeur(_ ligne: String, _ cle: String, par nouvelle: String) -> String {
        let motif = "\"\(cle)\":"
        guard let r = ligne.range(of: motif), r.upperBound < ligne.endIndex else { return ligne }
        let debut = r.upperBound
        var fin = debut
        switch ligne[debut] {
        case "{", "[":
            let fermante: Character = ligne[debut] == "{" ? "}" : "]"
            guard let f = ligne[debut...].firstIndex(of: fermante) else { return ligne }
            fin = ligne.index(after: f)
        case "\"":
            guard let f = ligne[ligne.index(after: debut)...].firstIndex(of: "\"") else { return ligne }
            fin = ligne.index(after: f)
        default:
            while fin < ligne.endIndex, ligne[fin] != ",", ligne[fin] != "}" { fin = ligne.index(after: fin) }
        }
        return ligne.replacingCharacters(in: debut..<fin, with: nouvelle)
    }

    /// Champs entiers `"cle":<n>` de la ligne, dans l'ordre.
    static func entiers(_ ligne: String) -> [(cle: String, valeur: Int)] {
        ligne.matches(of: /"([a-z_0-9]+)":(-?[0-9]+)/).compactMap { m in
            Int(m.output.2).map { (String(m.output.1), $0) }
        }
    }

    /// Retranche `base` (relevee par `entiers` sur une ligne de meme forme) champ
    /// par champ, dans l'ordre, sans descendre sous 0 ; `sauf` : champs gardes.
    static func soustraire(_ ligne: String, base: [(cle: String, valeur: Int)], sauf: Set<String>) -> String {
        var i = 0
        return ligne.replacing(/"([a-z_0-9]+)":(-?[0-9]+)/) { m -> String in
            defer { i += 1 }
            let cle = String(m.output.1)
            guard i < base.count, base[i].cle == cle, !sauf.contains(cle), let v = Int(m.output.2) else {
                return String(m.output.0)
            }
            return "\"\(cle)\":\(max(0, v - base[i].valeur))"
        }
    }

    /// Valeur texte brute d'un champ (premiere occurrence), pour relire un bloc.
    static func valeur(_ ligne: String, _ cle: String) -> Substring? {
        let motif = "\"\(cle)\":"
        guard let r = ligne.range(of: motif) else { return nil }
        var fin = r.upperBound
        var profondeur = 0
        var dansChaine = false
        while fin < ligne.endIndex {
            let c = ligne[fin]
            if dansChaine {
                if c == "\"" { dansChaine = false }
            } else if c == "\"" {
                dansChaine = true
            } else if c == "{" || c == "[" {
                profondeur += 1
            } else if c == "}" || c == "]" {
                if profondeur == 0 { break }
                profondeur -= 1
            } else if c == ",", profondeur == 0 {
                break
            }
            fin = ligne.index(after: fin)
        }
        return ligne[r.upperBound..<fin]
    }
}
