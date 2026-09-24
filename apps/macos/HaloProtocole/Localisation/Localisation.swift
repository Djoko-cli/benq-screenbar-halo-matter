import Foundation
import Observation
import Synchronization

/// Langue de l'interface. Le francais est la langue de developpement : les
/// cles des catalogues (`Localizable.xcstrings`) sont les textes francais.
public enum Langue: String, CaseIterable, Sendable, Hashable {
    case francais = "fr"
    case anglais = "en"

    /// Code du dossier `.lproj` et de `Locale.Language`.
    public var code: String { rawValue }

    /// Premiere des preferences (`["en-GB", "fr-FR"]`...) que l'app sait
    /// servir, comme le fait `Bundle` ; a defaut, le francais, langue de
    /// developpement (c'est aussi ce que montrent alors les menus d'AppKit).
    public static func preferee(parmi preferences: [String]) -> Langue {
        for p in preferences {
            let code = Locale.Language(identifier: p).languageCode?.identifier
            if let l = allCases.first(where: { $0.code == code }) { return l }
        }
        return .francais
    }
}

/// Reglage "Langue" de l'app : celle du systeme (par defaut) ou une langue imposee.
public enum ChoixLangue: String, CaseIterable, Sendable, Identifiable {
    case systeme
    case anglais
    case francais

    public var id: String { rawValue }

    /// Langue en vigueur ; `preferences` : langues du systeme, dans l'ordre.
    public func langue(preferences: [String]) -> Langue {
        switch self {
        case .systeme: Langue.preferee(parmi: preferences)
        case .anglais: .anglais
        case .francais: .francais
        }
    }

    /// Locale de l'interface (dates, nombres, recherche de textes) : la langue
    /// en vigueur avec la region de l'utilisateur, comme macOS le fait pour une
    /// langue choisie app par app (anglais en France : `en_FR`, 24 h, jj/mm).
    /// La locale courante est gardee telle quelle si elle parle deja cette
    /// langue (formats personnalises de Reglages Systeme compris).
    public func locale(courante: Locale, preferences: [String]) -> Locale {
        Self.locale(langue(preferences: preferences), courante: courante)
    }

    public static func locale(_ langue: Langue, courante: Locale) -> Locale {
        if courante.language.languageCode?.identifier == langue.code { return courante }
        return Locale(components: Locale.Components(languageCode: Locale.LanguageCode(langue.code),
                                                    languageRegion: courante.region))
    }
}

/// Langue en vigueur pour les textes produits hors des vues (sens decode,
/// libelles, erreurs, notes de session), et pour ceux des vues qui ne passent
/// pas par `Text("...")`.
///
/// Observable : une vue qui lit un texte localise depend de la langue, et se
/// redessine quand elle change (bascule a chaud, sans relancer l'app).
/// Surete : un verrou garde l'etat, lu depuis n'importe quel fil.
public final class Localisation: Sendable, Observable {
    public static let partagee = Localisation()

    /// Langue imposee a une tache et a ses sous-taches (tests en parallele).
    @TaskLocal public static var imposee: Langue?

    private struct Etat {
        var langue: Langue
        var locale: Locale
    }

    private let registre = ObservationRegistrar()
    private let etat: Mutex<Etat>

    public init(langue: Langue = Langue.preferee(parmi: Locale.preferredLanguages)) {
        etat = Mutex(Etat(langue: langue, locale: ChoixLangue.locale(langue, courante: .current)))
    }

    /// Langue en vigueur (celle de la tache si elle en impose une).
    public var langue: Langue {
        registre.access(self, keyPath: \.langue)
        return Self.imposee ?? etat.withLock { $0.langue }
    }

    /// Locale des formats (nombres, dates) qui va avec la langue en vigueur.
    public var locale: Locale {
        registre.access(self, keyPath: \.langue)
        if let l = Self.imposee { return ChoixLangue.locale(l, courante: .current) }
        return etat.withLock { $0.locale }
    }

    /// Change la langue ; les vues qui en dependent se redessinent.
    public func appliquer(_ langue: Langue, locale: Locale) {
        registre.withMutation(of: self, keyPath: \.langue) {
            etat.withLock {
                $0.langue = langue
                $0.locale = locale
            }
        }
    }

    /// Texte localise d'un catalogue, dans la langue en vigueur.
    ///
    /// Recherche dans le sous-paquet `<langue>.lproj` : elle ne depend que de
    /// la langue choisie, pas des preferences du processus (qui ne changent
    /// qu'au lancement), et profite du cache des tables de `Bundle` (la
    /// locale d'un `LocalizedStringResource` choisit aussi la langue, mais
    /// relit les tables a chaque appel : 4 s pour 5000 trames). Formats des
    /// valeurs interpolees : ceux de la locale en vigueur.
    public func texte(_ valeur: String.LocalizationValue, paquet base: Bundle, table: String? = nil) -> String {
        let l = langue
        guard let p = paquet(base, langue: l) else {
            // Pas de `.lproj` pour la langue de developpement : la cle est le texte.
            return String(localized: valeur, table: Self.tableAbsente, bundle: base, locale: locale)
        }
        return String(localized: valeur, table: table, bundle: p, locale: locale)
    }

    /// Table qui n'existe dans aucun paquet : la recherche rend la cle.
    static let tableAbsente = "HaloAucuneTable"

    private let paquets = Mutex<[String: Bundle?]>([:])

    /// Sous-paquet `<langue>.lproj` de `base` ; nil pour la langue de
    /// developpement sans `.lproj` (aucune de ses cles n'a de variante).
    func paquet(_ base: Bundle, langue: Langue) -> Bundle? {
        let cle = base.bundlePath + "|" + langue.code
        if let p = paquets.withLock({ $0[cle] }) { return p }
        var p = base.path(forResource: langue.code, ofType: "lproj").flatMap(Bundle.init(path:))
        if p == nil, langue != .francais { p = base }
        paquets.withLock { $0[cle] = p }
        return p
    }
}

/// Ancre du paquet du framework.
private final class AncrePaquet {}

extension Bundle {
    /// Paquet du framework HaloProtocole (son catalogue de textes).
    public static let protocole = Bundle(for: AncrePaquet.self)
}

/// Texte du catalogue du framework, dans la langue en vigueur.
func tr(_ valeur: String.LocalizationValue) -> String {
    Localisation.partagee.texte(valeur, paquet: .protocole)
}
