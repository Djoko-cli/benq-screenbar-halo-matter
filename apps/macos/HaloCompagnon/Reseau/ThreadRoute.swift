import Foundation
import ServiceManagement

/// Etat de Thread Route, le demon systeme qui garde la route du Mac vers le reseau Thread
/// (`tools/macos/thread-route`, 10.1). Il s'installe par son installateur, avec le mot de passe
/// administrateur : une app dans le bac a sable ne peut pas l'inscrire elle-meme (essai du 06/10 :
/// SMAppService y refuse un demon qui n'est pas dans le bac a sable). L'app lit son etat aupres du
/// systeme, ce que permet le bac a sable (`SMAppService.statusForLegacyPlist`).
enum EtatThreadRoute: Equatable, Sendable {
    /// Ni Thread Route, ni halo-routes.
    case absent
    /// Installe, mais desactive dans Reglages Systeme (Ouverture et extensions).
    case aApprouver
    /// Installe et autorise : launchd le garde en marche.
    case actif
    /// halo-routes, son ancien nom, est encore installe : l'installateur le remplace.
    case ancien

    static let plist = URL(fileURLWithPath: "/Library/LaunchDaemons/fr.djoko.thread.route.plist")
    static let plistAncien = URL(fileURLWithPath: "/Library/LaunchDaemons/fr.djoko.halo.routes.plist")

    /// L'etat, a partir de ce que le systeme dit du plist de Thread Route et de celui de halo-routes.
    static func depuis(nouveau: SMAppService.Status, ancien: SMAppService.Status) -> EtatThreadRoute {
        switch nouveau {
        case .enabled: .actif
        case .requiresApproval: .aApprouver
        default: ancien == .enabled || ancien == .requiresApproval ? .ancien : .absent
        }
    }

    /// L'etat du moment, lu aupres du systeme.
    static func lire() -> EtatThreadRoute {
        depuis(nouveau: SMAppService.statusForLegacyPlist(at: plist),
               ancien: SMAppService.statusForLegacyPlist(at: plistAncien))
    }

    /// Libelle de l'etat, dans les Reglages.
    var libelle: String {
        switch self {
        case .absent: tr("Absent")
        case .aApprouver: tr("Désactivé dans Réglages Système")
        case .actif: tr("Actif")
        case .ancien: tr("halo-routes, son ancien nom, est encore installé")
        }
    }

    /// Ce qu'il reste a faire ; rien quand il est actif.
    var consigne: String? {
        switch self {
        case .absent: tr("Pour l'installer : sh tools/macos/thread-route/installer.sh (mot de passe administrateur).")
        case .aApprouver: tr("L'autoriser dans Réglages Système, Général, Ouverture et extensions.")
        case .actif: nil
        case .ancien: tr("Pour le remplacer : sh tools/macos/thread-route/installer.sh (mot de passe administrateur).")
        }
    }
}
