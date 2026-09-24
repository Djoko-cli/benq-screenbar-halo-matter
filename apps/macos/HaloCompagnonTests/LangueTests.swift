import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

/// Impose une langue aux textes du test (et de ses sous-taches), sans toucher
/// a la langue de l'app : les suites tournent en parallele.
struct LangueImposee: TestTrait, SuiteTrait, TestScoping {
    let langue: Langue
    var isRecursive: Bool { true }

    func provideScope(for test: Test, testCase: Test.Case?,
                      performing function: @Sendable () async throws -> Void) async throws {
        try await Localisation.$imposee.withValue(langue) { try await function() }
    }
}

extension Trait where Self == LangueImposee {
    static func langue(_ l: Langue) -> Self { LangueImposee(langue: l) }
}

@Suite("Langue de l'app", .serialized)
@MainActor
struct LangueAppTests {
    /// Preferences des tests, videes avant et apres : le choix de
    /// l'utilisateur n'est pas touche, et un seul fichier sert a chaque passage.
    static let nomDefauts = "fr.djoko.halo.compagnon.tests.langue"

    static func defauts() throws -> UserDefaults {
        let d = try #require(UserDefaults(suiteName: nomDefauts))
        d.removePersistentDomain(forName: nomDefauts)
        return d
    }

    static func propre(_ d: UserDefaults, _ cle: String) -> Any? {
        d.persistentDomain(forName: nomDefauts)?[cle]
    }

    @Test(.langue(.anglais)) func textesDeLAppEnAnglais() {
        #expect(Ecran.allCases.map(\.titre) == ["Dashboard", "Live Frames", "Charts", "Controls & Console"])
        #expect(CategorieTrame.rx.libelle == "Received (rx)")
        #expect(Format.oui(true) == "yes")
        #expect(Format.duree(secondes: 90_061) == "1 d 1 h 1 min")
        #expect(TransportDemo.nomLisible == "Demo (replay of demo-halo.jsonl)")
        #expect(Marqueur(id: 1, date: .now, etiquette: .abandon(.injoignable)).texte == "lamp unreachable")
        #expect(Marqueur(id: 1, date: .now, etiquette: .abandon(nil)).texte == "abandoned")
    }

    @Test(.langue(.francais)) func textesDeLAppEnFrancais() {
        #expect(Ecran.allCases.map(\.titre) == ["Tableau de bord", "Trames en direct", "Graphiques", "Commandes et console"])
        #expect(CategorieTrame.rx.libelle == "Reçues (rx)")
        #expect(Format.oui(true) == "oui")
        #expect(Format.duree(secondes: 90_061) == "1 j 1 h 1 min")
        #expect(Marqueur(id: 1, date: .now, etiquette: .relance(.sourde)).texte == "puce sourde")
    }

    /// Les formats suivent la langue choisie (avec la region de l'utilisateur,
    /// qui depend de la machine : on ne compare que ce que la langue change).
    @Test func formatsSelonLaLangue() {
        let midi = Date(timeIntervalSince1970: 43_200.5)
        for l in Langue.allCases {
            let h = Localisation.$imposee.withValue(l) { Format.heure(midi) }
            #expect(h.contains("00.500") || h.contains("00,500"), "heure a la milliseconde : \(h)")
        }
        // Unites d'octets : "ko" en francais, "kB" en anglais.
        let octetsEn = Localisation.$imposee.withValue(.anglais) { Format.octets(1536) }
        let octetsFr = Localisation.$imposee.withValue(.francais) { Format.octets(1536) }
        #expect(octetsEn != octetsFr, "\(octetsEn) / \(octetsFr)")
    }

    @Test func journalDesTramesRelocalise() throws {
        let json = #"{"v":1,"t":"livraison","n":7,"ms":7,"issue":"abandon","cause":"injoignable","version":3,"ids":[]}"#
        guard case .valide(let l) = DecodeurMessages.decoder(json: Data(json.utf8)) else {
            Issue.record("livraison invalide")
            return
        }
        let gamma = ParametresGamma(EtatPont())
        let fr = Localisation.$imposee.withValue(.francais) {
            EntreeTrame(id: 1, date: .now, n: 7, ms: 7, type: "livraison", message: l.message, json: json,
                        historique: false, gamma: gamma, correspondance: gamma.correspondance)
        }
        #expect(fr.resume.hasPrefix("Consigne abandonnée : lampe injoignable"))
        let en = Localisation.$imposee.withValue(.anglais) { fr.relocalisee(correspondance: gamma.correspondance) }
        #expect(en.resume.hasPrefix("Target abandoned: lamp unreachable"))
        #expect(en.cleRecherche.contains("target abandoned"), "la recherche suit la langue affichee")
        #expect(en.id == fr.id && en.json == fr.json)
    }

    @Test func choixPersisteEtApplique() throws {
        let d = try Self.defauts()
        defer { d.removePersistentDomain(forName: Self.nomDefauts) }
        let nom = Self.nomDefauts
        let l = Localisation(langue: .francais)
        #expect(ReglageLangue.choix(d) == .systeme, "par defaut : la langue du systeme")

        ReglageLangue.appliquer(.anglais, defauts: d, domaine: nom, localisation: l)
        #expect(ReglageLangue.choix(d) == .anglais)
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguages) as? [String] == ["en"],
                "les menus de macOS suivront au prochain lancement")
        #expect(l.langue == .anglais)
        #expect(l.locale.language.languageCode == .english)

        ReglageLangue.appliquer(.francais, defauts: d, domaine: nom, localisation: l)
        #expect(l.langue == .francais)
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguages) as? [String] == ["fr"])

        ReglageLangue.appliquer(.systeme, defauts: d, domaine: nom, localisation: l)
        #expect(ReglageLangue.choix(d) == .systeme)
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguages) == nil, "la langue du systeme reprend la main")
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguagesAvantChoix) == nil)
        #expect(l.langue == Langue.preferee(parmi: ReglageLangue.languesSysteme(d)))
    }

    /// Une langue choisie pour l'app dans Reglages Systeme (son `AppleLanguages`)
    /// revient avec « Langue du systeme », apres un passage par English/Francais.
    @Test func langueDeReglagesSystemeRendue() throws {
        let d = try Self.defauts()
        defer { d.removePersistentDomain(forName: Self.nomDefauts) }
        let nom = Self.nomDefauts
        let l = Localisation(langue: .francais)
        d.set(["en-GB"], forKey: ReglageLangue.cleAppleLanguages)

        // Au lancement, "Langue du systeme" la garde.
        ReglageLangue.appliquer(.systeme, defauts: d, domaine: nom, lancement: true, localisation: l)
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguages) as? [String] == ["en-GB"])
        // (-AppleLanguages de la ligne de commande, `-testLanguage`, passe devant.)
        #expect(l.langue == Langue.preferee(parmi: ReglageLangue.languesSysteme(d)))

        ReglageLangue.appliquer(.francais, defauts: d, domaine: nom, localisation: l)
        ReglageLangue.appliquer(.anglais, defauts: d, domaine: nom, localisation: l)
        ReglageLangue.appliquer(.francais, defauts: d, domaine: nom, lancement: true, localisation: l)
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguages) as? [String] == ["fr"])
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguagesAvantChoix) as? [String] == ["en-GB"],
                "gardee a travers les choix et les lancements")

        ReglageLangue.appliquer(.systeme, defauts: d, domaine: nom, localisation: l)
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguages) as? [String] == ["en-GB"])
        #expect(Self.propre(d, ReglageLangue.cleAppleLanguagesAvantChoix) == nil)
        #expect(l.langue == Langue.preferee(parmi: ReglageLangue.languesSysteme(d)))
    }
}
