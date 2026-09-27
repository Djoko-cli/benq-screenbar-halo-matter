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
            Self.port("/dev/cu.usbmodem144401", vid: 0x303A, serie: "58:E6:C5:66:5B:CE"),
            Self.port("/dev/cu.Bluetooth-Incoming-Port", vid: nil, serie: nil),
            Self.port("/dev/cu.debug-console", vid: nil, serie: nil),
            Self.port("/dev/cu.usbmodem221KFEKNM1532", vid: 0x043E, serie: "221KFEKNM153"),
        ]
        #expect(Pont.portsVisibles(ports).map(\.chemin) == ["/dev/cu.usbmodem144401"])
    }

    @Test func titresAppris() throws {
        let (d, nom) = Self.preferences()
        defer { d.removePersistentDomain(forName: nom) }
        var r = RepertoirePonts()
        r.noter(mac: "58E6C5665BCE", serie: "HALO1-58E6C5665BCE", srp: "56B1E064401F74EF")
        d.set(try JSONEncoder().encode(r), forKey: Pont.cleRepertoire)
        let pont = Pont(trousseau: TrousseauMemoire(), preferences: d)
        let branche = Self.port("/dev/cu.usbmodem144401", vid: 0x303A, serie: "58:E6:C5:66:5B:CE")
        #expect(pont.titre(port: branche) == "HALO1 · 58:E6:C5:66:5B:CE")
        #expect(pont.titre(pont: PontConnu(nom: "56B1E064401F74EF", empreinte: "5A84F173")) == "HALO1 · 58:E6:C5:66:5B:CE")
        #expect(pont.titre(pont: PontConnu(nom: "0123456789ABCDEF", empreinte: "B7B89D09")) == "Pont Halo",
                "pont jamais vu : nom generique")
        let jamaisVu = Self.port("/dev/cu.usbmodem1", vid: 0x303A, serie: "AA:BB:CC:DD:EE:FF")
        #expect(pont.titre(port: jamaisVu) == "ESP32 · AA:BB:CC:DD:EE:FF")
        pont.deconnecter()
    }
}
