import Foundation
import Testing
@testable import HaloProtocole

@Suite("Repertoire des ponts (noms du menu Source)")
struct RepertoirePontsTests {
    @Test func mac() {
        #expect(RepertoirePonts.mac("58:E6:C5:DD:7E:F0") == "58E6C5DD7EF0", "numero de serie USB")
        #expect(RepertoirePonts.mac("58e6c5dd7ef0") == "58E6C5DD7EF0", "hello identite")
        #expect(RepertoirePonts.mac("58-E6-C5-DD-7E-F0") == "58E6C5DD7EF0")
        #expect(RepertoirePonts.mac("58:E6:C5:DD:7E") == nil)
        #expect(RepertoirePonts.mac("010NTHMAX529") == nil, "numero de serie d'un ecran LG")
        #expect(RepertoirePonts.mac(nil) == nil)
        #expect(RepertoirePonts.macLisible("58E6C5DD7EF0") == "58:E6:C5:DD:7E:F0")
    }

    @Test func modele() {
        #expect(RepertoirePonts.modele(serie: "HALO1-58E6C5DD7EF0", mac: "58E6C5DD7EF0") == "HALO1")
        #expect(RepertoirePonts.modele(serie: "HALO-2-58E6C5DD7EF0", mac: "58E6C5DD7EF0") == "HALO-2")
        #expect(RepertoirePonts.modele(serie: "HALO1-AABBCCDDEEFF", mac: "58E6C5DD7EF0") == nil, "autre MAC")
        #expect(RepertoirePonts.modele(serie: "-58E6C5DD7EF0", mac: "58E6C5DD7EF0") == nil)
        #expect(RepertoirePonts.modele(serie: "HALO1", mac: "58E6C5DD7EF0") == nil)
        #expect(RepertoirePonts.modele(serie: nil, mac: "58E6C5DD7EF0") == nil)
    }

    @Test func noterEtNommer() {
        var r = RepertoirePonts()
        #expect(r.titre(mac: "58E6C5DD7EF0") == "ESP32 · 58:E6:C5:DD:7E:F0", "jamais connecte")
        let change1 = r.noter(mac: "58E6C5DD7EF0", serie: "HALO1-58E6C5DD7EF0", srp: nil)
        #expect(change1)
        #expect(r.titre(mac: "58E6C5DD7EF0") == "HALO1 · 58:E6:C5:DD:7E:F0")
        let change2 = r.noter(mac: "58E6C5DD7EF0", serie: "HALO1-58E6C5DD7EF0", srp: "561F9A6463953778")
        #expect(change2)
        let change3 = r.noter(mac: "58E6C5DD7EF0", serie: "HALO1-58E6C5DD7EF0", srp: "561F9A6463953778")
        #expect(!change3, "rien de neuf")
        #expect(r.mac(pourSrp: "561F9A6463953778") == "58E6C5DD7EF0")
        let change4 = r.noter(mac: nil, serie: "HALO1-58E6C5DD7EF0", srp: "X")
        #expect(!change4, "sans MAC : rien")
        // Serie sans la MAC (build diag, autre firmware) : le modele appris reste.
        let change5 = r.noter(mac: "58E6C5DD7EF0", serie: nil, srp: nil)
        #expect(!change5)
        #expect(r.titre(mac: "58E6C5DD7EF0") == "HALO1 · 58:E6:C5:DD:7E:F0")
    }

    /// Un nom SRP n'appartient qu'a une carte : la carte qui le reprend l'emporte.
    @Test func nomSrpRepris() {
        var r = RepertoirePonts()
        r.noter(mac: "58E6C5DD7EF0", serie: "HALO1-58E6C5DD7EF0", srp: "561F9A6463953778")
        let change6 = r.noter(mac: "AABBCCDDEEFF", serie: "HALO1-AABBCCDDEEFF", srp: "561F9A6463953778")
        #expect(change6)
        #expect(r.mac(pourSrp: "561F9A6463953778") == "AABBCCDDEEFF")
        #expect(r.parMac["58E6C5DD7EF0"]?.srp == nil && r.parMac["58E6C5DD7EF0"]?.modele == "HALO1")
    }

    @Test func codable() throws {
        var r = RepertoirePonts()
        r.noter(mac: "58E6C5DD7EF0", serie: "HALO1-58E6C5DD7EF0", srp: "561F9A6463953778")
        let relu = try JSONDecoder().decode(RepertoirePonts.self, from: try JSONEncoder().encode(r))
        #expect(relu == r)
    }
}
