import Foundation
import Testing
@testable import HaloProtocole

private let t0 = Date(timeIntervalSinceReferenceDate: 1_000_000)  // multiple de 10

private func ech(_ s: Double, boot: String = "A", raz: Int = 0, _ v: [Grandeur: Int]) -> Echantillon {
    Echantillon(date: t0.addingTimeInterval(s), boot: boot, raz: raz, valeurs: v)
}

@Suite("Courbes (section 8)")
struct CourbesTests {
    @Test func segmentsSurDifferenceNegativeRazEtBoot() {
        let e = [
            ech(0, [.paquets: 10]),
            ech(1, [.paquets: 12]),
            ech(2, [.paquets: 3]),              // difference negative
            ech(3, [.paquets: 5]),
            ech(4, raz: 1, [.paquets: 6]),      // raz change
            ech(5, boot: "B", raz: 1, [.paquets: 7]),  // redemarrage
        ]
        let s = Courbes.segmenter(e)
        #expect(s.map(\.count) == [2, 2, 1, 1])
    }

    @Test func differencesParFenetre() {
        // Un echantillon par seconde pendant 30 s, 3 paquets par seconde.
        let e = (0...30).map { ech(Double($0), [.paquets: 3 * $0, .accuses: 3 * $0 - ($0 / 5)]) }
        let d = Courbes.differences(e, fenetre: 10)
        // Fenetres [0,10) [10,20) [20,30) [30,40) : la premiere part du premier echantillon.
        #expect(d.count == 4)
        #expect(d[0].debut == t0 && d[0].fin == t0.addingTimeInterval(9))
        #expect(d[0][.paquets] == 27)
        #expect(d[1].debut == t0.addingTimeInterval(9) && d[1].fin == t0.addingTimeInterval(19))
        #expect(d[1][.paquets] == 30)
        #expect(d[3][.paquets] == 3)
        #expect(d.allSatisfy { $0.segment == 0 })
        let total = d.compactMap { $0[.paquets] }.reduce(0, +)
        #expect(total == 90, "les fenetres se recouvrent exactement, sans trou ni double compte")
    }

    @Test func pasDeValeurAberranteAuRedemarrage() {
        let e = [ech(0, [.crcFaux: 100]), ech(5, [.crcFaux: 110]), ech(12, boot: "B", [.crcFaux: 2]), ech(15, boot: "B", [.crcFaux: 4])]
        let d = Courbes.differences(e, fenetre: 10)
        #expect(d.map { $0[.crcFaux] } == [10, 2])
        #expect(d.map(\.segment) == [0, 1])
        #expect(d.allSatisfy { ($0[.crcFaux] ?? 0) >= 0 })
    }

    @Test func unTrouDEchantillonsOuvreUnSegment() {
        // L'app ne lit plus pendant 590 s (veille, app suspendue), meme boot : sans rupture, une
        // seule difference de 5900 rearmements serait tracee "par fenetre" contre le seuil de 1000.
        let e = [ech(0, [.rearmHorsRx: 0]), ech(1, [.rearmHorsRx: 10]), ech(591, [.rearmHorsRx: 5910]),
                 ech(592, [.rearmHorsRx: 5920])]
        #expect(Courbes.segmenter(e).map(\.count) == [2, 2])
        let d = Courbes.differences(e, fenetre: 10)
        #expect(d.map { $0[.rearmHorsRx] } == [10, 10])
        #expect(d.map(\.segment) == [0, 1])
        // Periode compteurs lente (json compteurs 60000) : l'ecart tolere suit (3 periodes).
        #expect(Courbes.ecartMax(compteursMs: 1000) == 30)
        #expect(Courbes.ecartMax(compteursMs: 60000) == 180)
        let lent = [ech(0, [.paquets: 0]), ech(60, [.paquets: 6]), ech(120, [.paquets: 12])]
        #expect(Courbes.segmenter(lent, ecartMax: Courbes.ecartMax(compteursMs: 60000)).count == 1)
    }

    @Test func tauxDePerte() {
        let d = Difference(debut: t0, fin: t0.addingTimeInterval(60), segment: 0,
                           deltas: [.paquets: 20, .accuses: 14, .maxRt: 4, .delais: 1, .fifo: 1])
        #expect(Courbes.tauxPerte(d) == 0.3)
        #expect(abs((Courbes.tauxSansAccuse(d) ?? 0) - 0.3) < 1e-12)
        let vide = Difference(debut: t0, fin: t0.addingTimeInterval(10), segment: 0, deltas: [.paquets: 0])
        #expect(Courbes.tauxPerte(vide) == nil, "pas de division par zero")
    }

    @Test func crcFauxParMinuteEtPart() {
        let d = Difference(debut: t0, fin: t0.addingTimeInterval(10), segment: 0,
                           deltas: [.trames: 50, .crcFaux: 5])
        #expect(Courbes.parMinute(d, .crcFaux) == 30)
        #expect(Courbes.partCrcFaux(d) == 0.1)
        // Le firmware declenche a 100 trames dont 90 % de CRC faux sur 10 s (halo1_watch.cpp) :
        // au moins 90 CRC faux par fenetre, 540 par minute.
        #expect(Courbes.seuilDelugeParMinute(trames: 100, pct: 90, fenetreMs: 10000) == 540)
        #expect(Courbes.seuilSurditeParMinute(horsRx: 1000) == 6000)
    }

    @Test func cumulDesRelancesParSegment() {
        let e = [ech(0, [.relances: 0]), ech(1, [.relances: 1]), ech(2, raz: 1, [.relances: 0])]
        let c = Courbes.cumul(e, .relances)
        #expect(c.map(\.valeur) == [0, 1, 0])
        #expect(c.map(\.segment) == [0, 0, 1])
    }

    @Test func echantillonsDepuisLesCompteurs() throws {
        let l = try ExemplesSpec.decoder()
        guard case .compteursPilote(let p) = l[6].message, case .compteursRadio(let r) = l[7].message else {
            Issue.record("compteurs"); return
        }
        let ep = Echantillon.pilote(p, date: t0, boot: "3FA2C901")
        #expect(ep.valeurs[.paquets] == 21)
        #expect(ep.valeurs[.maxRt] == 2)
        #expect(ep.valeurs[.crcFaux] == 0)
        #expect(ep.raz == 0)
        let er = Echantillon.radio(r, date: t0, boot: "3FA2C901")
        #expect(er.valeurs[.rearmHorsRx] == 3)
        #expect(er.valeurs[.gardeRefus] == 0)
        #expect(er.valeurs[.relances] == 0)
    }
}

@Suite("Correspondances niveau / brut, mireds / temp")
struct CorrespondanceTests {
    let g2 = CorrespondanceLuminosite(gammaC: 200)

    @Test func valeursDuBanc() {
        // Section 12 : gamma 2, niveau 180 = lum A5, niveau 200 = lum BA, 127 = 0x78.
        #expect(g2.brut(niveau: 180) == 0xA5)
        #expect(g2.brut(niveau: 200) == 0xBA)
        #expect(g2.brut(niveau: 127) == 0x78)
        #expect(g2.niveau(brut: 0xA5) == 180)
        #expect(g2.niveau(brut: 0xBA) == 200)
        #expect(g2.niveau(brut: 0x78) == 127)
        #expect(CorrespondanceLuminosite.mired(temp: 53) == 268)
        #expect(CorrespondanceLuminosite.temp(mired: 268) == 53)
    }

    @Test func bornesEtPlancher() {
        #expect(g2.brut(niveau: 0) == 0x4C)
        #expect(g2.brut(niveau: 4) == 0x4C)
        #expect(g2.brut(niveau: 254) == 0xFE)
        #expect(g2.niveau(brut: 0x4C) == 4, "jamais sous le plancher")
        #expect(g2.niveau(brut: 0xFE) == 254)
        #expect(CorrespondanceLuminosite.mired(temp: 0) == 153)
        #expect(CorrespondanceLuminosite.mired(temp: 100) == 370)
        #expect(CorrespondanceLuminosite.temp(mired: 100) == 0)
        #expect(CorrespondanceLuminosite.temp(mired: 500) == 100)
    }

    @Test func monotonieEtAllerRetour() {
        for l in 1...254 { #expect(g2.brut(niveau: l) >= g2.brut(niveau: l - 1)) }
        for b in 0x4C...0xFE {
            let l = g2.niveau(brut: b)
            #expect(g2.brut(niveau: l) >= b)
        }
        for t in 0...100 {
            #expect(CorrespondanceLuminosite.temp(mired: CorrespondanceLuminosite.mired(temp: t)) == t)
        }
    }

    @Test func gammaLineaire() {
        let g1 = CorrespondanceLuminosite(gammaC: 100)
        #expect(g1.brut(niveau: 254) == 0xFE)
        #expect(g1.brut(niveau: 128) == 0x4C + (127 * 178 + 126) / 253)
    }
}

@Suite("Regles des commandes (2.6, 6.4, 10.5)")
struct PolitiqueTests {
    @Test func longueurEtCaracteres() throws {
        #expect(try LigneCommande.valider("lampe niveau 200", id: 17).get() == "id=17 lampe niveau 200")
        let longue = String(repeating: "x", count: 122)  // "id=17 " + 122 = 128
        if case .failure(.tropLongue) = LigneCommande.valider(longue, id: 17) {} else { Issue.record("trop longue") }
        #expect((try? LigneCommande.valider(String(repeating: "x", count: 121), id: 17).get())?.utf8.count == 127)
        #expect(LigneCommande.valider("lampe é", id: 1) == .failure(.caractereInterdit))
        #expect(LigneCommande.valider("a\u{1E}b", id: 1) == .failure(.json))
        #expect(LigneCommande.valider("{\"v\":1}", id: 1) == .failure(.json))
        #expect(LigneCommande.valider("id=3 reboot", id: 1) == .failure(.prefixeId))
        #expect(LigneCommande.valider("   ", id: 1) == .failure(.vide))
    }

    @Test func confirmations() {
        for c in ["reboot", "decommission", "erase", "wifi x y", "addr 1", "chan 5", "xo 3", "debit 1", "amble 2",
                  "aw 5", "holtek", "regcfg", "lampe oublie", "lampe adresse 4FF0FD63", "lampe stats raz",
                  "matter med 1", "matter maxint 20", "matter reprise auto 1", "json cle nouvelle 00",
                  "json cle efface"] {
            if case .confirmation = PolitiqueCommandes.verdictConsole(c, transport: .usb) {} else {
                Issue.record("confirmation attendue pour \(c)")
            }
        }
        for c in ["lampe stats", "lampe adresse", "lampe niveau 200", "help", "json etat", "matter", "json cle"] {
            #expect(PolitiqueCommandes.verdictConsole(c, transport: .usb) == .autorisee, "\(c)")
        }
        if case .interdite = PolitiqueCommandes.verdictConsole("json 0", transport: .usb) {} else {
            Issue.record("json 0 passe par Liberer le port")
        }
        #expect(PolitiqueCommandes.attendReenumeration("reboot"))
    }

    @Test func listeBlancheADistance() {
        let ok = ["json 1", "json 1 bail 60", "json 0", "json etat", "json hello", "json ping", "json periode 2000",
                  "json compteurs 0", "json compteurs 5000", "json reseau 10000", "json trames 1", "json log 0",
                  "lampe on", "lampe off", "lampe avant on", "lampe arriere off", "lampe mode deux",
                  "lampe niveau 200", "lampe lum A5", "lampe temp 50", "lampe mired 300", "lampe auto",
                  "lampe sync", "led test", "led stop"]
        for c in ok { #expect(PolitiqueCommandes.autoriseeADistance(c), "\(c)") }
        let non = ["json 1 bail 0", "json 1 bail 600", "json periode 1000", "json compteurs 1000", "json reseau 5000",
                   "json cle", "reboot", "lampe brut C5A5", "lampe stats", "matter", "txack", "chiplog", "calib"]
        for c in non { #expect(!PolitiqueCommandes.autoriseeADistance(c), "\(c)") }
        if case .interdite = PolitiqueCommandes.verdictConsole("reboot", transport: .udp) {} else {
            Issue.record("reboot interdit a distance")
        }
    }

    @Test func masquageDeLaCle() {
        let cle = String(repeating: "AB", count: 32)
        #expect(!PolitiqueCommandes.masquerCle("id=4 json cle nouvelle \(cle)").contains(cle))
        let json = #"{"v":1,"t":"reponse","n":1,"ms":1,"id":4,"etape":"fin","ok":true,"code":"ok","cle":"\#(cle)"}"#
        let m = PolitiqueCommandes.masquerCle(json)
        #expect(!m.contains(cle))
        #expect(m.contains("\"cle\":\"••••••••\""))
        #expect(PolitiqueCommandes.masquerCle(m) == m, "idempotent")
        // La CLI lit les mots sans casse ni espaces multiples : le masque aussi.
        let tape = PolitiqueCommandes.masquerCle("JSON  Cle   NOUVELLE \(cle.lowercased())")
        #expect(!tape.lowercased().contains(cle.lowercased()))
        // Cle imprimee en texte (commande sans id), et deux champs cle dans une ligne.
        #expect(!PolitiqueCommandes.masquerCle("  cle : \(cle)").contains(cle))
        let deux = PolitiqueCommandes.masquerCle(#"{"cle":"\#(cle)","autre":{"cle":"\#(cle)"}}"#)
        #expect(!deux.contains(cle))
        #expect(PolitiqueCommandes.masquerCle("lampe niveau 200") == "lampe niveau 200")
    }
}
