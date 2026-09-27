import Foundation
import Testing
@testable import HaloProtocole

/// Ancre du temps (heure locale <-> `ms` de la carte) et phase du voyant
/// (`led.depuis_ms`, rev 4).
@Suite("Ancre du temps et phase du voyant")
struct AncreTempsTests {
    static let t0 = Date(timeIntervalSinceReferenceDate: 1_000_000)

    /// Egalite a la microseconde : les dates passent par des sommes de flottants.
    static func proche(_ a: Date?, _ b: Date) -> Bool {
        guard let a else { return false }
        return abs(a.timeIntervalSince(b)) < 1e-6
    }

    static func ligne(_ json: String) throws -> LigneMachine {
        guard case .valide(let l) = DecodeurMessages.decoder(json: Data(json.utf8)) else {
            throw ErreurTransport("ligne invalide : \(json)")
        }
        return l
    }

    static func hello(ms: UInt32) throws -> LigneMachine {
        try ligne(#"{"v":1,"t":"hello","bloc":"base","n":1,"ms":\#(ms)}"#)
    }

    static func sante(n: Int, ms: UInt32, motif: String = "operationnel", depuis: Int? = nil) throws -> LigneMachine {
        let d = depuis.map { #","depuis_ms":\#($0)"# } ?? ""
        return try ligne(#"{"v":1,"t":"etat","bloc":"sante","n":\#(n),"ms":\#(ms),"led":{"motif":"\#(motif)","test":false\#(d)}}"#)
    }

    /// Le `hello` part en tete d'un instantane : il arrive parfois tard. La
    /// ligne arrivee le plus vite par rapport a lui devient l'ancre.
    @Test func ancreAffineeParLaLigneLaPlusRapide() throws {
        var e = EtatPont()
        e.appliquer(try Self.hello(ms: 10_000), recueA: Self.t0)
        // Predite a t0 + 2 s, arrivee 0,35 s plus tot : le hello avait du retard.
        e.appliquer(try Self.sante(n: 2, ms: 12_000), recueA: Self.t0.addingTimeInterval(1.65))
        #expect(e.ancre?.ms == 12_000)
        #expect(Self.proche(e.dater(ms: 20_000, recueA: .distantPast), Self.t0.addingTimeInterval(9.65)))
        // Une ligne plus lente que la prediction ne change rien.
        e.appliquer(try Self.sante(n: 3, ms: 14_000), recueA: Self.t0.addingTimeInterval(4.2))
        #expect(e.ancre?.ms == 12_000)
    }

    /// La fenetre suit la derive des horloges : la meilleure ligne sort apres
    /// `fenetreAncre` lignes, l'ancre revient aux plus recentes.
    @Test func fenetreGlissante() throws {
        var e = EtatPont()
        e.appliquer(try Self.hello(ms: 10_000), recueA: Self.t0)
        e.appliquer(try Self.sante(n: 2, ms: 12_000), recueA: Self.t0.addingTimeInterval(1.5))
        #expect(e.ancre?.ms == 12_000)
        for i in 0..<EtatPont.fenetreAncre {
            let ms = UInt32(14_000 + 100 * i)
            e.appliquer(try Self.sante(n: 3 + i, ms: ms), recueA: Self.t0.addingTimeInterval(Double(ms - 10_000) / 1000))
        }
        #expect(e.ancre?.ms != 12_000, "la ligne rapide est sortie de la fenetre")
        #expect(Self.proche(e.dater(ms: 30_000, recueA: .distantPast), Self.t0.addingTimeInterval(20)))
    }

    /// Nouveau `hello` : l'ancre repart de lui, la fenetre est videe.
    @Test func helloRepartDeZero() throws {
        var e = EtatPont()
        e.appliquer(try Self.hello(ms: 10_000), recueA: Self.t0)
        e.appliquer(try Self.sante(n: 2, ms: 12_000), recueA: Self.t0.addingTimeInterval(1.0))
        let t1 = Self.t0.addingTimeInterval(100)
        e.appliquer(try Self.hello(ms: 500), recueA: t1)
        #expect(e.ancre?.ms == 500 && e.ancre?.date == t1)
    }

    /// Avec `depuis_ms`, le voyant part du depart de phase de la carte, pas
    /// de la premiere ligne vue (banc du 25/09 : lueur decalee).
    @Test func phaseDuVoyantCaleeSurLaCarte() throws {
        var e = EtatPont()
        e.appliquer(try Self.hello(ms: 10_000), recueA: Self.t0)
        e.appliquer(try Self.sante(n: 2, ms: 20_000, depuis: 7_000), recueA: Self.t0.addingTimeInterval(10.2))
        #expect(e.motifLed == .operationnel)
        #expect(Self.proche(e.motifLedDepuis, Self.t0.addingTimeInterval(3)))
        // Evenement led : meme calcul (retour en ligne, phase d'avant).
        let led = try Self.ligne(#"{"v":1,"t":"led","n":3,"ms":21000,"motif":"livree","avant":"operationnel","test":false,"depuis_ms":0}"#)
        e.appliquer(led, recueA: Self.t0.addingTimeInterval(11.1))
        #expect(e.motifLed == .livree)
        #expect(Self.proche(e.motifLedDepuis, Self.t0.addingTimeInterval(11)))
    }

    /// Firmware sans `depuis_ms` : la phase part de la ligne ou le motif change.
    @Test func sansDepuisMsCommeAvant() throws {
        var e = EtatPont()
        e.appliquer(try Self.hello(ms: 10_000), recueA: Self.t0)
        e.appliquer(try Self.sante(n: 2, ms: 12_000), recueA: Self.t0.addingTimeInterval(2.1))
        let depart = e.motifLedDepuis
        #expect(Self.proche(depart, Self.t0.addingTimeInterval(2)))
        e.appliquer(try Self.sante(n: 3, ms: 14_000), recueA: Self.t0.addingTimeInterval(4.1))
        #expect(e.motifLedDepuis == depart, "meme motif : la phase ne repart pas")
    }
}
