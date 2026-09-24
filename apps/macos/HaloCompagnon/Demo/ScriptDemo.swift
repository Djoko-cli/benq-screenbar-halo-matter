import Foundation

/// Chronologie du mode demo, lue dans `demo-halo.jsonl` (voir Outils/generer_demo.py).
struct ScriptDemo: Sendable {
    enum Genre: Sendable {
        /// Ligne machine telle que la carte l'emettrait (sans RS ni LF).
        case machine(ligne: String, t: String, bloc: String?)
        case texte(String)
        case lampeDebranchee(Bool)
        /// La ligne machine suivante est coupee par ce log IDF (2.1).
        case ligneCoupee(log: String)
        /// Lignes produites mais perdues (trou dans `n`).
        case sautN(Int)
    }

    struct Element: Sendable {
        let ms: Int
        let genre: Genre
    }

    /// Lignes de l'instantane de connexion (hello, config, etat, compteurs, reseau de 12.1).
    let instantane: [Element]
    /// Tout le reste, dans l'ordre.
    let suite: [Element]
    /// `ms` de la carte au debut du fichier.
    let msDebut: Int
    /// `boot` ecrit dans le fichier (remplace a chaque "redemarrage" de la demo).
    let boot: String

    static func charger() throws -> ScriptDemo {
        guard let url = Bundle.main.url(forResource: "demo-halo", withExtension: "jsonl") else {
            throw ErreurDemo(tr("demo-halo.jsonl absent du paquet de l'app"))
        }
        return try ScriptDemo(texte: String(contentsOf: url, encoding: .utf8))
    }

    init(texte: String) throws {
        var elements: [Element] = []
        var boot = "3FA2C901"
        for ligne in texte.split(separator: "\n") where !ligne.isEmpty {
            guard let objet = try JSONSerialization.jsonObject(with: Data(ligne.utf8)) as? [String: Any],
                  let ms = objet["ms"] as? Int else { throw ErreurDemo(tr("ligne illisible : \(String(ligne.prefix(60)))")) }
            if let t = objet["t"] as? String, objet["v"] != nil {
                if t == "hello", let b = objet["boot"] as? String { boot = b }
                elements.append(Element(ms: ms, genre: .machine(ligne: String(ligne), t: t, bloc: objet["bloc"] as? String)))
            } else if let texte = objet["texte"] as? String {
                elements.append(Element(ms: ms, genre: .texte(texte)))
            } else if let directive = objet["demo"] as? String {
                switch directive {
                case "lampe_debranchee": elements.append(Element(ms: ms, genre: .lampeDebranchee(true)))
                case "lampe_rebranchee": elements.append(Element(ms: ms, genre: .lampeDebranchee(false)))
                case "ligne_coupee":
                    elements.append(Element(ms: ms, genre: .ligneCoupee(log: objet["log"] as? String ?? "E (0) demo: coupure")))
                case "saut_n": elements.append(Element(ms: ms, genre: .sautN(objet["nombre"] as? Int ?? 1)))
                default: break  // entete et directives futures
                }
            }
        }
        guard let premier = elements.first?.ms else { throw ErreurDemo(tr("chronologie vide")) }
        // L'instantane : les lignes periodiques des toutes premieres millisecondes.
        let limite = premier + 50
        var instantane: [Element] = []
        var i = 0
        while i < elements.count, elements[i].ms <= limite, case .machine = elements[i].genre {
            instantane.append(elements[i])
            i += 1
        }
        self.instantane = instantane
        self.suite = Array(elements[i...])
        self.msDebut = premier
        self.boot = boot
    }
}

struct ErreurDemo: Error, CustomStringConvertible {
    var description: String
    init(_ d: String) { description = d }
}
