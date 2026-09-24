import Foundation
import Testing
@testable import HaloProtocole

func texte(_ d: Data) -> String { String(decoding: d, as: UTF8.self) }

func reponse(_ id: Int, _ etape: EtapeReponse = .fin, code: CodeReponse = .ok, ok: Bool = true,
             suite: SuiteReponse? = nil, cmd: String? = nil) -> Reponse {
    Reponse(id: id, etape: etape, cmd: cmd, ok: ok, code: code, msg: nil, dureeMs: 1, suite: suite, consigne: nil,
            aLivrer: nil, version: nil, bailS: nil, upS: nil, cle: nil, empreinte: nil)
}

func livraison(_ issue: IssueLivraison, ids: [Int], perdus: Int = 0) -> Livraison {
    Livraison(issue: issue, cause: nil, derniere: nil, version: 1, consigne: nil, cru: nil, aLivrer: [],
              ids: ids, idsPerdus: perdus, attenteMs: 200, livrees: 1, abandons: 0)
}

@Suite("Correlation des commandes (6.2 a 6.5)")
struct CorrelateurTests {
    @Test func uneSeuleCommandeEnVol() throws {
        var c = Correlateur()
        let a = c.soumettre("lampe on", origine: .interface, maintenant: 0)
        let b = c.soumettre("lampe niveau 200", origine: .interface, maintenant: 0)
        let p1 = c.prochainEnvoi(maintenant: 0)
        let e1 = try #require(p1)
        #expect(e1.id == a)
        #expect(texte(e1.octets) == "id=1 lampe on\n")
        #expect(c.prochainEnvoi(maintenant: 0.1) == nil, "une seule commande en vol")

        #expect(c.recevoir(reponse(1, code: .accepte, suite: .livraison), maintenant: 0.2)
                    == .fin(a, livraisonAttendue: true))
        #expect(c.suivi(a)?.etat == .attenteLivraison)
        let p2 = c.prochainEnvoi(maintenant: 0.2)
        let e2 = try #require(p2)
        #expect(e2.id == b)
        #expect(texte(e2.octets) == "id=2 lampe niveau 200\n")
    }

    @Test func livraisonCouvreLesCommandesAcceptees() throws {
        var c = Correlateur()
        let a = c.soumettre("lampe niveau 100", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(reponse(1, code: .accepte, suite: .livraison), maintenant: 0)
        let b = c.soumettre("lampe niveau 150", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(reponse(2, code: .accepte, suite: .livraison), maintenant: 0)
        let d = c.soumettre("lampe temp 50", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(reponse(3, code: .differe, suite: .aucune), maintenant: 0)

        // La carte fond les consignes : une livraison porte les id en attente
        // (ici 1 est sorti de sa liste : ids_perdus).
        let touches = c.recevoir(livraison(.livree, ids: [2], perdus: 1), maintenant: 1)
        #expect(Set(touches) == [a, b])
        #expect(c.suivi(a)?.etat == .livree)
        #expect(c.suivi(b)?.etat == .livree)
        #expect(c.suivi(d)?.etat == .terminee, "differe : aucune livraison")
        // Une livraison d'ailleurs (ids vides) ne touche rien.
        #expect(c.recevoir(livraison(.abandon, ids: []), maintenant: 2).isEmpty)
    }

    @Test func abandonEtAnnulation() {
        var c = Correlateur()
        let a = c.soumettre("lampe lum 80", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(reponse(1, code: .accepte, suite: .livraison), maintenant: 0)
        c.recevoir(livraison(.abandon, ids: [1]), maintenant: 5)
        #expect(c.suivi(a)?.etat == .abandonnee)
        #expect(c.suivi(a)?.livraison?.issue == .abandon)

        let b = c.soumettre("lampe niveau 200", origine: .interface, maintenant: 6)
        _ = c.prochainEnvoi(maintenant: 6)
        _ = c.recevoir(reponse(2, code: .accepte, suite: .livraison), maintenant: 6)
        c.recevoir(livraison(.annulee, ids: [2]), maintenant: 7)
        #expect(c.suivi(b)?.etat == .annulee)
    }

    @Test func sansReponseSousTroisSecondes() throws {
        var c = Correlateur()
        let a = c.soumettre("lampe auto", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.verifierDelais(maintenant: 2.9).isEmpty)
        let expirees = c.verifierDelais(maintenant: 3.0)
        #expect(expirees.map(\.id) == [a])
        #expect(c.suivi(a)?.etat == .sansReponse)
        #expect(c.enVol == nil, "la suivante peut partir, sans reemission")
        #expect(c.prochainEnvoi(maintenant: 3) == nil)
        // Une reponse tardive met encore le suivi a jour.
        _ = c.recevoir(reponse(1, code: .refuse, ok: false), maintenant: 4)
        #expect(c.suivi(a)?.etat == .terminee)
    }

    @Test func commandeHistoriqueEtTexteRattache() throws {
        var c = Correlateur()
        let a = c.soumettre("lampe stats", origine: .console, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.recevoir(reponse(1, .debut, code: .enCours), maintenant: 0.01) == .debut(a))
        #expect(c.texte("  emission : 7 consignes") == a)
        #expect(c.commandeDeBanc?.id == a)
        // Apres debut : pas de verdict de silence, meme longtemps apres.
        #expect(c.verifierDelais(maintenant: 600).isEmpty)
        _ = c.recevoir(reponse(1, .fin, code: .execute), maintenant: 601)
        #expect(c.suivi(a)?.texte == ["  emission : 7 consignes"])
        #expect(c.suivi(a)?.etat == .terminee)
        #expect(c.texte("apres") == nil)
    }

    @Test func fusionDesCurseurs() throws {
        var c = Correlateur()
        _ = c.soumettre("json ping", origine: .session, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        let v1 = c.soumettre("lampe niveau 100", origine: .interface, fusion: "niveau", maintenant: 0.1)
        let v2 = c.soumettre("lampe niveau 120", origine: .interface, fusion: "niveau", maintenant: 0.2)
        let t = c.soumettre("lampe mired 300", origine: .interface, fusion: "mired", maintenant: 0.3)
        #expect(c.suivi(v1)?.etat == .remplacee)
        #expect(c.enFile == 2)
        _ = c.recevoir(reponse(1), maintenant: 0.4)
        let p1 = c.prochainEnvoi(maintenant: 0.4)
        #expect(p1?.id == v2)
        _ = c.recevoir(reponse(2), maintenant: 0.5)
        let p2 = c.prochainEnvoi(maintenant: 0.5)
        #expect(p2?.id == t)
    }

    @Test func reponseInattendueEtReinitialisation() throws {
        var c = Correlateur()
        #expect(c.recevoir(reponse(42), maintenant: 0) == .inattendue)
        let a = c.soumettre("lampe on", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        let b = c.soumettre("lampe off", origine: .interface, maintenant: 0)
        c.reinitialiser(maintenant: 1)
        #expect(c.suivi(a)?.etat == .perdue)
        #expect(c.suivi(b)?.etat == .perdue)
        #expect(c.enVol == nil)
        // Les numeros repartent a 1 par connexion.
        _ = c.soumettre("lampe sync", origine: .interface, maintenant: 2)
        let p = c.prochainEnvoi(maintenant: 2)
        #expect(p.map { texte($0.octets) } == "id=1 lampe sync\n")
    }

    @Test func numerotationBouclee() {
        #expect(LigneCommande.suivant(1) == 2)
        #expect(LigneCommande.suivant(999_999_999) == 1)
    }
}

@Suite("Moteur de session (3.3 a 3.6)")
struct MoteurSessionTests {
    static let hello = #"{"v":1,"t":"hello","n":0,"ms":83512,"bloc":"base","boot":"3FA2C901","up_s":83,"session":{"transport":"usb","periode_ms":1000,"bail_s":30}}"#

    static func element(_ json: String) -> ElementRecu {
        var r = RecepteurLignes()
        return r.alimenter(ligneMachine(json))[0]
    }

    static func envois(_ effets: [MoteurSession.Effet]) -> [String] {
        effets.compactMap { if case .envoyer(let d) = $0 { return texte(d) } else { return nil } }
    }

    @Test func sequenceDeConnexion() {
        var m = MoteurSession()
        let e = m.ouvert(maintenant: 0)
        #expect(Self.envois(e) == ["\u{15}\n", "id=1 json 1\n"])
        #expect(m.phase == .attenteHello(essai: 1))
        #expect(m.historique)
        _ = m.recu(Self.element(Self.hello), maintenant: 0.1)
        #expect(m.phase == .connecte)
        #expect(!m.historique)
        #expect(m.boot == "3FA2C901")
        #expect(m.bailS == 30)
        // La reponse au json 1 n'est pas une commande inattendue.
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":11,"ms":83523,"id":1,"etape":"fin","cmd":"json 1","ok":true,"code":"ok","duree_ms":12,"bail_s":30,"up_s":83}"#), maintenant: 0.2)
        #expect(m.phase == .connecte)
    }

    @Test func renvoisDuJson1PuisSansReponse() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        #expect(Self.envois(m.tic(maintenant: 1.9)).isEmpty)
        #expect(Self.envois(m.tic(maintenant: 2.0)) == ["id=2 json 1\n"])
        #expect(Self.envois(m.tic(maintenant: 4.0)) == ["id=3 json 1\n"])
        #expect(Self.envois(m.tic(maintenant: 6.0)) == ["id=4 json 1\n"])
        #expect(m.phase == .attenteHello(essai: 4))
        let e = m.tic(maintenant: 8.0)
        #expect(Self.envois(e).isEmpty)
        #expect(m.phase == .sansReponse)
        // Puis \x15\n et json 1 toutes les 30 s, pas plus souvent.
        #expect(Self.envois(m.tic(maintenant: 30)).isEmpty)
        #expect(Self.envois(m.tic(maintenant: 36.0)) == ["\u{15}\n", "id=5 json 1\n"])
        _ = m.recu(Self.element(Self.hello), maintenant: 37)
        #expect(m.phase == .connecte)
    }

    @Test func ancienFirmware() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        let e = m.recu(.texte(ClasseurTexte.classer(#"Commande inconnue : "id=1". Tape 'help'."#)), maintenant: 0.1)
        #expect(m.phase == .ancienFirmware)
        #expect(e.contains { if case .note(_, true) = $0 { return true } else { return false } })
        #expect(Self.envois(m.tic(maintenant: 60)).isEmpty, "plus de json 1 vers un ancien firmware")
    }

    @Test func versionInconnue() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(.versionInconnue(v: 2, t: "hello"), maintenant: 0.1)
        #expect(m.phase == .versionInconnue(2))
    }

    @Test func pingApresDixSecondes() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0.1)
        // L'etat periodique arrive : pas de silence.
        for t in stride(from: 1.0, through: 9.0, by: 1.0) {
            _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: t)
            #expect(Self.envois(m.tic(maintenant: t)).isEmpty)
        }
        _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 10)
        #expect(Self.envois(m.tic(maintenant: 10)) == ["id=2 json ping\n"])
    }

    @Test func silencePuisReouverture() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        // 3 x max(1 s, 2 s) = 6 s sans aucune ligne.
        #expect(Self.envois(m.tic(maintenant: 5.9)).isEmpty)
        let e = m.tic(maintenant: 6.0)
        #expect(Self.envois(e) == ["id=2 json 1\n"])
        #expect(m.phase == .resynchro)
        let r = m.tic(maintenant: 11.0)
        #expect(r.contains { if case .rouvrir = $0 { return true } else { return false } })
    }

    @Test func pasDeSilencePendantUneCommandeDeBanc() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        let (_, e) = m.soumettre("txack", origine: .console, maintenant: 0)
        #expect(Self.envois(e) == ["id=2 txack\n"])
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":1,"ms":1,"id":2,"etape":"debut","cmd":"txack","ok":true,"code":"en_cours"}"#), maintenant: 0.1)
        #expect(Self.envois(m.tic(maintenant: 300)).isEmpty)
        #expect(m.phase == .connecte)
        let long = m.tic(maintenant: 20 * 60 + 1)
        #expect(long.contains(.proposerFermeture))
    }

    @Test func commandeSansReponseDemandeUnInstantane() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        let (id, _) = m.soumettre("lampe auto", origine: .interface, maintenant: 1)
        _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 3.5)
        let e = m.tic(maintenant: 4.0)
        #expect(e.contains(.commandeSansReponse(id)))
        #expect(Self.envois(e) == ["id=3 json etat\n"])
    }

    @Test func redemarrageParBootOuUpS() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        let hb = #"{"v":1,"t":"hb","n":1,"ms":90000,"boot":"3FA2C901","up_s":90,"json_perdus":0}"#
        #expect(m.recu(Self.element(hb), maintenant: 1).isEmpty)
        // up_s qui recule, meme boot : redemarrage, json 1 renvoye.
        let e = m.recu(Self.element(#"{"v":1,"t":"hb","n":2,"ms":1000,"boot":"3FA2C901","up_s":1,"json_perdus":0}"#),
                       maintenant: 2)
        #expect(e.contains(.redemarrage(ancien: "3FA2C901", nouveau: "3FA2C901")))
        #expect(Self.envois(e) == ["id=2 json 1\n"])
        // Nouveau boot au hello suivant : redemarrage signale, pas de json 1 de plus.
        let h = m.recu(Self.element(Self.hello.replacingOccurrences(of: "3FA2C901", with: "0BADCAFE")), maintenant: 3)
        #expect(h.contains(.redemarrage(ancien: "3FA2C901", nouveau: "0BADCAFE")))
        #expect(Self.envois(h).isEmpty)
        #expect(m.statistiques.redemarrages == 2)
    }

    @Test func finDeBailPuisJson1() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        let e = m.recu(Self.element(#"{"v":1,"t":"fin","n":662,"ms":231400,"cause":"bail"}"#), maintenant: 5)
        #expect(Self.envois(e) == ["id=2 json 1\n"])
        #expect(m.phase == .attenteHello(essai: 1))
    }

    @Test func libererLePort() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        #expect(Self.envois(m.liberer(maintenant: 1)) == ["id=2 json 0\n"])
        #expect(m.phase == .ferme)
    }

    @Test func periodeSuivieParLeSeuilDeSilence() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0)
        _ = m.soumettre("json periode 5000", origine: .console, maintenant: 0)
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":1,"ms":1,"id":2,"etape":"fin","cmd":"json periode 5000","ok":true,"code":"ok"}"#), maintenant: 0.1)
        #expect(m.periodeMs == 5000)
        // 3 x 5 s = 15 s de silence toleres (ping a 10 s compris).
        _ = m.tic(maintenant: 10.2)
        #expect(m.phase == .connecte)
    }
}
