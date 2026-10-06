import Foundation
import HaloProtocole

/// Alerte de la source reseau, montree en bandeau ; son texte suit la langue en vigueur.
enum AlerteReseau: Equatable, Sendable {
    case transport(ErreurReseau)
    case trousseau(ErreurTrousseau)
    /// DEFI recu, puis aucun `hello` (places de session prises, 10.4).
    case sansHello

    /// Le texte du bandeau, avec l'etat reel de Thread Route (lu seulement pour "pas de route").
    var texte: String { texte(etatThreadRoute: { EtatThreadRoute.lire() }) }

    /// Le meme texte, l'etat de Thread Route venant de `etatThreadRoute` : les tests le fixent.
    func texte(etatThreadRoute: () -> EtatThreadRoute) -> String {
        switch self {
        case .transport(.pasDeRoute): Self.textePasDeRoute(etatThreadRoute())
        case .transport(let e): e.description
        case .trousseau(let e): e.description
        case .sansHello:
            tr("Aucune réponse au json 1 par le réseau : deux autres sessions déjà actives (autre Mac, iPhone, halo_udp.py) ? Nouvel essai toutes les 30 s.")
        }
    }

    /// Sans route : la route revient d'elle-meme si Thread Route est actif (10.1) ; sinon, ce qu'il reste a faire.
    static func textePasDeRoute(_ etat: EtatThreadRoute) -> String {
        ErreurReseau.pasDeRoute.description + " "
            + (etat.consigne ?? tr("Thread Route est actif : la route revient d'elle-même."))
    }
}
