import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

/// Attend qu'une condition devienne vraie, au plus `delai` : pas de sommeil de
/// duree fixe, qui casserait sur une machine chargee.
@MainActor
func attendre(_ delai: Duration = .seconds(15), _ condition: () -> Bool) async -> Bool {
    let fin = ContinuousClock.now + delai
    while ContinuousClock.now < fin {
        if condition() { return true }
        try? await Task.sleep(for: .milliseconds(20))
    }
    return condition()
}

/// Bout en bout sans materiel : la carte simulee du mode demo, le tramage,
/// le moteur de session et la correlation, comme dans l'app.
@Suite("Mode demo, bout en bout", .serialized)
struct DemoBoutEnBoutTests {
    /// Fait tourner la couche protocole sur un transport jusqu'a sa fermeture.
    final class Banc {
        let transport: TransportDemo
        var recepteur = RecepteurLignes()
        var moteur = MoteurSession()
        var lignes: [LigneMachine] = []
        var textes: [String] = []
        let debut = ContinuousClock.now

        init(vitesse: Double) throws {
            transport = try TransportDemo(vitesse: vitesse)
        }

        func maintenant() -> TimeInterval { (ContinuousClock.now - debut) / .seconds(1) }

        func executer(_ effets: [MoteurSession.Effet]) throws {
            for e in effets {
                if case .envoyer(let d) = e { try transport.envoyer(d) }
            }
        }

        /// Lit le flux jusqu'a sa fin, jusqu'a ce que `jusqua` soit vrai, ou au plus `delai`
        /// (garde-fou) ; `pendant` est appele apres chaque paquet d'octets.
        func lire(delai: Duration, jusqua: ((Banc) -> Bool)? = nil,
                  pendant: (Banc) throws -> Void = { _ in }) async throws {
            let flux = try await transport.ouvrir()
            recepteur.resynchroniser()
            try executer(moteur.ouvert(maintenant: maintenant()))
            let t = transport
            let garde = Task {
                try? await Task.sleep(for: delai)
                t.fermer()
            }
            defer { garde.cancel() }
            for await ev in flux {
                guard case .donnees(let d) = ev else { continue }
                for element in recepteur.alimenter(d) {
                    if case .machine(let l) = element { lignes.append(l) }
                    if case .texte(let tx) = element { textes.append(tx.texte) }
                    try executer(moteur.recu(element, maintenant: maintenant()))
                }
                try executer(moteur.tic(maintenant: maintenant()))
                try pendant(self)
                if let jusqua, jusqua(self) { t.fermer() }
            }
        }

        func types() -> Set<String> { Set(lignes.map(\.enveloppe.t)) }
    }

    @Test func connexionPuisCommandeLivree() async throws {
        let banc = try Banc(vitesse: 1)
        var envoyee: UUID?
        try await banc.lire(delai: .seconds(20), jusqua: { b in
            envoyee.flatMap { b.moteur.correlateur.suivi($0) }?.etat.estFinal == true
        }) { b in
            if b.moteur.phase == .connecte, envoyee == nil {
                let (id, effets) = b.moteur.soumettre("lampe niveau 200", origine: .interface, maintenant: b.maintenant())
                envoyee = id
                try b.executer(effets)
            }
        }
        // Sequence de connexion (3.3) : hello, config, instantane complet, reponse au json 1.
        #expect(banc.lignes.first?.cle == "hello.base")
        for cle in ["hello.identite", "config", "etat.lampe", "etat.tranches", "etat.sante", "compteurs.pilote",
                    "compteurs.radio", "compteurs.matter", "reseau.thread", "reseau.abonnements"] {
            #expect(banc.lignes.contains { $0.cle == cle }, "\(cle) absent")
        }
        #expect(banc.moteur.statistiques.pertes == 0)
        #expect(banc.recepteur.compteurs.lignesAbimees == 0)
        // La commande : reponse "accepte", trois paquets, livraison qui porte son id (6.2, 12.2).
        let id = try #require(envoyee)
        let suivi = try #require(banc.moteur.correlateur.suivi(id))
        #expect(suivi.fin?.code == .accepte)
        #expect(suivi.fin?.consigne?.lum == 0xBA)
        #expect(suivi.etat == .livree)
        #expect(suivi.livraison?.ids == [suivi.numero ?? -1])
        #expect(banc.lignes.filter { $0.enveloppe.t == "tx" }.count >= 3)
    }

    @Test func refusEtUsage() async throws {
        let banc = try Banc(vitesse: 1)
        var ids: [UUID] = []
        try await banc.lire(delai: .seconds(20), jusqua: { b in
            !ids.isEmpty && ids.allSatisfy { b.moteur.correlateur.suivi($0)?.etat.estFinal == true }
        }) { b in
            if b.moteur.phase == .connecte, ids.isEmpty {
                for c in ["lampe lum 20", "lampe stats", "commande_inexistante"] {
                    let (id, effets) = b.moteur.soumettre(c, origine: .console, maintenant: b.maintenant())
                    ids.append(id)
                    try b.executer(effets)
                }
            }
        }
        try #require(ids.count == 3)
        let usage = banc.moteur.correlateur.suivi(ids[0])
        #expect(usage?.fin?.code == .usage)
        #expect(usage?.fin?.msg == "lum : 4C..FE, en hexa")
        // Commande historique : reponse debut, texte rattache, reponse fin "execute" (12.6).
        let stats = banc.moteur.correlateur.suivi(ids[1])
        #expect(stats?.fin?.code == .execute)
        #expect(stats?.debutA != nil)
        #expect(stats?.texte.isEmpty == false)
        #expect(banc.moteur.correlateur.suivi(ids[2])?.fin?.code == .commandeInconnue)
    }

    @Test func razDesStatistiques() async throws {
        // lampe stats raz (5.4) : blocs pilote et radio remis a zero, raz + 1, tx.total garde.
        let banc = try Banc(vitesse: 1)
        var envoye = false
        func apres(_ b: Banc) -> (CompteursPilote, CompteursRadio)? {
            var p: CompteursPilote?
            var r: CompteursRadio?
            for l in b.lignes {
                if case .compteursPilote(let c) = l.message, c.raz == 1 { p = c }
                if case .compteursRadio(let c) = l.message, c.raz == 1 { r = c }
            }
            guard let p, let r else { return nil }
            return (p, r)
        }
        try await banc.lire(delai: .seconds(20), jusqua: { apres($0) != nil }) { b in
            if b.moteur.phase == .connecte, !envoye {
                envoye = true
                let (_, e) = b.moteur.soumettre("lampe stats raz", origine: .console, maintenant: b.maintenant())
                try b.executer(e)
            }
        }
        let (p, r) = try #require(apres(banc))
        #expect(p.tx?.paquets == 0)
        #expect(p.rx?.trames == 0)
        #expect(p.tx?.total == 21, "tx.total n'est jamais remis a zero")
        #expect(r.radio?.configs == 0)
        #expect(r.relances?.total == 0)
        #expect(LigneJSON.soustraire(#"{"v":1,"n":5,"raz":1,"tx":{"paquets":30,"total":30}}"#,
                                     base: LigneJSON.entiers(#"{"v":1,"n":2,"raz":0,"tx":{"paquets":21,"total":21}}"#),
                                     sauf: ["v", "n", "ms", "raz", "total"])
                == #"{"v":1,"n":5,"raz":1,"tx":{"paquets":9,"total":30}}"#)
    }

    @Test func toutLaChronologieAccelereeJusquAuRedemarrage() async throws {
        // x25 : les 195 s de la demo en ~8 s, puis la carte "redemarre" (flux ferme).
        let banc = try Banc(vitesse: 25)
        let debut = ContinuousClock.now
        try await banc.lire(delai: .seconds(60))
        #expect(ContinuousClock.now - debut < .seconds(59), "la demo doit se fermer seule (re-enumeration simulee)")
        let types = banc.types()
        for t in ["rx", "tx", "livraison", "relance", "module", "intent", "abonnement", "thread", "led"] {
            #expect(types.contains(t), "\(t) jamais recu")
        }
        let livraisons = banc.lignes.compactMap { l -> Livraison? in
            if case .livraison(let v) = l.message { return v }
            return nil
        }
        #expect(livraisons.contains { $0.issue == .abandon && $0.cause == .injoignable })
        let modules = banc.lignes.compactMap { l -> EtatModule? in
            if case .module(let v) = l.message { return v.etat }
            return nil
        }
        #expect(modules == [.panne, .retabli])
        let relances = banc.lignes.compactMap { l -> CauseRelance? in
            if case .relance(let v) = l.message { return v.cause }
            return nil
        }
        #expect(relances == [.sourde, .delais, .delais, .delais])
        // La ligne coupee par un log IDF (2.1) : une ligne abimee, un fragment, et des trous de n.
        #expect(banc.recepteur.compteurs.lignesAbimees == 1)
        #expect(banc.recepteur.compteurs.fragments == 1)
        #expect(banc.moteur.statistiques.pertes >= 2)
        #expect(banc.textes.contains { $0.hasPrefix("E (") })
        #expect(banc.textes.contains("[lampe] injoignable : consigne abandonnee"))
    }
}

@Suite("Modele de l'app en mode demo", .serialized, .langue(.francais))
@MainActor
struct PontDemoTests {
    @Test func connexionEtatEtCommande() async throws {
        let pont = Pont()
        pont.connecter(.demo)
        let pret = await attendre {
            pont.phase == .connecte && pont.etat.lampe != nil && pont.etat.identite != nil && !pont.pilote.elements.isEmpty
        }
        try #require(pret, "session demo etablie")
        #expect(pont.phase == .connecte)
        #expect(pont.etatTransport == .ouvert)
        #expect(pont.etat.lampe?.valeur.consigne.niveau == 180)
        #expect(pont.etat.identite?.valeur.id?.serie == "HALO1-F0F5BD012345")
        #expect(pont.etat.ancre != nil, "l'ancre du temps est posee au hello")
        #expect(!pont.pilote.elements.isEmpty)

        pont.envoyer("lampe niveau 200")
        _ = await attendre { pont.suivis.last { $0.commande == "lampe niveau 200" }?.etat.estFinal == true }
        let suivi = try #require(pont.suivis.last { $0.commande == "lampe niveau 200" })
        #expect(suivi.etat == .livree)
        #expect(pont.console.elements.contains { $0.texte.contains("lampe niveau 200") })

        // Console : confirmation demandee, interdits refuses (6.4).
        #expect(pont.console("reboot") == .confirmation("Redémarre la carte (le port USB va se ré-énumérer)."))
        if case .refusee = pont.console("json 0") {} else { Issue.record("json 0 doit passer par Liberer le port") }
        if case .refusee = pont.console("lampe é") {} else { Issue.record("accents refuses") }

        pont.deconnecter()
        #expect(pont.phase == .ferme)
    }

    @Test func redemarrageEtReconnexion() async throws {
        // Demo acceleree : a la fin de la chronologie la carte "redemarre" (flux ferme,
        // comme une re-enumeration USB) ; l'app rouvre apres 300 ms et voit un nouveau boot.
        let pont = Pont()
        pont.vitesseDemo = 25
        pont.connecter(.demo)
        let reconnectee = await attendre(.seconds(60)) {
            pont.statistiques.redemarrages >= 1 && pont.statistiques.connexions >= 2 && pont.phase == .connecte
                && pont.etat.helloBase != nil && Courbes.segmenter(pont.pilote.elements).count >= 2
        }
        try #require(reconnectee, "redemarrage simule puis reconnexion")
        #expect(pont.statistiques.redemarrages >= 1)
        #expect(pont.statistiques.connexions >= 2)
        #expect(pont.phase == .connecte)
        let boot = try #require(pont.etat.helloBase?.valeur.boot, "le hello du nouveau demarrage reste")
        #expect(boot != "3FA2C901")
        #expect(pont.marqueurs.elements.contains { $0.genre == .redemarrage })
        #expect(Courbes.segmenter(pont.pilote.elements).count >= 2, "nouveau segment de courbes")
        pont.deconnecter()
    }

    @Test func changerDeSourceRemetTout() async throws {
        let pont = Pont()
        pont.connecter(.demo)
        try #require(await attendre { pont.phase == .connecte && !pont.pilote.elements.isEmpty && !pont.trames.elements.isEmpty })
        #expect(pont.reglages?.trames == true)
        // Autre source (chemin inexistant : l'ouverture echoue, aucun port reel n'est touche).
        pont.connecter(.serie(chemin: "/dev/cu.halo-test-inexistant", serie: nil))
        #expect(pont.etat.lampe == nil && pont.etat.helloBase == nil, "rien de la demo ne reste affiche")
        #expect(pont.pilote.elements.isEmpty && pont.trames.elements.isEmpty && pont.marqueurs.elements.isEmpty)
        #expect(pont.statistiques == StatistiquesLien())
        #expect(pont.reglages == nil)
        #expect(pont.console.elements.contains { $0.texte.hasPrefix("Nouvelle source") })
        _ = await attendre(.seconds(2)) { if case .attente = pont.etatTransport { true } else { false } }
        if case .attente = pont.etatTransport {} else { Issue.record("reouverture programmee attendue : \(pont.etatTransport)") }
        #expect(pont.statistiques.redemarrages == 0, "pas de faux redemarrage")
        pont.deconnecter()
    }
}

@Suite("Calendrier du voyant")
struct HoraireVoyantTests {
    let depuis = Date(timeIntervalSinceReferenceDate: 0)

    func instants(_ m: MotifLed?, _ n: Int) -> [Double] {
        HoraireVoyant(motif: m, depuis: depuis).entries(from: depuis, mode: .normal).prefix(n)
            .map { $0.timeIntervalSince(depuis) }
    }

    @Test func motifFixeUneSeuleImage() {
        #expect(instants(.panneRadio, 10) == [0])
        #expect(instants(nil, 10) == [0])
        #expect(instants(.inconnu, 10) == [0])
    }

    @Test func eclatPuisPlusRien() {
        let t = instants(.livree, 10)
        #expect(t.count <= 3 && (t.last ?? 1) < 0.2, "eclat vert de 150 ms, puis le calendrier s'arrete : \(t)")
    }

    @Test func clignementsAuxBasculesSeulement() {
        #expect(instants(.nonAppaire, 4) == [0, 0.25, 0.5, 0.75])
        #expect(instants(.horsReseau, 3) == [0, 1, 2])
        #expect(instants(.injoignable, 20).count == 7, "3 clignements de 200 ms / 200 ms, puis noir")
    }

    @Test func lueurPuisAttenteDeDixSecondes() {
        let t = instants(.operationnel, 40)
        #expect(zip(t, t.dropFirst()).allSatisfy { $0 < $1 }, "strictement croissant")
        let pendantLaLueur = t.filter { $0 < 0.7 }.count
        #expect(pendantLaLueur >= 15 && pendantLaLueur <= 22, "~30 images/s pendant 600 ms")
        #expect(t.contains(10), "rien entre la fin de la lueur et 10 s")
        #expect(!t.contains { $0 > 0.7 && $0 < 10 })
    }
}
