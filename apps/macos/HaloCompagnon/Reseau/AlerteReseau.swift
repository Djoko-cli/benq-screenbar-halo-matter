import Foundation
import HaloProtocole

/// Alerte de la source reseau, montree en bandeau ; son texte suit la langue en vigueur.
enum AlerteReseau: Equatable, Sendable {
    case transport(ErreurReseau)
    case trousseau(ErreurTrousseau)
    /// DEFI recu, puis aucun `hello` (places de session prises, 10.4).
    case sansHello

    var texte: String {
        switch self {
        case .transport(.pasDeRoute): Self.textePasDeRoute(assistant: Self.assistantInstalle)
        case .transport(let e): e.description
        case .trousseau(let e): e.description
        case .sansHello:
            tr("Aucune réponse au json 1 par le réseau : deux autres sessions déjà actives (autre Mac, iPhone, halo_udp.py) ? Nouvel essai toutes les 30 s.")
        }
    }

    static func textePasDeRoute(assistant: Bool) -> String {
        ErreurReseau.pasDeRoute.description + " " + (assistant
            ? tr("L'assistant système halo-routes est installé : la route revient d'elle-même.")
            : tr("Installer l'assistant système : sh tools/macos/halo-routes/installer.sh"))
    }

    /// Le plist de l'assistant (10.1), si la sandbox laisse le voir.
    static var assistantInstalle: Bool {
        FileManager.default.fileExists(atPath: "/Library/LaunchDaemons/fr.djoko.halo.routes.plist")
    }
}
