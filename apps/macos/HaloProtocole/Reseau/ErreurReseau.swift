import Foundation
import Network
import dnssd

/// Echec du transport reseau (10.1 a 10.4), avec son texte et sa reprise.
public enum ErreurReseau: Error, Sendable, Equatable, CustomStringConvertible {
    /// Autorisation "reseau local" refusee (Reglages Systeme).
    case reseauLocalRefuse
    /// Pas de route IPv6 vers le reseau Thread (bug du noyau de macOS, 10.1).
    case pasDeRoute
    /// `<nom>.local` introuvable.
    case nomIntrouvable(String)
    /// ICMPv6 "port injoignable" : le pont n'a plus de cle (port 5480 ferme).
    case portInjoignable
    /// Aucun DEFI juste apres les essais du SALUT (autre cle, pont muet).
    case aucunDefi
    /// Connexion perdue apres son ouverture.
    case cheminPerdu(String)
    case autre(String)

    /// Vrai : la reconnexion reessaie seule ; faux : il faut l'utilisateur
    /// (ou un changement du reseau).
    public var repriseAutomatique: Bool {
        switch self {
        case .reseauLocalRefuse, .portInjoignable: false
        default: true
        }
    }

    public var description: String {
        switch self {
        case .reseauLocalRefuse:
            tr("Accès au réseau local refusé : Réglages Système › Confidentialité et sécurité › Réseau local › Halo Compagnon.")
        case .pasDeRoute:
            tr("Pas de route IPv6 vers le réseau Thread (bug du noyau de macOS, section 10.1).")
        case .nomIntrouvable(let hote):
            tr("Pont introuvable (\(hote)) : éteint, hors du réseau Thread, ou routeurs de bordure injoignables.")
        case .portInjoignable:
            tr("Le pont n'a plus de clé : le brancher en USB, puis « Activer l'accès réseau ».")
        case .aucunDefi:
            tr("Aucune réponse du pont : clé différente de la sienne, ou pont sans clé ? (vérifier par l'USB, carte Thread et Matter)")
        case .cheminPerdu(let raison):
            tr("Connexion réseau perdue : \(raison)")
        case .autre(let raison):
            tr("Erreur réseau : \(raison)")
        }
    }

    /// Erreur de Network.framework ; `chemin` : dernier chemin connu de la connexion.
    public static func depuis(_ e: NWError, chemin: NWPath?, hote: String) -> ErreurReseau {
        if chemin?.unsatisfiedReason == .localNetworkDenied { return .reseauLocalRefuse }
        switch e {
        case .posix(let code):
            switch code {
            case .EHOSTUNREACH, .ENETUNREACH, .ENETDOWN, .EHOSTDOWN: return .pasDeRoute
            case .ECONNREFUSED: return .portInjoignable
            default: return .autre(String(describing: code))
            }
        case .dns(let code):
            return Int(code) == kDNSServiceErr_PolicyDenied ? .reseauLocalRefuse : .nomIntrouvable(hote)
        default:
            return .autre(String(describing: e))
        }
    }
}
