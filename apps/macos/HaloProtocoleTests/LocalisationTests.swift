import Foundation
import Observation
import Testing
@testable import HaloProtocole

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

/// Les catalogues (`*.xcstrings`) lus dans le depot, et les cles que le
/// compilateur a extraites du code (`.stringsdata`).
enum Catalogues {
    static let racine = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // HaloProtocoleTests
        .deletingLastPathComponent()  // macos

    struct Catalogue: Sendable, CustomStringConvertible {
        let cible: String
        /// Table : le nom du fichier (`Localizable`, `Titres`).
        let table: String
        let chemin: URL
        var description: String { "\(cible)/\(table)" }
    }

    /// Catalogues du framework et de l'app, trouves dans leurs dossiers.
    static let tous: [Catalogue] = ["HaloProtocole", "HaloCompagnon"].flatMap { cible in
        let dossier = racine.appendingPathComponent(cible)
        let fichiers = FileManager.default.enumerator(at: dossier, includingPropertiesForKeys: nil)?
            .compactMap { $0 as? URL }.filter { $0.pathExtension == "xcstrings" } ?? []
        return fichiers.sorted { $0.path < $1.path }.map {
            Catalogue(cible: cible, table: $0.deletingPathExtension().lastPathComponent, chemin: $0)
        }
    }

    static func entrees(_ chemin: URL) throws -> (source: String, cles: [String: [String: Any]]) {
        let d = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: chemin)) as? [String: Any])
        let cles = try #require(d["strings"] as? [String: [String: Any]])
        return (d["sourceLanguage"] as? String ?? "", cles)
    }

    /// Dossier des produits (`.../Build/Products/Debug`) : celui du paquet de test.
    private final class Ancre {}
    static var produits: URL { Bundle(for: Ancre.self).bundleURL.deletingLastPathComponent() }

    /// `.stringsdata` d'une cible : `Build/Intermediates.noindex/<projet>.build/<config>/<cible>.build/Objects-normal/<arch>/`.
    static func stringsdata(cible: String) -> [URL] {
        let config = produits.lastPathComponent
        let intermediaires = produits.deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Intermediates.noindex")
        let fm = FileManager.default
        guard let projets = try? fm.contentsOfDirectory(at: intermediaires, includingPropertiesForKeys: nil) else { return [] }
        var fichiers: [URL] = []
        for projet in projets {
            let objets = projet.appendingPathComponent(config).appendingPathComponent("\(cible).build/Objects-normal")
            for arch in (try? fm.contentsOfDirectory(at: objets, includingPropertiesForKeys: nil)) ?? [] {
                for f in (try? fm.contentsOfDirectory(at: arch, includingPropertiesForKeys: nil)) ?? []
                where f.pathExtension == "stringsdata" {
                    fichiers.append(f)
                }
            }
        }
        return fichiers
    }

    static var stringsdataDisponibles: Bool {
        !stringsdata(cible: "HaloProtocole").isEmpty && !stringsdata(cible: "HaloCompagnon").isEmpty
    }

    /// Cles extraites du code d'une cible, par table.
    static func clesExtraites(cible: String) throws -> [String: Set<String>] {
        var cles: [String: Set<String>] = [:]
        for f in stringsdata(cible: cible) {
            let d = try JSONSerialization.jsonObject(with: Data(contentsOf: f)) as? [String: Any]
            let tables = d?["tables"] as? [String: [[String: Any]]] ?? [:]
            for (table, entrees) in tables {
                for e in entrees { if let k = e["key"] as? String { cles[table, default: []].insert(k) } }
            }
        }
        return cles
    }

    /// Specificateurs d'un format, sans leur position (`%1$@` -> `@`), dans l'ordre.
    static func specificateurs(_ s: String) -> [String] {
        let motif = /%(?:\d+\$)?(lld|ld|d|@|lf|f|%)/
        return s.matches(of: motif).map { String($0.output.1) }
    }

    /// Unites de texte d'une localisation : simple, ou formes du pluriel.
    static func unites(_ loc: [String: Any]) -> [String: [String: Any]] {
        if let u = loc["stringUnit"] as? [String: Any] { return ["": u] }
        let pluriel = (loc["variations"] as? [String: Any])?["plural"] as? [String: [String: Any]] ?? [:]
        return pluriel.compactMapValues { $0["stringUnit"] as? [String: Any] }
    }
}

@Suite("Catalogues de textes (francais source, anglais complet)")
struct CataloguesTests {
    @Test func catalogues() {
        #expect(Catalogues.tous.map(\.description)
                == ["HaloProtocole/Localizable", "HaloCompagnon/Localizable", "HaloCompagnon/Titres"])
    }

    @Test(arguments: Catalogues.tous)
    func chaqueCleATraductionAnglaise(_ c: Catalogues.Catalogue) throws {
        let (source, cles) = try Catalogues.entrees(c.chemin)
        #expect(source == "fr", "le francais est la langue de developpement")
        #expect(!cles.isEmpty)
        for (cle, entree) in cles {
            #expect(entree["extractionState"] as? String != "stale", "cle perimee : \(cle)")
            let locs = entree["localizations"] as? [String: [String: Any]] ?? [:]
            let en = try #require(locs["en"], "pas d'anglais : \(cle)")
            let unites = Catalogues.unites(en)
            #expect(!unites.isEmpty, "anglais vide : \(cle)")
            if unites.keys.contains(where: { !$0.isEmpty }) {
                #expect(unites["one"] != nil && unites["other"] != nil, "pluriel incomplet : \(cle)")
            }
            let attendus = Catalogues.specificateurs(cle)
            for (forme, u) in unites {
                #expect(u["state"] as? String == "translated", "anglais non valide (\(forme)) : \(cle)")
                let valeur = u["value"] as? String ?? ""
                #expect(!valeur.isEmpty, "anglais vide (\(forme)) : \(cle)")
                // Memes valeurs interpolees, de memes types (l'ordre peut changer avec %1$).
                #expect(Catalogues.specificateurs(valeur).sorted() == attendus.sorted(),
                        "specificateurs differents : \(cle) -> \(valeur)")
            }
            // Le francais du pluriel (s'il y en a un) garde lui aussi ses valeurs.
            for (forme, u) in Catalogues.unites(locs["fr"] ?? [:]) {
                let valeur = u["value"] as? String ?? ""
                #expect(Catalogues.specificateurs(valeur).sorted() == attendus.sorted(),
                        "francais (\(forme)) : \(cle) -> \(valeur)")
            }
        }
    }

    /// Le code et les catalogues vont ensemble : aucune cle extraite du code
    /// ne manque, aucune cle du catalogue n'est morte (rejouer
    /// `xcstringstool sync` apres un changement de texte, voir le README).
    @Test(.enabled(if: Catalogues.stringsdataDisponibles, "produits de compilation introuvables"),
          arguments: Catalogues.tous)
    func codeEtCatalogueAlignes(_ c: Catalogues.Catalogue) throws {
        let extraites = try Catalogues.clesExtraites(cible: c.cible)[c.table] ?? []
        let catalogue = Set(try Catalogues.entrees(c.chemin).cles.keys)
        #expect(!extraites.isEmpty)
        #expect(extraites.subtracting(catalogue).sorted() == [], "absentes du catalogue \(c)")
        #expect(catalogue.subtracting(extraites).sorted() == [], "inutilisees dans \(c)")
    }

    /// Chaque table utilisee par le code a son catalogue.
    @Test(.enabled(if: Catalogues.stringsdataDisponibles, "produits de compilation introuvables"),
          arguments: ["HaloProtocole", "HaloCompagnon"])
    func chaqueTableASonCatalogue(_ cible: String) throws {
        let tables = Set(try Catalogues.clesExtraites(cible: cible).keys)
        let catalogues = Set(Catalogues.tous.filter { $0.cible == cible }.map(\.table))
        #expect(tables == catalogues)
    }
}

@Suite("Reglage de la langue")
struct ChoixLangueTests {
    @Test func langueDuSysteme() {
        #expect(ChoixLangue.systeme.langue(preferences: ["en-GB", "fr-FR"]) == .anglais)
        #expect(ChoixLangue.systeme.langue(preferences: ["fr-CA", "en-US"]) == .francais)
        #expect(ChoixLangue.systeme.langue(preferences: ["de-DE", "en-US"]) == .anglais, "premiere langue servie")
        #expect(ChoixLangue.systeme.langue(preferences: ["de-DE"]) == .francais, "a defaut, la langue de developpement")
        #expect(ChoixLangue.systeme.langue(preferences: []) == .francais)
    }

    @Test func langueImposee() {
        #expect(ChoixLangue.anglais.langue(preferences: ["fr-FR"]) == .anglais)
        #expect(ChoixLangue.francais.langue(preferences: ["en-US"]) == .francais)
        #expect(ChoixLangue.allCases == [.systeme, .anglais, .francais])
        #expect(ChoixLangue(rawValue: "anglais") == .anglais, "valeur gardee dans les preferences")
    }

    @Test func localeDesFormats() {
        let france = Locale(identifier: "fr_FR")
        let anglais = ChoixLangue.anglais.locale(courante: france, preferences: ["fr-FR"])
        #expect(anglais.language.languageCode == .english)
        #expect(anglais.region == .france, "la region de l'utilisateur reste : anglais en France")
        let francais = ChoixLangue.francais.locale(courante: france, preferences: ["en-US"])
        #expect(francais == france, "la locale courante parle deja francais : gardee telle quelle")
        let systeme = ChoixLangue.systeme.locale(courante: Locale(identifier: "en_US"), preferences: ["en-US"])
        #expect(systeme.identifier == "en_US")
        let etats = ChoixLangue.francais.locale(courante: Locale(identifier: "en_US"), preferences: [])
        #expect(etats.language.languageCode == .french)
        #expect(etats.region == .unitedStates)
        // Les nombres suivent la locale choisie.
        #expect(2.5.formatted(.number.locale(Locale(identifier: "en_US"))) == "2.5")
        #expect(2.5.formatted(.number.locale(ChoixLangue.francais.locale(courante: france, preferences: []))) == "2,5")
    }

    @Test func changerDeLangueRedessineLesVues() {
        let l = Localisation(langue: .francais)
        final class Drapeau: @unchecked Sendable { var leve = false }
        let d = Drapeau()
        withObservationTracking { _ = l.langue } onChange: { d.leve = true }
        l.appliquer(.anglais, locale: Locale(identifier: "en_US"))
        #expect(d.leve, "une vue qui a lu un texte depend de la langue")
        #expect(l.langue == .anglais)
        #expect(l.locale.identifier == "en_US")
        #expect(l.texte("Consigne livrée", paquet: .protocole) == "Target delivered")
        l.appliquer(.francais, locale: Locale(identifier: "fr_FR"))
        #expect(l.texte("Consigne livrée", paquet: .protocole) == "Consigne livrée")
    }
}

@Suite("Sens decode dans les deux langues")
struct SensDecodeTests {
    static func ligne(_ json: String) throws -> MessageCarte {
        guard case .valide(let l) = DecodeurMessages.decoder(json: Data(json.utf8)) else {
            throw ErreurTransport("ligne invalide")
        }
        return l.message
    }

    static let tranches1 = #"{"v":1,"t":"etat","n":3,"ms":3,"bloc":"tranches","tranches":[{"tranche":"lum","charge":"C5A5","accuses":1,"essais":2,"paquets":3}]}"#
    static let tranches2 = #"{"v":1,"t":"etat","n":3,"ms":3,"bloc":"tranches","tranches":[{"tranche":"lum"},{"tranche":"a"}]}"#

    @Test(.langue(.francais)) func enFrancais() throws {
        let rx = try ExemplesSpec.decoder().compactMap { if case .rx(let r) = $0.message { r } else { nil } }
        #expect(Interpretation.rx(rx[1]) == "Luminosité A5 (niveau 180) · les deux · allumée")
        let abandon = try ExemplesSpec.decoder().compactMap { if case .livraison(let l) = $0.message { l } else { nil } }[1]
        #expect(Interpretation.livraison(abandon).hasPrefix("Consigne abandonnée : lampe injoignable"))
        #expect(Interpretation.resume(try Self.ligne(Self.tranches1)) == "1 tranche active")
        #expect(Interpretation.resume(try Self.ligne(Self.tranches2)) == "2 tranches actives")
        #expect(MoteurSession.Phase.attenteHello(essai: 2).libelle == "Connexion (essai 2)…")
        #expect(MotifLed.injoignable.libelle == "lampe injoignable")
        #expect(VerdictTx.maxRt.libelle == "MAX_RT (sans accusé)")
        #expect(ErreurLigne.tropLongue(octets: 130, max: 127).description
                == "Ligne trop longue : 130 octets, 127 au plus avec le préfixe id=.")
        #expect(PolitiqueCommandes.verdictConsole("reboot", transport: .usb)
                == .confirmation("Redémarre la carte (le port USB va se ré-énumérer)."))
        #expect(MoteurSession.Note.silence(secondes: 6).texte == "Silence de la carte depuis 6 s : json 1 renvoyé.")
        #expect(ValeurScalaire.booleen(true).description == "oui")
    }

    @Test(.langue(.anglais)) func enAnglais() throws {
        let rx = try ExemplesSpec.decoder().compactMap { if case .rx(let r) = $0.message { r } else { nil } }
        #expect(Interpretation.rx(rx[1]) == "Brightness A5 (level 180) · both · on")
        #expect(Interpretation.rx(rx[4]).hasPrefix("A button, press #1"))
        let abandon = try ExemplesSpec.decoder().compactMap { if case .livraison(let l) = $0.message { l } else { nil } }[1]
        #expect(Interpretation.livraison(abandon).hasPrefix("Target abandoned: lamp unreachable · version"))
        let relances = try ExemplesSpec.decoder().compactMap { if case .relance(let r) = $0.message { r } else { nil } }
        let relance = try #require(relances.first)
        // Les quantites suivent les separateurs de milliers de la locale (1204 : "1,204", "1 204"...).
        let texteRelance = Interpretation.relance(relance)
        #expect(texteRelance.hasPrefix("Module restart: deaf chip (level 1) · 1"))
        #expect(texteRelance.contains("204 re-arms outside RX, in 2"))
        #expect(texteRelance.hasSuffix("870 ms · succeeded in 312 ms · 1 in total"))
        #expect(Interpretation.resume(try Self.ligne(Self.tranches1)) == "1 active slice")
        #expect(Interpretation.resume(try Self.ligne(Self.tranches2)) == "2 active slices")
        #expect(MoteurSession.Phase.attenteHello(essai: 2).libelle == "Connecting (attempt 2)…")
        #expect(MoteurSession.Phase.connecte.libelle == "Connected")
        #expect(MotifLed.injoignable.libelle == "lamp unreachable")
        #expect(MotifLed.desappairage.description == "fast red/purple: release the button to unpair")
        #expect(VerdictTx.maxRt.libelle == "MAX_RT (no ack)")
        #expect(CodeReponse.accepte.libelle == "accepted, delivery to follow")
        #expect(ErreurLigne.tropLongue(octets: 130, max: 127).description
                == "Line too long: 130 bytes, 127 at most including the id= prefix.")
        #expect(PolitiqueCommandes.verdictConsole("reboot", transport: .usb)
                == .confirmation("Restarts the board (the USB port will re-enumerate)."))
        #expect(MoteurSession.Note.aucuneReponse.texte.hasPrefix("No response: wrong port"))
        #expect(ValeurScalaire.booleen(false).description == "no")
        // Termes techniques intacts : champs JSON, commandes, hexa, unites.
        #expect(Interpretation.etat(EtatLampe(marche: true, lum: 0xA5, niveau: 180, temp: 53, mired: 268))
                == "on · lum A5 (level 180) · temp 53 (268 mireds)")
    }

    @Test(.langue(.anglais)) func plurielsAnglais() throws {
        let un = try Self.ligne(#"{"v":1,"t":"tx","n":1,"ms":1,"verdict":"ack","accuses":1,"sautes":1}"#)
        let deux = try Self.ligne(#"{"v":1,"t":"tx","n":1,"ms":1,"verdict":"ack","accuses":2,"sautes":2}"#)
        #expect(Interpretation.resume(un).hasSuffix("· ack · 1 ack · 1 skipped before"))
        #expect(Interpretation.resume(deux).hasSuffix("· ack · 2 acks · 2 skipped before"))
    }

    @Test(.langue(.francais)) func plurielsFrancais() throws {
        // En francais, 0 et 1 sont au singulier.
        let zero = try Self.ligne(#"{"v":1,"t":"tx","n":1,"ms":1,"verdict":"ack","accuses":0}"#)
        let deux = try Self.ligne(#"{"v":1,"t":"tx","n":1,"ms":1,"verdict":"ack","accuses":2}"#)
        #expect(Interpretation.resume(zero).hasSuffix("· accusé · 0 accusé"))
        #expect(Interpretation.resume(deux).hasSuffix("· accusé · 2 accusés"))
    }

    @Test func langueImposeeParTache() async {
        // Deux taches en parallele, chacune sa langue : aucune ne deteint sur l'autre.
        async let fr = Localisation.$imposee.withValue(.francais) { EtatLien.perdu.libelle }
        async let en = Localisation.$imposee.withValue(.anglais) { EtatLien.perdu.libelle }
        #expect(await [fr, en] == ["injoignable", "unreachable"])
    }
}
