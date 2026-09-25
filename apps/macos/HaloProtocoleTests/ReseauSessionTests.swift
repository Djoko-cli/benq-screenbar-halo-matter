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

    /// Sans `hello`, a distance : la carte n'a ouvert qu'une session H1
    /// provisoire, oubliee 30 s apres le SALUT (10.4). La relance de 30 s est
    /// donc une nouvelle poignee de main (`.rouvrir`), jamais un `json 1`
    /// scelle pour une session que la carte ne connait plus.
    @Test func sansHelloADistanceRouvreApres30s() {
        var m = MoteurSession()
        #expect(M.envois(m.ouvert(maintenant: 0, genre: .udp)) == ["id=1 json 1\n"])
        for t in [2.0, 4.0, 6.0] {
            #expect(M.envois(m.tic(maintenant: t)) == ["id=1 json 1\n"], "renvoi du meme id a \(t) s")
        }
        let e8 = m.tic(maintenant: 8.0)
        #expect(m.phase == .sansReponse)
        #expect(e8 == [.note(.aucuneReponse)])
        let e35 = m.tic(maintenant: 35.9)
        #expect(e35.isEmpty)
        let e36 = m.tic(maintenant: 36.0)
        #expect(e36 == [.rouvrir(.reseauSansHello)], "nouvelle poignee de main, aucun json 1")
        #expect(M.envois(e36).isEmpty)
        #expect(!MoteurSession.Note.reseauSansHello.grave, "note de console, pas de bandeau de plus")
        #expect(m.statistiques.reouvertures == 1)
        // Tics de 250 ms avant que la fermeture n'arrive : pas de second .rouvrir.
        let e36b = m.tic(maintenant: 36.25)
        #expect(e36b.isEmpty)
        // Transport rouvert (nouvelle resolution, nouvelle poignee de main) : json 1 d'un id neuf.
        m.ferme(maintenant: 36.5)
        #expect(M.envois(m.ouvert(maintenant: 37, genre: .udp)) == ["id=2 json 1\n"])
        #expect(m.phase == .attenteHello(essai: 1))
    }

    @Test func sansHelloParUSBEtDemoInchange() {
        for genre in [GenreTransport.usb, .demo] {
            var u = MoteurSession()
            _ = u.ouvert(maintenant: 0, genre: genre)
            for t in [2.0, 4.0, 6.0, 8.0] { _ = u.tic(maintenant: t) }
            #expect(u.phase == .sansReponse)
            let e = u.tic(maintenant: 36.0)
            #expect(e == [.envoyer(LigneCommande.effacement), .envoyer(Data("id=5 json 1\n".utf8))],
                    "\(genre) : Ctrl-U et json 1 d'un id neuf, sur le meme port")
            #expect(u.statistiques.reouvertures == 0)
        }
    }

    @Test func reessayerADistanceRouvre() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        for t in [2.0, 4.0, 6.0, 8.0] { _ = m.tic(maintenant: t) }
        #expect(m.phase == .sansReponse)
        let e = m.reessayer(maintenant: 10)
        #expect(e == [.rouvrir(.reseauSansHello)])
        // La relance de 30 s repart de cet essai : rien de plus avant 40 s.
        let e36 = m.tic(maintenant: 36)
        #expect(e36.isEmpty)
        var u = MoteurSession()
        _ = u.ouvert(maintenant: 0)
        for t in [2.0, 4.0, 6.0, 8.0] { _ = u.tic(maintenant: t) }
        let r = u.reessayer(maintenant: 10)
        #expect(M.envois(r) == ["\u{15}\n", "id=5 json 1\n"], "USB : inchange")
        #expect(u.phase == .attenteHello(essai: 1))
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
