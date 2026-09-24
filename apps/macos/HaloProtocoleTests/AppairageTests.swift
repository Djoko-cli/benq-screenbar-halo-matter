import Testing
@testable import HaloProtocole

/// Codes d'appairage Matter du pont : groupes comme dans Maison, charge du QR verifiee.
struct AppairageTests {
    @Test func codeManuelGroupe() {
        #expect(CodeAppairage.lisible("34970112332") == "3497-011-2332")
        #expect(CodeAppairage.lisible("749701123365521327") == "749701123365521327", "18 chiffres : tel quel")
        #expect(CodeAppairage.lisible("749701123365521327694") == "7497-011-2336-55213-27694")
        #expect(CodeAppairage.lisible("3497-011-2332") == "3497-011-2332", "deja groupe : tel quel")
        #expect(CodeAppairage.lisible("") == "")
        #expect(CodeAppairage.lisible("3497011233٢") == "3497011233٢", "chiffre non ASCII : tel quel")
    }

    @Test func chargeDuQR() {
        #expect(CodeAppairage.chargeValide("MT:Y.K9042C00KA0648G00"))
        #expect(!CodeAppairage.chargeValide("MT:"))
        #expect(!CodeAppairage.chargeValide("mt:Y.K9042C00KA0648G00"))
        #expect(!CodeAppairage.chargeValide("MT:y.k9042"), "base38 : majuscules seulement")
        #expect(!CodeAppairage.chargeValide("https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3AY"))
        #expect(!CodeAppairage.chargeValide("MT:" + String(repeating: "A", count: 62)))
    }
}
