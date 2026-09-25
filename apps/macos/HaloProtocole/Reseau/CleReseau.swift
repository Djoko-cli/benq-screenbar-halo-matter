import Foundation

/// Cle partagee du transport reseau, creee par l'USB (10.4) : l'app fournit
/// un alea, la carte calcule `cle = HMAC-SHA256(alea_app, alea_carte)` et la
/// rend une seule fois dans la reponse.
public enum CleReseau {
    /// Alea de l'app : 32 octets d'un generateur cryptographique.
    public static func alea() -> Data { H1.aleatoire(32) }

    /// `json cle nouvelle <64 HEXA>` (USB seulement ; la carte la refuse a distance).
    public static func commande(alea: Data) -> String {
        "json cle nouvelle \(H1.hexa(alea))"
    }

    public struct Creee: Sendable, Equatable {
        public var cle: Data
        public var empreinte: String
    }

    public enum Erreur: Error, Sendable, Equatable, CustomStringConvertible {
        /// `ok` faux (tampon USB occupe...) : rien n'a change sur la carte.
        case refusee(String)
        case cleIllisible
        case empreinteIncoherente

        public var description: String {
            switch self {
            case .refusee(let msg): tr("La carte refuse la nouvelle clé : \(msg)")
            case .cleIllisible: tr("Réponse sans clé lisible (64 hexa majuscules attendus) : clé non rangée.")
            case .empreinteIncoherente: tr("Empreinte incohérente avec la clé reçue : clé non rangée.")
            }
        }
    }

    /// Reponse `fin` a `json cle nouvelle` : la cle et son empreinte, verifiees.
    public static func verifier(_ r: Reponse) -> Result<Creee, Erreur> {
        guard r.ok else { return .failure(.refusee(r.msg ?? r.code.rawValue)) }
        guard let texte = r.cle, let cle = H1.octets(hexa: texte), cle.count == 32 else { return .failure(.cleIllisible) }
        guard let e = r.empreinte, e == H1.kid(cle: cle) else { return .failure(.empreinteIncoherente) }
        return .success(Creee(cle: cle, empreinte: e))
    }
}

/// Acces reseau vu par l'USB : bloc `ip` de la carte, et cles de ce Mac.
public enum EtatAccesReseau: Sendable, Equatable {
    /// Pas de bloc `ip` (build sans Thread, pas encore recu), nom SRP inconnu,
    /// ou firmware sans transport reseau (pas de `udp`).
    case inconnu
    /// Le pont n'a pas de cle : transport reseau coupe.
    case sansCle(nom: String)
    /// La cle du pont est celle de ce Mac.
    case cleConnue(nom: String, empreinte: String)
    /// Le pont a une cle que ce Mac n'a pas (autre Mac, `halo_udp.py`, cle recreee).
    case cleInconnue(nom: String, empreinte: String)

    public static func depuis(ip: ReseauIp?, empreinteDuMac: (String) -> String?) -> EtatAccesReseau {
        guard let nom = ip?.srp?.nom, !nom.isEmpty, let udp = ip?.udp else { return .inconnu }
        guard udp.ouvert == true, let e = udp.empreinte else { return .sansCle(nom: nom) }
        return empreinteDuMac(nom) == e ? .cleConnue(nom: nom, empreinte: e) : .cleInconnue(nom: nom, empreinte: e)
    }
}
