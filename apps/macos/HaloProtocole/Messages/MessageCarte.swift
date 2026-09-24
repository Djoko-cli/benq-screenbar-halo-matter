import Foundation

/// Champs communs a toutes les lignes machine (section 4).
public struct Enveloppe: Codable, Sendable, Equatable {
    /// Version majeure du protocole.
    public var v: Int
    /// Type de message.
    public var t: String
    /// Numero de ligne produite sur ce transport depuis le demarrage.
    public var n: UInt32
    /// `millis()` a la production de la ligne.
    public var ms: UInt32?
    /// Partie du message (`hello`, `etat`, `compteurs`, `reseau`).
    public var bloc: String?
}

/// Un message de la carte, decode selon (`t`, `bloc`).
public enum MessageCarte: Sendable, Equatable {
    case helloBase(HelloBase)
    case helloIdentite(HelloIdentite)
    case config(ConfigCarte)
    case etatLampe(BlocEtatLampe)
    case etatTranches(BlocTranches)
    case etatSante(BlocSante)
    case compteursPilote(CompteursPilote)
    case compteursRadio(CompteursRadio)
    case compteursMatter(CompteursMatter)
    case reseauThread(ReseauThread)
    case reseauAbonnements(ReseauAbonnements)
    case reseauIp(ReseauIp)
    case battement(Battement)
    case fin(FinSession)
    case reponse(Reponse)
    case rx(TrameRx)
    case tx(PaquetTx)
    case livraison(Livraison)
    case relance(Relance)
    case module(EvenementModule)
    case intent(IntentMatter)
    case abonnement(EvenementAbonnement)
    case thread(ChangementRole)
    case led(ChangementLed)
    case log(MessageLog)
    /// Type ou bloc inconnu : ignore (section 9.1).
    case inconnu

    /// Vrai pour les messages periodiques (instantanes), faux pour les evenements.
    public var estPeriodique: Bool {
        switch self {
        case .helloBase, .helloIdentite, .config, .etatLampe, .etatTranches, .etatSante,
             .compteursPilote, .compteursRadio, .compteursMatter, .reseauThread,
             .reseauAbonnements, .reseauIp, .battement:
            true
        default:
            false
        }
    }
}

/// Ligne machine valide et decodee.
public struct LigneMachine: Sendable, Equatable {
    public var enveloppe: Enveloppe
    public var message: MessageCarte
    /// Le JSON tel que recu (ASCII), pour le journal et l'inspection.
    public var json: String

    public init(enveloppe: Enveloppe, message: MessageCarte, json: String) {
        self.enveloppe = enveloppe
        self.message = message
        self.json = json
    }

    /// Cle de remplacement des instantanes (`t`, `bloc`).
    public var cle: String {
        if let bloc = enveloppe.bloc { return "\(enveloppe.t).\(bloc)" }
        return enveloppe.t
    }
}

/// Decodage d'un objet JSON de la carte : enveloppe d'abord, puis un
/// `Codable` par (`t`, `bloc`) (section 9.1).
public enum DecodeurMessages {
    /// Versions majeures gerees par l'app.
    public static let versionsGerees: Set<Int> = [1]

    public enum Resultat: Sendable, Equatable {
        case valide(LigneMachine)
        /// Enveloppe absente ou mal typee : ligne abimee.
        case abimee(String)
        case versionInconnue(v: Int, t: String)
        /// Champ obligatoire absent ou mal type dans le corps.
        case invalide(t: String, raison: String)
    }

    private static func decodeur() -> JSONDecoder {
        let d = JSONDecoder()
        d.keyDecodingStrategy = .convertFromSnakeCase
        return d
    }

    public static func decoder(json: Data) -> Resultat {
        let d = decodeur()
        let enveloppe: Enveloppe
        do {
            enveloppe = try d.decode(Enveloppe.self, from: json)
        } catch {
            return .abimee(tr("enveloppe : \(Self.raison(error))"))
        }
        guard versionsGerees.contains(enveloppe.v) else {
            return .versionInconnue(v: enveloppe.v, t: enveloppe.t)
        }
        let texte = String(decoding: json, as: UTF8.self)
        do {
            let message = try corps(enveloppe: enveloppe, json: json, decodeur: d)
            return .valide(LigneMachine(enveloppe: enveloppe, message: message, json: texte))
        } catch {
            return .invalide(t: enveloppe.t, raison: Self.raison(error))
        }
    }

    private static func corps(enveloppe e: Enveloppe, json: Data, decodeur d: JSONDecoder) throws -> MessageCarte {
        switch (e.t, e.bloc) {
        case ("hello", "base"): return .helloBase(try d.decode(HelloBase.self, from: json))
        case ("hello", "identite"): return .helloIdentite(try d.decode(HelloIdentite.self, from: json))
        case ("config", _): return .config(try d.decode(ConfigCarte.self, from: json))
        case ("etat", "lampe"): return .etatLampe(try d.decode(BlocEtatLampe.self, from: json))
        case ("etat", "tranches"): return .etatTranches(try d.decode(BlocTranches.self, from: json))
        case ("etat", "sante"): return .etatSante(try d.decode(BlocSante.self, from: json))
        case ("compteurs", "pilote"): return .compteursPilote(try d.decode(CompteursPilote.self, from: json))
        case ("compteurs", "radio"): return .compteursRadio(try d.decode(CompteursRadio.self, from: json))
        case ("compteurs", "matter"): return .compteursMatter(try d.decode(CompteursMatter.self, from: json))
        case ("reseau", "thread"): return .reseauThread(try d.decode(ReseauThread.self, from: json))
        case ("reseau", "abonnements"): return .reseauAbonnements(try d.decode(ReseauAbonnements.self, from: json))
        case ("reseau", "ip"): return .reseauIp(try d.decode(ReseauIp.self, from: json))
        case ("hb", _): return .battement(try d.decode(Battement.self, from: json))
        case ("fin", _): return .fin(try d.decode(FinSession.self, from: json))
        case ("reponse", _): return .reponse(try d.decode(Reponse.self, from: json))
        case ("rx", _): return .rx(try d.decode(TrameRx.self, from: json))
        case ("tx", _): return .tx(try d.decode(PaquetTx.self, from: json))
        case ("livraison", _): return .livraison(try d.decode(Livraison.self, from: json))
        case ("relance", _): return .relance(try d.decode(Relance.self, from: json))
        case ("module", _): return .module(try d.decode(EvenementModule.self, from: json))
        case ("intent", _): return .intent(try d.decode(IntentMatter.self, from: json))
        case ("abonnement", _): return .abonnement(try d.decode(EvenementAbonnement.self, from: json))
        case ("thread", _): return .thread(try d.decode(ChangementRole.self, from: json))
        case ("led", _): return .led(try d.decode(ChangementLed.self, from: json))
        case ("log", _): return .log(try d.decode(MessageLog.self, from: json))
        default: return .inconnu
        }
    }

    static func raison(_ erreur: any Error) -> String {
        guard let e = erreur as? DecodingError else { return String(describing: erreur) }
        func chemin(_ c: [any CodingKey]) -> String {
            c.map { $0.intValue.map(String.init) ?? $0.stringValue }.joined(separator: ".")
        }
        switch e {
        case .keyNotFound(let cle, let ctx):
            let base = chemin(ctx.codingPath)
            let champ = (base.isEmpty ? "" : base + ".") + cle.stringValue
            return tr("champ absent : \(champ)")
        case .typeMismatch(_, let ctx), .valueNotFound(_, let ctx):
            return tr("champ mal typé : \(chemin(ctx.codingPath))")
        case .dataCorrupted(let ctx):
            return tr("JSON invalide \(chemin(ctx.codingPath))")
        @unknown default:
            return tr("décodage impossible")
        }
    }
}
