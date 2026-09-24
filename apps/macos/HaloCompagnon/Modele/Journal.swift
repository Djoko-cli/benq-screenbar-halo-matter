import Foundation
import HaloProtocole

/// Famille d'un evenement, pour les filtres de l'ecran Trames.
enum CategorieTrame: String, CaseIterable, Identifiable, Sendable {
    case rx, tx, livraison, relance, module, matter, voyant, reponse, session

    var id: String { rawValue }

    var libelle: String {
        switch self {
        case .rx: tr("Reçues (rx)")
        case .tx: tr("Émises (tx)")
        case .livraison: tr("Livraisons")
        case .relance: tr("Relances")
        case .module: tr("Module")
        case .matter: tr("Matter / Thread")
        case .voyant: tr("Voyant")
        case .reponse: tr("Réponses")
        case .session: tr("Session, log")
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

/// Reglages de la carte dont depend le sens decode d'une trame (niveau Matter
/// d'une luminosite brute) : gardes pour le recalculer dans une autre langue.
struct ParametresGamma: Hashable, Sendable {
    let gammaC: Int?
    let plancher: Int?

    init(_ etat: EtatPont) {
        gammaC = etat.config?.valeur.reglages?.gammaC
        plancher = etat.config?.valeur.matter?.niveauPlancher
    }

    var correspondance: CorrespondanceLuminosite { CorrespondanceLuminosite(gammaC: gammaC, plancher: plancher) }
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
    let gamma: ParametresGamma
    /// Sens decode, dans la langue en vigueur a son calcul (recalcule quand elle change).
    private(set) var resume: String
    /// `resume` et `json` en minuscules, calcules une fois : la recherche ne
    /// refait pas 5000 conversions a chaque evenement recu.
    private(set) var cleRecherche: String

    init(id: Int, date: Date, n: UInt32, ms: UInt32?, type: String, message: MessageCarte, json: String,
         historique: Bool, gamma: ParametresGamma, correspondance: CorrespondanceLuminosite) {
        self.id = id
        self.date = date
        self.n = n
        self.ms = ms
        self.type = type
        categorie = .de(message)
        self.message = message
        self.json = json
        self.historique = historique
        self.gamma = gamma
        resume = ""
        cleRecherche = ""
        localiser(correspondance)
    }

    private mutating func localiser(_ c: CorrespondanceLuminosite) {
        resume = Interpretation.resume(message, correspondance: c)
        cleRecherche = (resume + "\n" + json).lowercased()
    }

    /// La meme entree, son sens decode dans la langue en vigueur.
    func relocalisee(correspondance c: CorrespondanceLuminosite) -> EntreeTrame {
        var e = self
        e.localiser(c)
        return e
    }

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
    enum Genre: Sendable {
        case relance, module, abandon, role, redemarrage
    }

    /// Ce que dit le marqueur ; son texte suit la langue en vigueur.
    enum Etiquette: Sendable, Equatable {
        case relance(CauseRelance)
        case module(EtatModule)
        case abandon(CauseAbandon?)
        case role(de: String?, vers: String)
        case redemarrage(boot: String?)
    }

    let id: Int
    let date: Date
    let etiquette: Etiquette

    var genre: Genre {
        switch etiquette {
        case .relance: .relance
        case .module: .module
        case .abandon: .abandon
        case .role: .role
        case .redemarrage: .redemarrage
        }
    }

    var texte: String {
        switch etiquette {
        case .relance(let c): c.libelle
        case .module(let e): e.libelle
        case .abandon(let c): c?.libelle ?? tr("abandon")
        case .role(let de, let vers): "\(de ?? "?") → \(vers)"
        case .redemarrage(let boot): "boot \(boot ?? "?")"
        }
    }
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

    /// Remplace chaque element, dans l'ordre.
    mutating func transformer(_ f: (Element) -> Element) {
        elements = elements.map(f)
    }
}

extension Borne: Sendable where Element: Sendable {}
