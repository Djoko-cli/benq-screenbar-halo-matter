import Foundation
import Testing
@testable import HaloProtocole

@Suite("Cle du transport reseau (10.4)")
struct CleReseauTests {
    static func reponseCle(ok: Bool = true, code: CodeReponse = .ok, cle: String?, empreinte: String?) -> Reponse {
        Reponse(id: 7, etape: .fin, cmd: "json cle nouvelle", ok: ok, code: code, msg: ok ? nil : "tampon USB occupe",
                dureeMs: 1, suite: nil, consigne: nil, aLivrer: nil, version: nil, bailS: nil, upS: nil,
                cle: cle, empreinte: empreinte)
    }

    @Test func commande() {
        let c = CleReseau.commande(alea: Data(repeating: 0xAB, count: 32))
        #expect(c == "json cle nouvelle " + String(repeating: "AB", count: 32))
        if case .failure(let e) = LigneCommande.valider(c, id: LigneCommande.idMax) { Issue.record("\(e)") }
        #expect(CleReseau.alea().count == 32)
    }

    @Test func verifierUneBonneCle() throws {
        let r = Self.reponseCle(cle: H1.hexa(VecteursH1.psk), empreinte: "630DCD29")
        let c = try CleReseau.verifier(r).get()
        #expect(c.cle == VecteursH1.psk)
        #expect(c.empreinte == "630DCD29")
    }

    @Test func refus() {
        #expect(CleReseau.verifier(Self.reponseCle(ok: false, code: .refuse, cle: nil, empreinte: nil))
                == .failure(.refusee("tampon USB occupe")))
        #expect(CleReseau.verifier(Self.reponseCle(cle: nil, empreinte: "630DCD29")) == .failure(.cleIllisible))
        #expect(CleReseau.verifier(Self.reponseCle(cle: H1.hexa(VecteursH1.psk).lowercased(), empreinte: "630DCD29"))
                == .failure(.cleIllisible))
        #expect(CleReseau.verifier(Self.reponseCle(cle: H1.hexa(VecteursH1.psk), empreinte: "00000000"))
                == .failure(.empreinteIncoherente))
    }

    @Test func etatDeLAcces() {
        func ip(_ nom: String?, ouvert: Bool?, empreinte: String?) -> ReseauIp {
            ReseauIp(srp: .init(nom: nom), udp: .init(port: 5480, ouvert: ouvert, empreinte: empreinte))
        }
        let mac: (String) -> String? = { $0 == "56B1E064401F74EF" ? "630DCD29" : nil }
        #expect(EtatAccesReseau.depuis(ip: nil, empreinteDuMac: mac) == .inconnu)
        #expect(EtatAccesReseau.depuis(ip: ip(nil, ouvert: true, empreinte: "630DCD29"), empreinteDuMac: mac) == .inconnu)
        #expect(EtatAccesReseau.depuis(ip: ReseauIp(srp: .init(nom: "56B1E064401F74EF")), empreinteDuMac: mac) == .inconnu,
                "firmware sans transport reseau (pas de bloc udp)")
        #expect(EtatAccesReseau.depuis(ip: ip("56B1E064401F74EF", ouvert: false, empreinte: nil), empreinteDuMac: mac)
                == .sansCle(nom: "56B1E064401F74EF"))
        #expect(EtatAccesReseau.depuis(ip: ip("56B1E064401F74EF", ouvert: true, empreinte: "630DCD29"), empreinteDuMac: mac)
                == .cleConnue(nom: "56B1E064401F74EF", empreinte: "630DCD29"))
        #expect(EtatAccesReseau.depuis(ip: ip("56B1E064401F74EF", ouvert: true, empreinte: "B64D84FB"), empreinteDuMac: mac)
                == .cleInconnue(nom: "56B1E064401F74EF", empreinte: "B64D84FB"))
    }

    @Test func laCleNestJamaisRangeeDansLeSuivi() {
        var c = Correlateur()
        let a = c.soumettre(CleReseau.commande(alea: Data(repeating: 1, count: 32)), origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(Self.reponseCle(cle: H1.hexa(VecteursH1.psk), empreinte: "630DCD29").avecId(1), maintenant: 0.1)
        // La cle ne va que dans le trousseau : jamais dans un suivi de commande, meme un instant (5.2).
        #expect(c.suivi(a)?.fin?.cle == nil)
        #expect(c.suivi(a)?.fin?.empreinte == "630DCD29", "l'empreinte reste")
    }
}

extension Reponse {
    func avecId(_ n: Int) -> Reponse {
        var r = self
        r.id = n
        return r
    }
}
