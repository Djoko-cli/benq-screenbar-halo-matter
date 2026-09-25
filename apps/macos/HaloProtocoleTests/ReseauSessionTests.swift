import Foundation
import Testing
@testable import HaloProtocole

@Suite("Regles du reseau (10.2)")
struct ReseauSessionTests {
    typealias M = MoteurSessionTests

    @Test func renvoisDuMemeIdPuisSansReponse() throws {
        var c = Correlateur()
        c.politique = .reseau
        let a = c.soumettre("lampe auto", origine: .interface, maintenant: 0)
        let p = c.prochainEnvoi(maintenant: 0)
        let e = try #require(p)
        #expect(texte(e.octets) == "id=1 lampe auto\n")
        #expect(c.renvoisDus(maintenant: 1.9).isEmpty)
        #expect(c.renvoisDus(maintenant: 2.0).map(texte) == ["id=1 lampe auto\n"])
        #expect(c.renvoisDus(maintenant: 3.9).isEmpty)
        #expect(c.renvoisDus(maintenant: 4.0).map(texte) == ["id=1 lampe auto\n"])
        #expect(c.renvoisDus(maintenant: 5.9).isEmpty, "deux renvois au plus")
        #expect(c.verifierDelais(maintenant: 5.9).isEmpty)
        #expect(c.verifierDelais(maintenant: 6.0).map(\.id) == [a])
        #expect(c.suivi(a)?.etat == .sansReponse)
        #expect(c.suivi(a)?.renvois == 2)
    }

    @Test func reponseArreteLesRenvoisEtDoublonIgnore() {
        var c = Correlateur()
        c.politique = .reseau
        _ = c.soumettre("json ping", origine: .session, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.renvoisDus(maintenant: 2)
        #expect(c.recevoir(reponse(1), maintenant: 2.3) != .inattendue)
        #expect(c.renvoisDus(maintenant: 4).isEmpty)
        #expect(c.recevoir(reponse(1), maintenant: 2.4) == .inattendue, "reponse rejouee depuis le cache : ignoree")
    }

    @Test func usbSansRenvoi() {
        var c = Correlateur()
        let a = c.soumettre("lampe on", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.renvoisDus(maintenant: 2.5).isEmpty)
        #expect(c.verifierDelais(maintenant: 3).map(\.id) == [a])
    }

    @Test func ouvertureReseauSansCtrlU() {
        var m = MoteurSession()
        #expect(M.envois(m.ouvert(maintenant: 0, genre: .udp)) == ["id=1 json 1\n"])
        #expect(m.correlateur.politique == .reseau)
        var u = MoteurSession()
        #expect(M.envois(u.ouvert(maintenant: 0)) == ["\u{15}\n", "id=1 json 1\n"])
        #expect(u.correlateur.politique == .usb)
    }

    @Test func json1RenvoyeAvecLeMemeIdADistance() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        #expect(M.envois(m.tic(maintenant: 2.0)) == ["id=1 json 1\n"], "la carte ne refait pas l'instantane")
        var u = MoteurSession()
        _ = u.ouvert(maintenant: 0)
        #expect(M.envois(u.tic(maintenant: 2.0)) == ["id=2 json 1\n"], "USB : inchange")
    }

    @Test func finDuJson1AttendPlusLongtempsADistance() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        _ = m.recu(M.element(M.hello), maintenant: 0.3)
        #expect(!m.tic(maintenant: 3.5).contains(.note(.reponseJson1Perdue)), "instantane de ~6 Ko sur Thread")
        #expect(m.instantaneEnCours)
        _ = m.recu(M.element(M.hello), maintenant: 5.0)  // la carte parle : pas de silence
        #expect(m.tic(maintenant: 8.0).contains(.note(.reponseJson1Perdue)))
    }

    @Test func listeBlancheRefuseLaCleEtLeRedemarrage() {
        for c in ["json cle", "json cle nouvelle " + String(repeating: "AB", count: 32), "json cle efface", "reboot"] {
            #expect(!PolitiqueCommandes.autoriseeADistance(c), "\(c)")
            if case .interdite = PolitiqueCommandes.verdictConsole(c, transport: .udp) {} else {
                Issue.record("\(c) doit etre refusee a distance")
            }
        }
    }

    @Test func dejaTraiteDemandeUnEtat() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        _ = m.recu(M.element(M.hello), maintenant: 0.1)
        _ = m.recu(M.finJson1(), maintenant: 0.2)
        let (_, e1) = m.soumettre("lampe auto", origine: .interface, maintenant: 0.3)
        #expect(M.envois(e1) == ["id=2 lampe auto\n"])
        let deja = M.element(#"{"v":1,"t":"reponse","n":12,"ms":83530,"id":2,"etape":"fin","cmd":"lampe auto","ok":false,"code":"deja_traite","duree_ms":0}"#)
        #expect(M.envois(m.recu(deja, maintenant: 0.5)) == ["id=3 json etat\n"])
    }
}
