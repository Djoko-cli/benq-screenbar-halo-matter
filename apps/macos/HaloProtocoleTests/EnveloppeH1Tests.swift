import CryptoKit
import Foundation
import Testing
@testable import HaloProtocole

/// Vecteurs de docs/PROTOCOLE-JSON.md 10.4 (les memes que tools/host_tests/test_h1.cpp).
enum VecteursH1 {
    static let psk = Data((0..<32).map { UInt8($0) })
    static let na = Data((0xA0...0xAF).map { UInt8($0) })
    static let nc = "505152535455565758595A5B5C5D5E5F"
    static let sid = "1234ABCD"
    static let ks = "20D6D83D97ED44F2BBF8CE56389BD475CBE2B625CE6CE24768B6B4C1C625012F"
    static let salut = "H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D"
    static let defi = "H1 DEFI 1234ABCD 505152535455565758595A5B5C5D5E5F BFF13F71B42243E6017D2807F8E6171F"
    static let a1 = "H1 1234ABCD 1 FD97A0C9E604524B49C763452D0310CE id=1 json 1"
    static let chargeC1 = #"{"v":1,"t":"hb","n":7,"ms":1234}"#
    static let c1 = "H1 1234ABCD 1 347A2E6A129BC822ECFF39BEC910451C " + chargeC1

    static func session() -> SessionH1 {
        SessionH1(sid: sid, ks: H1.cleSession(cle: psk, na: na, nc: nc, sid: sid))
    }
}

func octets(_ s: String) -> Data { Data(s.utf8) }

@Suite("Enveloppe H1 (10.4)")
struct EnveloppeH1Tests {
    @Test func kidEtSalut() {
        #expect(H1.kid(cle: VecteursH1.psk) == "630DCD29")
        #expect(H1.salut(cle: VecteursH1.psk, na: VecteursH1.na) == octets(VecteursH1.salut))
    }

    @Test func hexa() {
        #expect(H1.hexa([0x00, 0xAB, 0x0F]) == "00AB0F")
        #expect(H1.octets(hexa: "00AB0F") == Data([0x00, 0xAB, 0x0F]))
        #expect(H1.octets(hexa: "00ab0f") == nil, "majuscules seulement")
        #expect(H1.octets(hexa: "ABC") == nil, "longueur impaire")
        #expect(H1.aleatoire(16).count == 16)
        #expect(H1.aleatoire(32) != H1.aleatoire(32))
    }

    @Test func defiEtCleDeSession() throws {
        let r = try #require(H1.verifierDefi(octets(VecteursH1.defi), cle: VecteursH1.psk, na: VecteursH1.na))
        #expect(r.sid == VecteursH1.sid)
        #expect(r.nc == VecteursH1.nc)
        let ks = H1.cleSession(cle: VecteursH1.psk, na: VecteursH1.na, nc: r.nc, sid: r.sid)
        #expect(ks.withUnsafeBytes { H1.hexa($0) } == VecteursH1.ks)
    }

    @Test func defiRefuse() {
        let autreNa = Data(repeating: 0x11, count: 16)
        #expect(H1.verifierDefi(octets(VecteursH1.defi), cle: VecteursH1.psk, na: autreNa) == nil, "autre na")
        let minuscules = VecteursH1.defi.replacingOccurrences(of: "BFF13F71B42243E6017D2807F8E6171F",
                                                              with: "bff13f71b42243e6017d2807f8e6171f")
        #expect(H1.verifierDefi(octets(minuscules), cle: VecteursH1.psk, na: VecteursH1.na) == nil, "minuscules")
        var faux = Array(VecteursH1.defi.utf8)
        faux[faux.count - 1] = UInt8(ascii: "E")
        #expect(H1.verifierDefi(Data(faux), cle: VecteursH1.psk, na: VecteursH1.na) == nil, "MAC faux")
        #expect(H1.verifierDefi(octets(VecteursH1.salut), cle: VecteursH1.psk, na: VecteursH1.na) == nil, "pas un DEFI")
    }

    @Test func scellerCommeLeFirmware() {
        var s = VecteursH1.session()
        #expect(s.sceller(octets("id=1 json 1")) == octets(VecteursH1.a1))
        #expect(s.ctrEmis == 1)
    }

    @Test func ouvrirUnMessageDeLaCarte() {
        var s = VecteursH1.session()
        #expect(s.ouvrir(octets(VecteursH1.c1)) == octets(VecteursH1.chargeC1))
        #expect(s.ouvrir(octets(VecteursH1.c1)) == nil, "rejeu")
        #expect(s.ecartes == 1)
    }

    @Test func formeCanonique() {
        var s = VecteursH1.session()
        let mac = "347A2E6A129BC822ECFF39BEC910451C"
        let c = VecteursH1.chargeC1
        let faux = [
            "H1 1234ABCD 01 \(mac) \(c)",              // zero de tete
            "H1 1234ABCD 0 \(mac) \(c)",               // ctr nul
            "H1 1234abcd 1 \(mac) \(c)",               // sid en minuscules
            "H1 1234ABCD 1 \(mac.lowercased()) \(c)",  // MAC en minuscules
            "H1 9999ABCD 1 \(mac) \(c)",               // autre session
            "H1 1234ABCD 1 \(mac)",                    // charge absente
        ]
        for f in faux { #expect(s.ouvrir(octets(f)) == nil, "\(f)") }
        #expect(s.ecartes == faux.count)
        #expect(s.ouvrir(octets(VecteursH1.c1)) != nil, "le vrai passe ensuite")
    }

    @Test func messageDeLAppNeSOuvrePasCommeMessageDeLaCarte() {
        var s = VecteursH1.session()
        #expect(s.ouvrir(octets(VecteursH1.a1)) == nil, "sens A presente comme C")
    }

    @Test func fenetre() {
        // Comparaison explicite (pas d'appel nu) : le macro #expect decompose
        // un appel a une methode mutante en `{ $0.accepter($1) }`, ou `$0` est
        // immuable, ce qui ne compile pas ; comparer le Bool rendu evite ca.
        var f = FenetreAntiRejeu()
        #expect(f.accepter(0) == false)
        #expect(f.accepter(1) == true)
        #expect(f.accepter(3) == true)
        #expect(f.accepter(2) == true, "desordre admis")
        #expect(f.accepter(2) == false, "rejeu")
        #expect(f.accepter(40) == true, "saut")
        #expect(f.accepter(8) == false, "trop ancien : 40 - 8 >= 32")
        #expect(f.accepter(9) == true, "dans la fenetre : 40 - 9 = 31")
        #expect(f.haut == 40)
    }

    @Test func macFauxNePoussePasLaFenetre() {
        var s = VecteursH1.session()
        #expect(s.ouvrir(octets("H1 1234ABCD 1000 00000000000000000000000000000000 {}")) == nil)
        #expect(s.ouvrir(octets(VecteursH1.c1)) != nil, "ctr 1 encore admis : la fenetre n'a pas bouge")
    }
}
