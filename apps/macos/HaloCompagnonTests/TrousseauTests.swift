import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

@Suite("Trousseau des cles reseau (10.4)", .langue(.francais))
struct TrousseauTests {
    static let cle = Data((0..<32).map { UInt8($0) })

    static func exercer(_ t: any TrousseauCles) throws {
        #expect(t.lister().isEmpty)
        #expect(throws: ErreurTrousseau.absente("56B1E064401F74EF")) { try t.lire(nom: "56B1E064401F74EF") }
        try t.ranger(nom: "56B1E064401F74EF", cle: cle, empreinte: "630DCD29")
        #expect(t.lister() == [PontConnu(nom: "56B1E064401F74EF", empreinte: "630DCD29")])
        #expect(try t.lire(nom: "56B1E064401F74EF") == cle)
        // Nouvelle cle pour le meme pont : remplacee, pas doublee.
        try t.ranger(nom: "56B1E064401F74EF", cle: Data(repeating: 9, count: 32), empreinte: "11111111")
        #expect(t.lister().count == 1)
        #expect(try t.lire(nom: "56B1E064401F74EF") == Data(repeating: 9, count: 32))
        try t.oublier(nom: "56B1E064401F74EF")
        #expect(t.lister().isEmpty)
        try t.oublier(nom: "56B1E064401F74EF")  // deja oublie : sans erreur
    }

    @Test func enMemoire() throws {
        try Self.exercer(TrousseauMemoire())
    }

    /// Vrai trousseau (service de test, nettoye) : seulement sur demande,
    /// `TEST_RUNNER_HALO_TEST_TROUSSEAU=1 xcodebuild ... test`.
    @Test(.enabled(if: ProcessInfo.processInfo.environment["HALO_TEST_TROUSSEAU"] == "1"))
    func trousseauDuMac() throws {
        let t = TrousseauSysteme(service: "fr.djoko.halo.pont.tests")
        try? t.oublier(nom: "56B1E064401F74EF")
        try Self.exercer(t)
    }

    @Test func pontConnu() {
        #expect(PontConnu(nom: "56B1E064401F74EF", empreinte: "630DCD29").hote == "56B1E064401F74EF.local")
    }
}
