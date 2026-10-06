import Combine
import Foundation
import Observation
import Sparkle

/// Les mises a jour de l'app, par Sparkle 2 : recherche au demarrage puis toutes les 24 h, telechargement et
/// installation automatiques, a la fermeture de l'app ou tout de suite par « Installer et relancer ». Le flux
/// (`SUFeedURL`), la cle publique (`SUPublicEDKey`) et ces choix par defaut sont dans l'Info.plist (project.yml).
/// L'app reste dans le bac a sable : Sparkle installe par son service (`SUEnableInstallerLauncherService`) ; les
/// textes de sa fenetre viennent de ses propres traductions.
///
/// Sous les tests, le moteur n'est jamais demarre : aucune recherche, aucun acces au reseau, et les reglages ne
/// sont pas ecrits (les preferences sont celles de l'app installee). Ni dans une compilation de travail (numero de
/// compilation 1, jamais publie) : elle ne se remplace jamais par la version publiee.
@MainActor
@Observable
final class MisesAJour {
    /// Celle de l'app, creee a son lancement.
    static private(set) weak var deLApp: MisesAJour?

    /// Le moteur est-il demarre ?
    let demarre: Bool
    /// « Rechercher les mises a jour… » est possible : le moteur tourne, et aucune recherche n'est en cours.
    private(set) var peutRechercher = false
    /// Reglage : rechercher automatiquement (au demarrage, puis toutes les 24 h).
    var rechercheAuto: Bool {
        didSet { if demarre { controleur.updater.automaticallyChecksForUpdates = rechercheAuto } }
    }
    /// Reglage : telecharger et installer automatiquement.
    var installationAuto: Bool {
        didSet { if demarre { controleur.updater.automaticallyDownloadsUpdates = installationAuto } }
    }

    @ObservationIgnored private let controleur: SPUStandardUpdaterController
    @ObservationIgnored private var abonnement: AnyCancellable?

    init(demarrer: Bool) {
        controleur = SPUStandardUpdaterController(startingUpdater: demarrer, updaterDelegate: nil,
                                                  userDriverDelegate: nil)
        demarre = demarrer
        rechercheAuto = controleur.updater.automaticallyChecksForUpdates
        installationAuto = controleur.updater.automaticallyDownloadsUpdates
        abonnement = controleur.updater.publisher(for: \.canCheckForUpdates).sink { [weak self] peut in
            MainActor.assumeIsolated { self?.peutRechercher = peut }
        }
        Self.deLApp = self
    }

    /// Le moteur demarre au lancement de l'app, sauf sous les tests et dans une compilation de travail.
    static var demarrerAuLancement: Bool {
        doitDemarrer(numero: Bundle.main.infoDictionary?["CFBundleVersion"] as? String,
                     environnement: ProcessInfo.processInfo.environment)
    }

    /// La decision, pure : oui, sauf si l'environnement porte celui des tests (Xcode pose
    /// `XCTestConfigurationFilePath`), et si le numero de compilation (`CFBundleVersion`) est celui d'une
    /// compilation de travail, 1 (project.yml ; publier.sh donne le nombre de commits de main) ou absent.
    nonisolated static func doitDemarrer(numero: String?, environnement: [String: String]) -> Bool {
        numero != nil && numero != "1" && environnement["XCTestConfigurationFilePath"] == nil
    }

    /// « Rechercher les mises a jour… » : la fenetre de Sparkle dit ce qu'elle trouve.
    func rechercher() {
        controleur.checkForUpdates(nil)
    }
}
