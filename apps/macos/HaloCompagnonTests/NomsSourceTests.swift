import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

/// Menu Source : seulement les cartes Espressif, nommees par leur modele et
/// leur MAC (repertoire appris des `hello`), le chemin du port en sous-titre.
@Suite("Noms du menu Source", .serialized, .langue(.francais))
@MainActor
struct NomsSourceTests {
    /// Preferences isolees : jamais celles de l'app.
    static func preferences() -> (UserDefaults, String) {
        let nom = "halo-tests-\(UUID().uuidString)"
        return (UserDefaults(suiteName: nom)!, nom)
    }

    static func port(_ chemin: String, vid: Int?, serie: String?) -> PortUSB {
        PortUSB(chemin: chemin, vid: vid, pid: vid == nil ? nil : 0x1001, serie: serie, produit: nil)
    }

    @Test func portsEspressifSeulement() {
        let ports = [
            Self.port("/dev/cu.usbmodem144401", vid: 0x303A, serie: "58:E6:C5:DD:7E:F0"),
            Self.port("/dev/cu.Bluetooth-Incoming-Port", vid: nil, serie: nil),
            Self.port("/dev/cu.debug-console", vid: nil, serie: nil),
            Self.port("/dev/cu.usbmodem010NTHMAX5292", vid: 0x043E, serie: "010NTHMAX529"),
        ]
        #expect(Pont.portsVisibles(ports).map(\.chemin) == ["/dev/cu.usbmodem144401"])
    }

    @Test func titresAppris() throws {
        let (d, nom) = Self.preferences()
        defer { d.removePersistentDomain(forName: nom) }
        var r = RepertoirePonts()
        r.noter(mac: "58E6C5DD7EF0", serie: "HALO1-58E6C5DD7EF0", srp: "561F9A6463953778")
        d.set(try JSONEncoder().encode(r), forKey: Pont.cleRepertoire)
        let pont = Pont(trousseau: TrousseauMemoire(), preferences: d)
        let branche = Self.port("/dev/cu.usbmodem144401", vid: 0x303A, serie: "58:E6:C5:DD:7E:F0")
        #expect(pont.titre(port: branche) == "HALO1 · 58:E6:C5:DD:7E:F0")
        #expect(pont.titre(pont: PontConnu(nom: "561F9A6463953778", empreinte: "CDE010BE")) == "HALO1 · 58:E6:C5:DD:7E:F0")
        #expect(pont.titre(pont: PontConnu(nom: "0123456789ABCDEF", empreinte: "A757109E")) == "Pont Halo",
                "pont jamais vu : nom generique")
        let jamaisVu = Self.port("/dev/cu.usbmodem1", vid: 0x303A, serie: "AA:BB:CC:DD:EE:FF")
        #expect(pont.titre(port: jamaisVu) == "ESP32 · AA:BB:CC:DD:EE:FF")
        pont.deconnecter()
    }
}
