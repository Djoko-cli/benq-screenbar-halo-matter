import Foundation
import Testing
@testable import HaloProtocole

/// Les lignes `<RS>{...}` de la section 12 de docs/PROTOCOLE-JSON.md, lues
/// dans le document lui-meme : la specification et les tests ne divergent pas.
enum ExemplesSpec {
    static let chemin = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // HaloProtocoleTests
        .deletingLastPathComponent()  // macos
        .deletingLastPathComponent()  // apps
        .deletingLastPathComponent()  // racine du depot
        .appendingPathComponent("docs/PROTOCOLE-JSON.md")

    static func lignes() throws -> [String] {
        let texte = try String(contentsOf: chemin, encoding: .utf8)
        return texte.split(separator: "\n", omittingEmptySubsequences: true)
            .filter { $0.hasPrefix("<RS>") }
            .map { String($0.dropFirst(4)) }
    }

    /// Decode toutes les lignes dans un meme recepteur, comme sur le fil.
    static func decoder() throws -> [LigneMachine] {
        var r = RecepteurLignes()
        var octets: [UInt8] = []
        for l in try lignes() { octets += [Octets.rs] + Array(l.utf8) + [Octets.lf] }
        return r.alimenter(octets).compactMap {
            if case .machine(let l) = $0 { return l }
            return nil
        }
    }
}

@Suite("Exemples de la specification (section 12)")
struct ExemplesSpecTests {
    @Test func toutesLesLignesSontLues() throws {
        let lignes = try ExemplesSpec.lignes()
        #expect(lignes.count == 43)
    }

    @Test(arguments: (try? ExemplesSpec.lignes()) ?? [])
    func chaqueLigneSeDecode(_ json: String) throws {
        var r = RecepteurLignes()
        let e = r.alimenter([Octets.rs] + Array(json.utf8) + [Octets.lf])
        try #require(e.count == 1, "un element attendu : \(e)")
        guard case .machine(let l) = e[0] else {
            Issue.record("ligne rejetee : \(e[0])")
            return
        }
        #expect(l.message != .inconnu, "type non gere : \(l.enveloppe.t) \(l.enveloppe.bloc ?? "")")
        #expect(l.enveloppe.v == 1)
        #expect(json.utf8.count + 2 <= 1024, "plus de 1024 octets")
        #expect(r.compteurs.lignesAbimees == 0)
    }

    @Test func connexion() throws {
        let l = try ExemplesSpec.decoder()
        guard case .helloBase(let h) = l[0].message else { Issue.record("hello base"); return }
        #expect(h.fw == "0.4.0-1a2b3c4")
        #expect(h.boot == "3FA2C901")
        #expect(h.reset == "logiciel")
        #expect(h.upS == 83)
        #expect(h.session?.periodeMs == 1000)
        #expect(h.session?.bailS == 30)
        #expect(h.session?.trames == true)
        #expect(h.limites?.cmdMax == 127)

        guard case .helloIdentite(let i) = l[1].message else { Issue.record("identite"); return }
        #expect(i.mac == "F0F5BD012345")
        #expect(i.id?.serie == "HALO1-F0F5BD012345")
        #expect(i.id?.hwTxt == "ESP32-C6 SuperMini + BM5602")
        #expect(i.caps?.contains("lampe_async") == true)

        guard case .config(let c) = l[2].message else { Issue.record("config"); return }
        #expect(c.lampe?.air == "63FDF04F")
        #expect(c.reglages?.gammaC == 200)
        #expect(c.reglages?.garde == true)
        #expect(c.seuils?.delugeTrames == 100)
        #expect(c.seuils?.sourdHorsRx == 1000)
        #expect(c.matter?.endpoints?.principal == 1)
        #expect(c.matter?.endpoints?.auto == nil)
        #expect(c.matter?.niveauPlancher == 4)

        guard case .etatLampe(let e) = l[3].message else { Issue.record("etat lampe"); return }
        #expect(e.consigne == EtatLampe(marche: true, lampes: .deux, lum: 165, niveau: 180, temp: 53, mired: 268))
        #expect(e.cru == e.consigne)
        #expect(e.aLivrer == [])
        #expect(e.confirme == [.marche, .lum, .temp])
        #expect(e.phase == .repos)
        #expect(e.repriseMs == nil)
        #expect(e.lien == .ok)
        #expect(e.accuseMs == 41210)
        #expect(e.memoire == .deux)

        guard case .etatTranches(let t) = l[4].message else { Issue.record("tranches"); return }
        #expect(t.tranches == [])

        guard case .etatSante(let s) = l[5].message else { Issue.record("sante"); return }
        #expect(s.radio?.mode == .ecoute)
        #expect(s.surveil?.symptome == nil)
        #expect(s.surveil?.horsRx10s == 0)
        #expect(s.surveil?.derniere == nil)
        #expect(s.led?.motif == .operationnel)
        #expect(s.matter?.enService == true)
        #expect(s.sys?.heapMin == 86016)

        guard case .compteursPilote(let p) = l[6].message else { Issue.record("pilote"); return }
        #expect(p.tx?.paquets == 21)
        #expect(p.tx?.maxRt == 2)
        #expect(p.rx?.accusesLampe == 36)
        #expect(p.divers?.relancesModule == 0)

        guard case .compteursRadio(let r) = l[7].message else { Issue.record("radio"); return }
        #expect(r.radio?.rearmHorsRx == 3)
        #expect(r.garde?.maxUs == 2380)
        #expect(r.relances?.total == 0)

        guard case .compteursMatter(let m) = l[8].message else { Issue.record("matter"); return }
        #expect(m.reflets == 11)

        guard case .reseauThread(let th) = l[9].message else { Issue.record("thread"); return }
        #expect(th.thread?.role == "child")
        #expect(th.thread?.parentRssi == -48)
        #expect(th.thread?.srp?.hote == "Registered")
        #expect(th.matter?.codeManuel == nil)

        guard case .reseauAbonnements(let a) = l[10].message else { Issue.record("abonnements"); return }
        #expect(a.abonnements?.actifs == 1)
        #expect(a.reprise?.enCours == .booleen(false))

        guard case .reponse(let rep) = l[11].message else { Issue.record("reponse"); return }
        #expect(rep.id == 1)
        #expect(rep.etape == .fin)
        #expect(rep.code == .ok)
        #expect(rep.bailS == 30)
    }

    @Test func commandeEtLivraison() throws {
        let l = try ExemplesSpec.decoder()
        guard case .reponse(let r) = l[12].message else { Issue.record("reponse id=2"); return }
        #expect(r.id == 2)
        #expect(r.code == .accepte)
        #expect(r.suite == .livraison)
        #expect(r.consigne?.lum == 186)
        #expect(r.aLivrer == [.lum])

        guard case .tx(let t) = l[13].message else { Issue.record("tx"); return }
        #expect(t.verdict == .ack)
        #expect(t.tranche == .lum)
        #expect(t.charge == "C5BA")
        #expect(t.us == 1719)

        guard case .livraison(let liv) = l[16].message else { Issue.record("livraison"); return }
        #expect(liv.issue == .livree)
        #expect(liv.derniere == .lum)
        #expect(liv.ids == [2])
        #expect(liv.attenteMs == 204)

        guard case .led(let led) = l[17].message else { Issue.record("led"); return }
        #expect(led.motif == .livree)
        #expect(led.avant == .operationnel)
    }

    @Test func tramesDeLaTelecommande() throws {
        let rx = try ExemplesSpec.decoder().compactMap { l -> TrameRx? in
            if case .rx(let r) = l.message { return r }
            return nil
        }
        #expect(rx.map(\.type) == [.service, .lum, .accuseLampe, .temp, .a, .crcFaux])
        #expect(rx[1].sens?.lum == 165)
        #expect(rx[1].sens?.lampes == .deux)
        #expect(rx[3].sens?.temp == 53)
        #expect(rx[4].sens?.numero == 1)
        #expect(rx[4].sens?.copie == false)
        #expect(rx[5].crcOk == false)
        #expect(rx[5].sautes == 0)
        #expect(rx[2].charge == "")
        #expect(Interpretation.rx(rx[1]).contains("niveau 180"))
    }

    @Test func echecEtRelance() throws {
        let l = try ExemplesSpec.decoder()
        let liv = l.compactMap { if case .livraison(let v) = $0.message { return v } else { return nil } }
        #expect(liv[1].issue == .abandon)
        #expect(liv[1].cause == .injoignable)
        #expect(liv[1].derniere == nil)
        #expect(liv[1].ids == [7])

        let rel = l.compactMap { if case .relance(let v) = $0.message { return v } else { return nil } }
        #expect(rel.first?.cause == .sourde)
        #expect(rel.first?.detail?.horsRx == 1204)
        #expect(rel.first?.detail?.ms == 2870)
        #expect(rel.first?.dureeMs == 312)

        let mod = l.compactMap { if case .module(let v) = $0.message { return v } else { return nil } }
        #expect(mod.first?.etat == .panne)
        #expect(mod.first?.symptome == .delais)
        #expect(mod.first?.essaiS == 600)
    }

    @Test func matterEtFinDeSession() throws {
        let l = try ExemplesSpec.decoder()
        let intents = l.compactMap { if case .intent(let v) = $0.message { return v } else { return nil } }
        #expect(intents.first?.recu?.niveau == 127)
        #expect(intents.first?.champs == [.marche, .lum])
        #expect(intents.first?.consigne?.lum == 120)

        let roles = l.compactMap { if case .thread(let v) = $0.message { return v } else { return nil } }
        #expect(roles.map(\.vers) == ["detached", "child"])
        #expect(roles.first?.aMs == 300402)

        let ab = l.compactMap { if case .abonnement(let v) = $0.message { return v } else { return nil } }
        #expect(ab.map(\.quoi) == [.termine, .demande, .etabli])
        #expect(ab[1].abonne == "0x000000000001B669")
        #expect(ab[1].appliqueS == 20)
        #expect(ab[2].totaux?.etablis == 2)

        let reps = l.compactMap { if case .reponse(let v) = $0.message { return v } else { return nil } }
        #expect(reps.contains { $0.etape == .debut && $0.code == .enCours && $0.cmd == "lampe stats" })
        #expect(reps.contains { $0.code == .refuse && $0.msg == "lampe eteinte : A n'est pas emis" })
        #expect(reps.contains { $0.code == .usage && $0.id == 10 })

        let logs = l.compactMap { if case .log(let v) = $0.message { return v } else { return nil } }
        #expect(logs.first?.src == "lampe")
        let fin = l.compactMap { if case .fin(let v) = $0.message { return v } else { return nil } }
        #expect(fin.first?.cause == .bail)
        let hb = l.compactMap { if case .battement(let v) = $0.message { return v } else { return nil } }
        #expect(hb.first?.jsonPerdus == 0)
    }

    @Test func continuiteDeNDansLesExemples() throws {
        // 12.2 : "n 74 manque ici" ; le moteur compte les trous comme des pertes.
        var r = RecepteurLignes()
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        for l in try ExemplesSpec.lignes().prefix(18) {
            for e in r.alimenter([Octets.rs] + Array(l.utf8) + [Octets.lf]) {
                _ = m.recu(e, maintenant: 1)
            }
        }
        // n 0..11, puis 71 (59 perdues), 72, 73, 75 (1 perdue), 76, 77.
        #expect(m.statistiques.pertes == 59 + 1)
    }
}
