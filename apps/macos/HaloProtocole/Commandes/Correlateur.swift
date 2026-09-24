import Foundation

/// Qui a demande la commande.
public enum OrigineCommande: String, Sendable, Equatable {
    /// Boutons et curseurs de l'ecran Commandes, tableau de bord.
    case interface
    /// Console brute.
    case console
    /// Commandes de service de l'app (`json ping`, `json etat` apres un silence).
    case session
}

/// Ou en est une commande envoyee avec un `id` (6.2 a 6.5).
public enum EtatCommande: Sendable, Equatable {
    case enFile
    case envoyee
    /// `reponse` `debut` recue : commande historique en cours (peut bloquer).
    case enCours
    /// `reponse` `fin` recue, rien d'autre a attendre.
    case terminee
    /// `accepte`, `suite` `livraison` : une `livraison` suivra.
    case attenteLivraison
    case livree
    case abandonnee
    case annulee
    /// Pas de `reponse` sous 3 s, sans `debut` (pas de reemission).
    case sansReponse
    /// Remplacee dans la file par une valeur plus recente (curseurs).
    case remplacee
    /// Connexion perdue avant la fin.
    case perdue

    public var estFinal: Bool {
        switch self {
        case .enFile, .envoyee, .enCours, .attenteLivraison: false
        default: true
        }
    }
}

/// Suivi d'une commande, de la file a la `livraison`.
public struct SuiviCommande: Sendable, Identifiable, Equatable {
    public let id: UUID
    public let commande: String
    public let origine: OrigineCommande
    /// Cle de fusion : une commande en file de meme cle est remplacee.
    public let fusion: String?
    public internal(set) var numero: Int?
    public internal(set) var etat: EtatCommande
    public internal(set) var fin: Reponse?
    public internal(set) var livraison: Livraison?
    /// Texte recu entre `reponse debut` et `reponse fin` (au mieux).
    public internal(set) var texte: [String]
    public let soumiseA: TimeInterval
    public internal(set) var envoyeeA: TimeInterval?
    public internal(set) var debutA: TimeInterval?
    public internal(set) var termineeA: TimeInterval?
}

/// Correlation des commandes par `id`, une seule en vol a la fois (6.5).
///
/// Code pur : le temps est passe en argument (secondes monotones).
public struct Correlateur: Sendable {
    public static let delaiReponse: TimeInterval = 3
    public static let historiqueMax = 300

    public private(set) var suivis: [SuiviCommande] = []
    private var file: [UUID] = []
    public private(set) var enVol: UUID?
    private var prochainNumero = 1
    public private(set) var dernierEnvoiA: TimeInterval?

    public init() {}

    /// Nouvelle connexion : les numeros repartent a 1, tout ce qui attendait est perdu.
    public mutating func reinitialiser(maintenant: TimeInterval) {
        for i in suivis.indices where !suivis[i].etat.estFinal {
            suivis[i].etat = .perdue
            suivis[i].termineeA = maintenant
        }
        file.removeAll()
        enVol = nil
        prochainNumero = 1
        dernierEnvoiA = nil
    }

    /// Reserve un numero hors file (json 1, json 0 envoyes par la session).
    public mutating func reserverNumero() -> Int {
        let n = prochainNumero
        prochainNumero = LigneCommande.suivant(n)
        return n
    }

    public func suivi(_ id: UUID) -> SuiviCommande? {
        suivis.first { $0.id == id }
    }

    public func suivi(numero: Int) -> SuiviCommande? {
        suivis.last { $0.numero == numero }
    }

    public var enFile: Int { file.count }
    public var occupe: Bool { enVol != nil || !file.isEmpty }

    /// Commande historique en cours (`debut` recu, pas encore `fin`).
    public var commandeDeBanc: SuiviCommande? {
        guard let id = enVol, let s = suivi(id), s.etat == .enCours else { return nil }
        return s
    }

    @discardableResult
    public mutating func soumettre(_ commande: String, origine: OrigineCommande, fusion: String? = nil,
                                   maintenant: TimeInterval) -> UUID {
        if let fusion {
            var gardees: [UUID] = []
            for id in file {
                if let i = index(id), suivis[i].fusion == fusion {
                    suivis[i].etat = .remplacee
                    suivis[i].termineeA = maintenant
                } else {
                    gardees.append(id)
                }
            }
            file = gardees
        }
        let s = SuiviCommande(id: UUID(), commande: commande, origine: origine, fusion: fusion, numero: nil,
                              etat: .enFile, fin: nil, livraison: nil, texte: [], soumiseA: maintenant,
                              envoyeeA: nil, debutA: nil, termineeA: nil)
        suivis.append(s)
        file.append(s.id)
        elaguer()
        return s.id
    }

    /// Prochaine ligne a envoyer si rien n'est en vol. Une ligne invalide
    /// (trop longue...) est retiree et marquee terminee.
    public mutating func prochainEnvoi(maintenant: TimeInterval) -> (id: UUID, numero: Int, octets: Data)? {
        while enVol == nil, !file.isEmpty {
            let id = file.removeFirst()
            guard let i = index(id) else { continue }
            let numero = prochainNumero
            switch LigneCommande.octets(suivis[i].commande, id: numero) {
            case .failure:
                suivis[i].etat = .terminee
                suivis[i].termineeA = maintenant
                continue
            case .success(let octets):
                prochainNumero = LigneCommande.suivant(numero)
                suivis[i].numero = numero
                suivis[i].etat = .envoyee
                suivis[i].envoyeeA = maintenant
                enVol = id
                dernierEnvoiA = maintenant
                return (id, numero, octets)
            }
        }
        return nil
    }

    /// Note un envoi fait hors file (json 1) pour le calcul du ping.
    public mutating func noterEnvoiHorsFile(maintenant: TimeInterval) {
        dernierEnvoiA = maintenant
    }

    public enum Correlation: Sendable, Equatable {
        /// `id` inconnu (autre connexion, humain au banc) : ignore.
        case inattendue
        case debut(UUID)
        /// `fin` : `livraisonAttendue` si `suite` vaut `livraison`.
        case fin(UUID, livraisonAttendue: Bool)
    }

    public mutating func recevoir(_ r: Reponse, maintenant: TimeInterval) -> Correlation {
        // Une reponse tardive (apres "sans reponse") met encore le suivi a jour.
        guard let i = suivis.lastIndex(where: {
            $0.numero == r.id && (!$0.etat.estFinal || $0.etat == .sansReponse)
        }) else {
            return .inattendue
        }
        let id = suivis[i].id
        switch r.etape {
        case .debut:
            suivis[i].etat = .enCours
            suivis[i].debutA = maintenant
            return .debut(id)
        case .fin, .inconnu:
            suivis[i].fin = r
            suivis[i].termineeA = maintenant
            let attend = r.ok && r.code == .accepte && r.suite == .livraison
            suivis[i].etat = attend ? .attenteLivraison : .terminee
            if enVol == id { enVol = nil }
            return .fin(id, livraisonAttendue: attend)
        }
    }

    /// Une `livraison` couvre toutes les commandes acceptees depuis la
    /// precedente (6.2) : chaque commande en attente dont le numero ne depasse
    /// pas le plus grand des `ids` prend son issue (`ids_perdus` compris).
    @discardableResult
    public mutating func recevoir(_ l: Livraison, maintenant: TimeInterval) -> [UUID] {
        guard let ids = l.ids, let plusGrand = ids.max() else { return [] }
        let etat: EtatCommande = switch l.issue {
        case .livree: .livree
        case .abandon: .abandonnee
        case .annulee, .inconnu: .annulee
        }
        var touches: [UUID] = []
        for i in suivis.indices where suivis[i].etat == .attenteLivraison {
            guard let n = suivis[i].numero, n <= plusGrand else { continue }
            suivis[i].etat = etat
            suivis[i].livraison = l
            suivis[i].termineeA = maintenant
            touches.append(suivis[i].id)
        }
        return touches
    }

    /// Texte recu : rattache a la commande en vol si elle a commence.
    @discardableResult
    public mutating func texte(_ ligne: String) -> UUID? {
        guard let id = enVol, let i = index(id), suivis[i].etat == .enCours || suivis[i].etat == .envoyee
        else { return nil }
        suivis[i].texte.append(ligne)
        if suivis[i].texte.count > 400 { suivis[i].texte.removeFirst() }
        return id
    }

    /// Commandes sans `reponse` sous 3 s (et sans `debut`) : marquees, pas reemises.
    public mutating func verifierDelais(maintenant: TimeInterval) -> [SuiviCommande] {
        guard let id = enVol, let i = index(id), suivis[i].etat == .envoyee,
              let t = suivis[i].envoyeeA, maintenant - t >= Self.delaiReponse else { return [] }
        suivis[i].etat = .sansReponse
        suivis[i].termineeA = maintenant
        enVol = nil
        return [suivis[i]]
    }

    private func index(_ id: UUID) -> Int? {
        suivis.lastIndex { $0.id == id }
    }

    private mutating func elaguer() {
        guard suivis.count > Self.historiqueMax else { return }
        var aRetirer = suivis.count - Self.historiqueMax
        suivis.removeAll { s in
            guard aRetirer > 0, s.etat.estFinal else { return false }
            aRetirer -= 1
            return true
        }
    }
}
