import Foundation
import Network
import dnssd

/// Echec du transport reseau (10.1 a 10.4), avec son texte et sa reprise.
public enum ErreurReseau: Error, Sendable, Equatable, CustomStringConvertible {
    /// Autorisation "reseau local" refusee (Reglages Systeme) : macOS refuse
    /// la resolution du nom `.local` du pont.
    case reseauLocalRefuse
    /// Pas de route IPv6 vers le reseau Thread (bug du noyau de macOS, 10.1).
    case pasDeRoute
    /// `<nom>.local` introuvable, ou le noeud ne repond pas (`EHOSTDOWN`).
    case nomIntrouvable(String)
    /// ICMPv6 "port injoignable" : le pont n'a plus de cle (port 5480 ferme).
    case portInjoignable
    /// Aucun DEFI juste apres les essais du SALUT (autre cle, pont muet).
    case aucunDefi
    /// Connexion perdue apres son ouverture (`.waiting`, `.failed`, erreur de
    /// reception) : son texte est la raison de fermeture du flux, la cause
    /// ensuite (`TransportUDP.raisonPerte`).
    case cheminPerdu(String)
    case autre(String)

    /// Vrai : la reconnexion reessaie seule ; faux : il faut l'utilisateur
    /// (ou un changement du reseau). "Reseau local refuse" reessaie : le
    /// refus se leve dans Reglages Systeme sans rien signaler a l'app, et un
    /// essai refuse ne sort pas du Mac (banc R7).
    public var repriseAutomatique: Bool {
        if case .portInjoignable = self { return false }
        return true
    }

    /// Vrai : l'utilisateur doit agir (Reglages Systeme, USB) ; montre en
    /// bandeau, essais en cours ou non.
    public var bandeau: Bool {
        switch self {
        case .reseauLocalRefuse, .portInjoignable: true
        default: false
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
            case .EHOSTUNREACH, .ENETUNREACH, .ENETDOWN: return .pasDeRoute
            // ICMPv6 "adresse injoignable" : la route existe, c'est le noeud
            // qui ne repond pas (eteint, hors du reseau Thread).
            case .EHOSTDOWN: return .nomIntrouvable(hote)
            case .ECONNREFUSED: return .portInjoignable
            default: return .autre(String(describing: code))
            }
        case .dns(let code):
            // Reseau local refuse : `PolicyDenied` selon la documentation ;
            // macOS repond en fait `NoSuchRecord`, aussitot, a la resolution
            // d'un nom `.local` (banc R7). Un nom `.local` absent, lui, reste
            // sans reponse (aucune erreur) : l'attente de `attentePret` le
            // classe introuvable.
            switch Int(code) {
            case kDNSServiceErr_PolicyDenied: return .reseauLocalRefuse
            case kDNSServiceErr_NoSuchRecord where estLocal(hote): return .reseauLocalRefuse
            default: return .nomIntrouvable(hote)
            }
        default:
            return .autre(String(describing: e))
        }
    }

    /// Nom mDNS (`.local`, avec ou sans point final).
    static func estLocal(_ hote: String) -> Bool {
        let h = hote.lowercased()
        return h.hasSuffix(".local") || h.hasSuffix(".local.")
    }
}
