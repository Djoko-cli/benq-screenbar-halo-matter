import Foundation
import ServiceManagement
import Testing
@testable import HaloCompagnon

/// L'etat de Thread Route, lu de ce que le systeme dit de ses deux plists : le sien et celui de
/// halo-routes, son ancien nom. Jamais l'etat reel du Mac, qui depend de la machine : la decision se teste
/// sur des etats donnes, et la lecture sur une source simulee des deux plists.
@Suite("Thread Route : son etat", .langue(.francais))
struct ThreadRouteTests {
    /// Ce que dirait le systeme des deux plists, et les plists qu'on lui a demandes. Les chemins sont ecrits
    /// en dur : une constante de l'app qui changerait ne ferait pas suivre le test.
    final class SourceSimulee {
        let nouveau: SMAppService.Status
        let ancien: SMAppService.Status
        private(set) var interroges: [String] = []

        init(nouveau: SMAppService.Status, ancien: SMAppService.Status) {
            self.nouveau = nouveau
            self.ancien = ancien
        }

        func statut(_ plist: URL) -> SMAppService.Status {
            interroges.append(plist.path)
            switch plist.path {
            case "/Library/LaunchDaemons/fr.djoko.thread.route.plist": return nouveau
            case "/Library/LaunchDaemons/fr.djoko.halo.routes.plist": return ancien
            default:
                Issue.record("plist inattendu : \(plist.path)")
                return .notFound
            }
        }
    }

    /// L'etat que lit `lire` quand le systeme repond `nouveau` pour Thread Route et `ancien` pour halo-routes.
    static func lu(nouveau: SMAppService.Status, ancien: SMAppService.Status) -> EtatThreadRoute {
        EtatThreadRoute.lire(statut: SourceSimulee(nouveau: nouveau, ancien: ancien).statut)
    }

    /// Les quatre cas : absent, a approuver, actif, ancien.
    @Test func quatreCas() {
        #expect(EtatThreadRoute.depuis(nouveau: .notRegistered, ancien: .notRegistered) == .absent)
        #expect(EtatThreadRoute.depuis(nouveau: .notFound, ancien: .notFound) == .absent)
        #expect(EtatThreadRoute.depuis(nouveau: .requiresApproval, ancien: .notRegistered) == .aApprouver,
                "desactive dans Reglages Systeme")
        #expect(EtatThreadRoute.depuis(nouveau: .enabled, ancien: .notRegistered) == .actif)
        #expect(EtatThreadRoute.depuis(nouveau: .notRegistered, ancien: .enabled) == .ancien, "halo-routes encore la")
        #expect(EtatThreadRoute.depuis(nouveau: .notRegistered, ancien: .requiresApproval) == .ancien)
    }

    /// Thread Route compte d'abord : un halo-routes reste n'y change rien.
    @Test func leNouveauDAbord() {
        #expect(EtatThreadRoute.depuis(nouveau: .enabled, ancien: .enabled) == .actif)
        #expect(EtatThreadRoute.depuis(nouveau: .requiresApproval, ancien: .enabled) == .aApprouver)
    }

    /// Les cas limites : `.notFound` (plist absent) se traite comme `.notRegistered`, des deux cotes.
    @Test func notFoundSeTraiteCommeNotRegistered() {
        #expect(EtatThreadRoute.depuis(nouveau: .notFound, ancien: .enabled) == .ancien,
                "halo-routes encore la, Thread Route introuvable")
        #expect(EtatThreadRoute.depuis(nouveau: .notFound, ancien: .requiresApproval) == .ancien)
        #expect(EtatThreadRoute.depuis(nouveau: .notRegistered, ancien: .notFound) == .absent)
        #expect(EtatThreadRoute.depuis(nouveau: .notFound, ancien: .notRegistered) == .absent)
    }

    /// `lire` interroge le systeme sur les deux plists, chacun a sa place : Thread Route pour `nouveau`,
    /// halo-routes pour `ancien`. Un plist permute, ou un etat constant, change au moins une ligne.
    @Test func lireLesDeuxPlists() {
        #expect(Self.lu(nouveau: .enabled, ancien: .notFound) == .actif)
        #expect(Self.lu(nouveau: .notFound, ancien: .enabled) == .ancien)
        #expect(Self.lu(nouveau: .requiresApproval, ancien: .notFound) == .aApprouver)
        #expect(Self.lu(nouveau: .notFound, ancien: .requiresApproval) == .ancien)
        #expect(Self.lu(nouveau: .notFound, ancien: .notFound) == .absent)
        #expect(Self.lu(nouveau: .enabled, ancien: .enabled) == .actif, "le nouveau d'abord")
        #expect(Self.lu(nouveau: .requiresApproval, ancien: .enabled) == .aApprouver)
        // Les deux plists sont demandes, une fois chacun.
        let source = SourceSimulee(nouveau: .notFound, ancien: .notFound)
        _ = EtatThreadRoute.lire(statut: source.statut)
        #expect(source.interroges.sorted() == [
            "/Library/LaunchDaemons/fr.djoko.halo.routes.plist",
            "/Library/LaunchDaemons/fr.djoko.thread.route.plist",
        ])
    }

    /// Les plists que lit l'app : ceux que posent l'installateur et l'ancien.
    @Test func plists() {
        #expect(EtatThreadRoute.plist.path == "/Library/LaunchDaemons/fr.djoko.thread.route.plist")
        #expect(EtatThreadRoute.plistAncien.path == "/Library/LaunchDaemons/fr.djoko.halo.routes.plist")
    }

    /// Ce que montrent les Reglages, en francais : le libelle de chaque etat, et ce qu'il reste a faire.
    @Test func libellesEtConsignes() {
        #expect(EtatThreadRoute.absent.libelle == "Absent")
        #expect(EtatThreadRoute.aApprouver.libelle == "Désactivé dans Réglages Système")
        #expect(EtatThreadRoute.actif.libelle == "Actif")
        #expect(EtatThreadRoute.ancien.libelle == "halo-routes, son ancien nom, est encore installé")
        #expect(EtatThreadRoute.absent.consigne
                == "Pour l'installer : sh tools/macos/thread-route/installer.sh (mot de passe administrateur).")
        #expect(EtatThreadRoute.aApprouver.consigne
                == "L'autoriser dans Réglages Système, Général, Ouverture et extensions.")
        #expect(EtatThreadRoute.actif.consigne == nil, "rien a faire quand il est actif")
        #expect(EtatThreadRoute.ancien.consigne
                == "Pour le remplacer : sh tools/macos/thread-route/installer.sh (mot de passe administrateur).")
    }

    /// Les memes textes en anglais : la traduction du catalogue, valeur par valeur.
    @Test(.langue(.anglais)) func libellesEtConsignesEnAnglais() {
        #expect(EtatThreadRoute.absent.libelle == "Absent")
        #expect(EtatThreadRoute.aApprouver.libelle == "Turned off in System Settings")
        #expect(EtatThreadRoute.actif.libelle == "Active")
        #expect(EtatThreadRoute.ancien.libelle == "halo-routes, its former name, is still installed")
        #expect(EtatThreadRoute.absent.consigne
                == "To install it: sh tools/macos/thread-route/installer.sh (administrator password).")
        #expect(EtatThreadRoute.aApprouver.consigne
                == "Allow it in System Settings, General, Login Items & Extensions.")
        #expect(EtatThreadRoute.actif.consigne == nil)
        #expect(EtatThreadRoute.ancien.consigne
                == "To replace it: sh tools/macos/thread-route/installer.sh (administrator password).")
    }
}
