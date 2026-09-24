import Foundation
import HaloProtocole

/// Famille d'un evenement, pour les filtres de l'ecran Trames.
enum CategorieTrame: String, CaseIterable, Identifiable, Sendable {
    case rx, tx, livraison, relance, module, matter, voyant, reponse, session

    var id: String { rawValue }

    var libelle: String {
        switch self {
        case .rx: "Reçues (rx)"
        case .tx: "Émises (tx)"
        case .livraison: "Livraisons"
        case .relance: "Relances"
        case .module: "Module"
        case .matter: "Matter / Thread"
        case .voyant: "Voyant"
        case .reponse: "Réponses"
        case .session: "Session, log"
        }
    }

    static func de(_ m: MessageCarte) -> CategorieTrame {
        switch m {
        case .rx: .rx
        case .tx: .tx
        case .livraison: .livraison
        case .relance: .relance
        case .module: .module
        case .intent, .abonnement, .thread: .matter
        case .led: .voyant
        case .reponse: .reponse
        default: .session
        }
    }
}

/// Un evenement de la carte (tout sauf les instantanes periodiques).
struct EntreeTrame: Identifiable, Sendable {
    let id: Int
    let date: Date
    let n: UInt32
    let ms: UInt32?
    let type: String
    let categorie: CategorieTrame
    let message: MessageCarte
    let json: String
    /// Ligne ancienne, restee dans le tampon avant le `hello` de la session.
    let historique: Bool
    let resume: String
    /// `resume` et `json` en minuscules, calcules une fois : la recherche ne
    /// refait pas 5000 conversions a chaque evenement recu.
    let cleRecherche: String

    /// Trame a bits douteux (CRC faux) : affichee en gris.
    var douteuse: Bool {
        if case .rx(let r) = message { return r.type == .crcFaux }
        return false
    }

    var echec: Bool {
        switch message {
        case .tx(let t): t.verdict.estEchec
        case .livraison(let l): l.issue == .abandon
        case .relance(let r): r.ok == false || r.panne == true
        case .module(let m): m.etat == .panne || m.etat == .perdu || m.etat == .configRejetee
        case .reponse(let r): !r.ok
        default: false
        }
    }
}

/// Ligne de la console.
struct LigneConsole: Identifiable, Sendable {
    enum Genre: Equatable, Sendable {
        /// Ligne envoyee par l'app.
        case envoi(OrigineCommande)
        /// Texte recu, classe (2.5).
        case texte(ClasseTexte)
        case fragment
        /// `reponse` ou `livraison` rendue lisible.
        case retour(ok: Bool, session: Bool)
        /// Message `log` (mode `json log 1`).
        case log
        /// Annonce de l'app elle-meme.
        case note(grave: Bool)
    }

    let id: Int
    let date: Date
    let genre: Genre
    let texte: String
    /// `id` de la commande a laquelle la ligne se rattache.
    let numero: Int?
}

/// Ligne machine rejetee (diagnostic du tableau de bord).
struct Rejet: Identifiable, Sendable {
    let id: Int
    let date: Date
    let raison: String
    let brut: String
}

/// Marqueur sur les courbes.
struct Marqueur: Identifiable, Sendable {
    enum Genre: String, Sendable {
        case relance = "Relance"
        case module = "Module"
        case abandon = "Abandon"
        case role = "Rôle Thread"
        case redemarrage = "Redémarrage"
    }

    let id: Int
    let date: Date
    let genre: Genre
    let texte: String
}

/// Mesure ponctuelle (abonnes actifs, RSSI du parent).
struct PointMesure: Identifiable, Sendable {
    let id: Int
    let date: Date
    let valeur: Double?
}

/// Tableau borne : les plus anciens sortent.
struct Borne<Element> {
    private(set) var elements: [Element] = []
    let capacite: Int

    init(capacite: Int) { self.capacite = capacite }

    mutating func ajouter(_ e: Element) {
        elements.append(e)
        if elements.count > capacite + capacite / 10 { elements.removeFirst(elements.count - capacite) }
    }

    mutating func vider() { elements.removeAll() }
}

extension Borne: Sendable where Element: Sendable {}
