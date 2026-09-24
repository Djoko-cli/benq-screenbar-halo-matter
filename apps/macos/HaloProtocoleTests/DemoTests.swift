import Foundation
import Testing
@testable import HaloProtocole

/// Le fichier rejoue par le mode demo (HaloCompagnon/Ressources/demo-halo.jsonl)
/// ne contient que des lignes que l'app sait lire.
@Suite("Fichier de demonstration")
struct DemoTests {
    static let chemin = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .appendingPathComponent("HaloCompagnon/Ressources/demo-halo.jsonl")

    @Test func toutesLesLignesMachineSontValides() throws {
        let texte = try String(contentsOf: Self.chemin, encoding: .utf8)
        var r = RecepteurLignes()
        var machines = 0
        var autres = 0
        var dernierMs: Int = 0
        var types = Set<String>()
        for ligne in texte.split(separator: "\n") {
            let objet = try #require(try JSONSerialization.jsonObject(with: Data(ligne.utf8)) as? [String: Any])
            let ms = try #require(objet["ms"] as? Int, "chaque ligne porte ms")
            #expect(ms >= dernierMs, "chronologie croissante")
            dernierMs = ms
            guard objet["v"] != nil else {
                #expect(objet["texte"] != nil || objet["demo"] != nil, "ligne inconnue : \(ligne)")
                autres += 1
                continue
            }
            #expect(ligne.utf8.allSatisfy { $0 >= 0x20 && $0 <= 0x7E }, "ASCII imprimable")
            let e = r.alimenter([Octets.rs] + Array(ligne.utf8) + [Octets.lf])
            guard case .machine(let l) = e.first, e.count == 1 else {
                Issue.record("ligne rejetee : \(e) \(ligne.prefix(80))")
                continue
            }
            #expect(l.message != .inconnu)
            types.insert(l.enveloppe.t)
            machines += 1
        }
        #expect(machines > 300)
        #expect(autres > 5)
        #expect(r.compteurs.lignesAbimees == 0)
        // La chronologie montre tous les evenements promis par le mode demo.
        for t in ["rx", "tx", "livraison", "relance", "module", "intent", "abonnement", "thread", "led"] {
            #expect(types.contains(t), "type absent de la demo : \(t)")
        }
    }

    @Test func courbesDeLaDemo() throws {
        // Les compteurs de la demo donnent des taux de perte et des relances coherents.
        let texte = try String(contentsOf: Self.chemin, encoding: .utf8)
        var r = RecepteurLignes()
        var pilote: [Echantillon] = []
        var radio: [Echantillon] = []
        for ligne in texte.split(separator: "\n") where ligne.hasPrefix(#"{"v":"#) {
            for case .machine(let l) in r.alimenter([Octets.rs] + Array(ligne.utf8) + [Octets.lf]) {
                let date = Date(timeIntervalSinceReferenceDate: Double(l.enveloppe.ms ?? 0) / 1000)
                if case .compteursPilote(let c) = l.message { pilote.append(.pilote(c, date: date, boot: "B")) }
                if case .compteursRadio(let c) = l.message { radio.append(.radio(c, date: date, boot: "B")) }
            }
        }
        let d = Courbes.differences(pilote, fenetre: 10)
        #expect(Courbes.segmenter(pilote).count == 1, "aucune rupture dans la demo")
        let pertes = d.compactMap(Courbes.tauxPerte)
        #expect(pertes.contains { $0 >= 0.99 }, "la commande en echec donne 100 % de pertes")
        #expect(pertes.contains { $0 == 0 })
        #expect(Courbes.cumul(radio, .relances).last?.valeur == 4)
    }
}
