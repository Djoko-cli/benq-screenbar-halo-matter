import Foundation
import Testing
@testable import HaloProtocole

@Suite("Repertoire des ponts (noms du menu Source)")
struct RepertoirePontsTests {
    @Test func mac() {
        #expect(RepertoirePonts.mac("58:E6:C5:66:5B:CE") == "58E6C5665BCE", "numero de serie USB")
        #expect(RepertoirePonts.mac("58e6c5665bce") == "58E6C5665BCE", "hello identite")
        #expect(RepertoirePonts.mac("58-E6-C5-66-5B-CE") == "58E6C5665BCE")
        #expect(RepertoirePonts.mac("58:E6:C5:66:5B") == nil)
        #expect(RepertoirePonts.mac("221KFEKNM153") == nil, "numero de serie d'un ecran LG")
        #expect(RepertoirePonts.mac(nil) == nil)
        #expect(RepertoirePonts.macLisible("58E6C5665BCE") == "58:E6:C5:66:5B:CE")
    }

    @Test func modele() {
        #expect(RepertoirePonts.modele(serie: "HALO1-58E6C5665BCE", mac: "58E6C5665BCE") == "HALO1")
        #expect(RepertoirePonts.modele(serie: "HALO-2-58E6C5665BCE", mac: "58E6C5665BCE") == "HALO-2")
        #expect(RepertoirePonts.modele(serie: "HALO1-AABBCCDDEEFF", mac: "58E6C5665BCE") == nil, "autre MAC")
        #expect(RepertoirePonts.modele(serie: "-58E6C5665BCE", mac: "58E6C5665BCE") == nil)
        #expect(RepertoirePonts.modele(serie: "HALO1", mac: "58E6C5665BCE") == nil)
        #expect(RepertoirePonts.modele(serie: nil, mac: "58E6C5665BCE") == nil)
    }

    @Test func noterEtNommer() {
        var r = RepertoirePonts()
        #expect(r.titre(mac: "58E6C5665BCE") == "ESP32 · 58:E6:C5:66:5B:CE", "jamais connecte")
        let change1 = r.noter(mac: "58E6C5665BCE", serie: "HALO1-58E6C5665BCE", srp: nil)
        #expect(change1)
        #expect(r.titre(mac: "58E6C5665BCE") == "HALO1 · 58:E6:C5:66:5B:CE")
        let change2 = r.noter(mac: "58E6C5665BCE", serie: "HALO1-58E6C5665BCE", srp: "56B1E064401F74EF")
        #expect(change2)
        let change3 = r.noter(mac: "58E6C5665BCE", serie: "HALO1-58E6C5665BCE", srp: "56B1E064401F74EF")
        #expect(!change3, "rien de neuf")
        #expect(r.mac(pourSrp: "56B1E064401F74EF") == "58E6C5665BCE")
        let change4 = r.noter(mac: nil, serie: "HALO1-58E6C5665BCE", srp: "X")
        #expect(!change4, "sans MAC : rien")
        // Serie sans la MAC (build diag, autre firmware) : le modele appris reste.
        let change5 = r.noter(mac: "58E6C5665BCE", serie: nil, srp: nil)
        #expect(!change5)
        #expect(r.titre(mac: "58E6C5665BCE") == "HALO1 · 58:E6:C5:66:5B:CE")
    }

    /// Un nom SRP n'appartient qu'a une carte : la carte qui le reprend l'emporte.
    @Test func nomSrpRepris() {
        var r = RepertoirePonts()
        r.noter(mac: "58E6C5665BCE", serie: "HALO1-58E6C5665BCE", srp: "56B1E064401F74EF")
        let change6 = r.noter(mac: "AABBCCDDEEFF", serie: "HALO1-AABBCCDDEEFF", srp: "56B1E064401F74EF")
        #expect(change6)
        #expect(r.mac(pourSrp: "56B1E064401F74EF") == "AABBCCDDEEFF")
        #expect(r.parMac["58E6C5665BCE"]?.srp == nil && r.parMac["58E6C5665BCE"]?.modele == "HALO1")
    }

    @Test func codable() throws {
        var r = RepertoirePonts()
        r.noter(mac: "58E6C5665BCE", serie: "HALO1-58E6C5665BCE", srp: "56B1E064401F74EF")
        let relu = try JSONDecoder().decode(RepertoirePonts.self, from: try JSONEncoder().encode(r))
        #expect(relu == r)
    }
}
