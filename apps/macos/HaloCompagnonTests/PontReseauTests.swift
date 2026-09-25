import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

@Suite("Source reseau du modele", .serialized, .langue(.francais))
@MainActor
struct PontReseauTests {
    static let nom = "561F9A6463953778"

    @Test func cleAbsenteArreteSansReessayer() async {
        let pont = Pont(trousseau: TrousseauMemoire())
        pont.connecter(.reseau(nom: Self.nom))
        #expect(pont.alerteReseau == .trousseau(.absente(Self.nom)))
        if case .erreur = pont.etatTransport {} else { Issue.record("etat \(pont.etatTransport)") }
        try? await Task.sleep(for: .milliseconds(500))
        if case .erreur = pont.etatTransport {} else { Issue.record("aucune reprise attendue") }
        pont.deconnecter()
    }

    @Test func choixDeLaSource() {
        let connu = PontConnu(nom: Self.nom, empreinte: "630DCD29")
        let c6 = PortUSB(chemin: "/dev/cu.usbmodem1", vid: 0x303A, pid: 0x1001, serie: nil, produit: nil)
        let autre = PortUSB(chemin: "/dev/cu.usbmodem2", vid: 0x043E, pid: 0x9A39, serie: nil, produit: nil)
        #expect(Pont.choisirSource(ports: [autre, c6], connus: [connu]) == .serie(chemin: c6.chemin, serie: nil))
        #expect(Pont.choisirSource(ports: [autre], connus: [connu]) == .reseau(nom: Self.nom))
        #expect(Pont.choisirSource(ports: [], connus: []) == .demo)
    }

    @Test func pontsConnusEtOubli() throws {
        let t = TrousseauMemoire()
        try t.ranger(nom: Self.nom, cle: Data(repeating: 1, count: 32), empreinte: "630DCD29")
        let pont = Pont(trousseau: t)
        #expect(pont.pontsConnus.map(\.nom) == [Self.nom])
        pont.oublierPont(Self.nom)
        #expect(pont.pontsConnus.isEmpty)
        #expect(t.lister().isEmpty)
    }

    @Test func textePasDeRoute() {
        #expect(AlerteReseau.textePasDeRoute(assistant: false).contains("installer.sh"))
        #expect(!AlerteReseau.textePasDeRoute(assistant: true).contains("installer.sh"))
    }
}
