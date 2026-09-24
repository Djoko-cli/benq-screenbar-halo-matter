import Foundation
import HaloProtocole

/// Texte du catalogue de l'app (`Ressources/Localizable.xcstrings`), dans la
/// langue en vigueur. Pour les textes qui ne passent pas par `Text("...")`
/// (libelles calcules, notes de la console, menus) ; lu dans le corps d'une
/// vue, il la fait dependre de la langue.
func tr(_ valeur: String.LocalizationValue) -> String {
    Localisation.partagee.texte(valeur, paquet: .main)
}

/// Reglage "Langue" (Reglages, ⌘,) : sa persistance et son application.
///
/// A chaud : les vues (`Text` et la locale de l'environnement) et tous les
/// textes calcules (`Localisation`). Au prochain lancement seulement : ce que
/// macOS dessine lui-meme (menus de l'app, Edition, Fenetre, boites du
/// systeme), qui suit `AppleLanguages` de l'app, ecrit ici.
enum ReglageLangue {
    /// Cle du choix dans les preferences (`@AppStorage`).
    static let cle = "langue"
    /// Langues de l'app pour AppKit, lues au lancement seulement.
    static let cleAppleLanguages = "AppleLanguages"

    /// Langue de l'interface au lancement : celle que montrent les menus de
    /// macOS jusqu'au prochain lancement.
    @MainActor static private(set) var langueAuLancement: Langue = .francais

    static func choix(_ defauts: UserDefaults = .standard) -> ChoixLangue {
        defauts.string(forKey: cle).flatMap(ChoixLangue.init(rawValue:)) ?? .systeme
    }

    /// Langues preferees de l'utilisateur pour cette app (Reglages Systeme, y
    /// compris la langue choisie app par app ; celles de la ligne de commande,
    /// `-AppleLanguages`, d'abord).
    static func languesSysteme(_ defauts: UserDefaults = .standard) -> [String] {
        defauts.stringArray(forKey: cleAppleLanguages) ?? Locale.preferredLanguages
    }

    /// Au lancement, avant la premiere vue.
    @MainActor
    static func appliquerAuLancement(_ defauts: UserDefaults = .standard, paquet: Bundle = .main) {
        // Langue que macOS a retenue pour ce processus (menus, boites du systeme).
        langueAuLancement = paquet.preferredLocalizations.first.flatMap(Langue.init(rawValue:)) ?? .francais
        appliquer(choix(defauts), defauts: defauts, lancement: true)
    }

    /// Nouveau choix : textes de l'app a chaud, menus de macOS au prochain lancement.
    @MainActor
    static func appliquer(_ c: ChoixLangue, defauts: UserDefaults = .standard, lancement: Bool = false,
                          localisation: Localisation = .partagee) {
        defauts.set(c.rawValue, forKey: cle)
        switch c {
        case .anglais, .francais:
            defauts.set([c.langue(preferences: []).code], forKey: cleAppleLanguages)
        case .systeme where !lancement:
            // Retirer la valeur de l'app rend celle du systeme (NSGlobalDomain).
            defauts.removeObject(forKey: cleAppleLanguages)
        case .systeme:
            // Au lancement, rien a retirer : une langue choisie pour l'app dans
            // Reglages Systeme reste la "langue du systeme" de l'app.
            break
        }
        let preferences = languesSysteme(defauts)
        let langue = c.langue(preferences: preferences)
        localisation.appliquer(langue, locale: ChoixLangue.locale(langue, courante: .current))
    }

    /// Les menus de macOS montreront une autre langue au prochain lancement.
    @MainActor static var relancePourLesMenus: Bool {
        Localisation.partagee.langue != langueAuLancement
    }
}
