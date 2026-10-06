import Foundation
import Security
import Testing
@testable import HaloCompagnon

/// Les mises a jour (Sparkle 2) : ce que porte l'app, et le moteur jamais demarre sous les tests.
@MainActor
@Suite("Mises a jour : Sparkle")
struct MisesAJourTests {
    /// L'Info.plist : la version, le flux des versions publiees, la cle publique et les reglages de Sparkle.
    @Test func infoPlist() throws {
        let info = try #require(Bundle.main.infoDictionary)
        #expect(info["CFBundleShortVersionString"] as? String == "1.0.0")
        #expect(Int(info["CFBundleVersion"] as? String ?? "") != nil, "un nombre, que compare Sparkle")
        #expect(info["SUFeedURL"] as? String
                == "https://raw.githubusercontent.com/Djoko-cli/benq-screenbar-halo-matter/main/apps/macos/appcast.xml")
        let cle = try #require(info["SUPublicEDKey"] as? String)
        #expect(Data(base64Encoded: cle)?.count == 32, "une cle publique Ed25519 : \(cle)")
        #expect(info["SUEnableAutomaticChecks"] as? Bool == true)
        #expect(info["SUScheduledCheckInterval"] as? Int == 86_400)
        #expect(info["SUAutomaticallyUpdate"] as? Bool == true)
        #expect(info["SUEnableInstallerLauncherService"] as? Bool == true)
        // Ceux de la generation de l'Info.plist par Xcode restent.
        #expect(info["CFBundleDisplayName"] as? String == "Halo Compagnon")
        #expect(info["NSLocalNetworkUsageDescription"] as? String != nil)
    }

    /// Les droits du bac a sable : les deux services de Sparkle, rien de plus (le telechargement passe par
    /// `network.client`).
    @Test func droitsMachLookup() throws {
        let tache = try #require(SecTaskCreateFromSelf(nil))
        let cle = "com.apple.security.temporary-exception.mach-lookup.global-name" as CFString
        let valeur = SecTaskCopyValueForEntitlement(tache, cle, nil) as? [String] ?? []
        // Sous les tests, Xcode ajoute ceux de ses outils (com.apple…) a la compilation Debug.
        #expect(valeur.filter { !$0.hasPrefix("com.apple.") }
                == ["fr.djoko.halo.compagnon-spks", "fr.djoko.halo.compagnon-spki"])
        let bac = SecTaskCopyValueForEntitlement(tache, "com.apple.security.app-sandbox" as CFString, nil) as? Bool
        #expect(bac == true)
    }

    /// Celle de l'app, creee a son lancement, n'est pas demarree sous les tests : aucune recherche, aucun reseau.
    @Test func moteurArreteSousLesTests() throws {
        #expect(!MisesAJour.demarrerAuLancement)
        let m = try #require(MisesAJour.deLApp, "creee au lancement de l'app")
        #expect(!m.demarre)
        #expect(!m.peutRechercher, "un moteur arrete ne recherche pas")
    }
}
