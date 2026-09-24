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
        let b = c.soumettre("lampe niveau 150", origine: .interface, maintenant: 0.1)
        _ = c.prochainEnvoi(maintenant: 0.1)
        _ = c.recevoir(reponse(2, code: .accepte, suite: .livraison), maintenant: 0.1)
        let d = c.soumettre("lampe temp 50", origine: .interface, maintenant: 0.2)
        _ = c.prochainEnvoi(maintenant: 0.2)
        _ = c.recevoir(reponse(3, code: .differe, suite: .aucune), maintenant: 0.2)

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
        // Les numeros ne repartent pas a 1 : une livraison tardive de l'ancienne
        // connexion (ids [1]) ne doit pas tomber sur une commande neuve.
        _ = c.soumettre("lampe sync", origine: .interface, maintenant: 2)
        let p = c.prochainEnvoi(maintenant: 2)
        #expect(p.map { texte($0.octets) } == "id=2 lampe sync\n")
        _ = c.recevoir(reponse(2, code: .accepte, suite: .livraison), maintenant: 2.1)
        #expect(c.recevoir(livraison(.livree, ids: [1]), maintenant: 2.2).isEmpty)
    }

    @Test func etapeInconnueNeClotRien() throws {
        var c = Correlateur()
        let a = c.soumettre("lampe stats", origine: .console, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.recevoir(reponse(1, .inconnu, code: .enCours), maintenant: 0.1) == .inattendue)
        #expect(c.suivi(a)?.etat == .envoyee, "une etape future ne vaut pas fin")
        #expect(c.enVol == a, "la place en vol reste prise")
        let b = c.soumettre("lampe on", origine: .interface, maintenant: 0.2)
        #expect(c.prochainEnvoi(maintenant: 0.2) == nil)
        _ = c.recevoir(reponse(1, .fin, code: .execute), maintenant: 0.3)
        #expect(c.prochainEnvoi(maintenant: 0.3)?.id == b)
    }

    @Test func auPlusVingtLignesParSeconde() throws {
        var c = Correlateur()
        _ = c.soumettre("json ping", origine: .session, maintenant: 0)
        let b = c.soumettre("lampe on", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(reponse(1), maintenant: 0.01)
        #expect(c.prochainEnvoi(maintenant: 0.02) == nil, "50 ms au moins entre deux lignes (6.5, cadence)")
        #expect(c.prochainEnvoi(maintenant: 0.05)?.id == b)
    }

    @Test func debutTardifBloqueLaFile() throws {
        var c = Correlateur()
        let banc = c.soumettre("txack", origine: .console, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.verifierDelais(maintenant: 3).map(\.id) == [banc])
        let etat = c.soumettre("json etat", origine: .session, maintenant: 3)
        _ = c.prochainEnvoi(maintenant: 3)
        // Le debut arrive apres le verdict "sans reponse" : la boucle de la carte est bloquee.
        #expect(c.recevoir(reponse(1, .debut, code: .enCours), maintenant: 3.5) == .debut(banc))
        #expect(c.commandeDeBanc?.id == banc)
        #expect(c.occupe)
        #expect(c.texte("txack : paquet 12") == banc, "le texte va a la commande de banc")
        let suivante = c.soumettre("lampe on", origine: .interface, maintenant: 4)
        _ = c.verifierDelais(maintenant: 6)  // json etat sans reponse : la carte ne lit plus
        #expect(c.suivi(etat)?.etat == .sansReponse)
        #expect(c.prochainEnvoi(maintenant: 60) == nil, "rien ne part pendant la commande de banc")
        _ = c.recevoir(reponse(1, .fin, code: .execute), maintenant: 600)
        #expect(c.commandeDeBanc == nil)
        #expect(c.prochainEnvoi(maintenant: 600)?.id == suivante)
    }

    @Test func finPerdueRattrapeeParUnBlocPeriodique() throws {
        var c = Correlateur()
        let a = c.soumettre("lampe stats", origine: .console, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(reponse(1, .debut, code: .enCours), maintenant: 0.1)
        let b = c.soumettre("lampe on", origine: .interface, maintenant: 0.2)
        // La fin est perdue ; un bloc etat prouve que la boucle de la carte tourne de nouveau.
        #expect(c.periodiqueRecu(maintenant: 1) == [a])
        #expect(c.suivi(a)?.etat == .finPerdue)
        #expect(c.prochainEnvoi(maintenant: 1)?.id == b)
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

    static func finJson1(_ id: Int = 1, n: Int = 11) -> ElementRecu {
        element(#"{"v":1,"t":"reponse","n":\#(n),"ms":83523,"id":\#(id),"etape":"fin","cmd":"json 1","ok":true,"code":"ok","duree_ms":12,"bail_s":30,"up_s":83}"#)
    }

    /// Ouverture, hello puis reponse au json 1 : session etablie, file libre.
    static func connecte(a t: TimeInterval = 0) -> MoteurSession {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: t)
        _ = m.recu(element(hello), maintenant: t)
        _ = m.recu(finJson1(), maintenant: t)
        return m
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
        // Une seule commande en vol (6.5) : rien ne part avant la reponse fin du json 1.
        #expect(m.instantaneEnCours)
        let (_, e1) = m.soumettre("lampe on", origine: .interface, maintenant: 0.15)
        #expect(Self.envois(e1).isEmpty)
        // La reponse au json 1 n'est pas une commande inattendue ; elle libere la file.
        let e2 = m.recu(Self.finJson1(), maintenant: 0.2)
        #expect(m.phase == .connecte)
        #expect(!m.instantaneEnCours)
        #expect(Self.envois(e2) == ["id=2 lampe on\n"])
    }

    @Test func reponseAuJson1SansHelloRenvoieJson1() {
        // hello perdu (json_perdus, coupe par un log IDF) : la reponse seule n'etablit rien,
        // et json 1 (idempotent) repart 2 s apres le premier envoi.
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.finJson1(), maintenant: 0.4)
        #expect(m.phase == .attenteHello(essai: 1))
        #expect(m.historique)
        #expect(Self.envois(m.tic(maintenant: 2.0)) == ["id=2 json 1\n"])
        _ = m.recu(Self.element(Self.hello), maintenant: 2.1)
        _ = m.recu(Self.finJson1(2, n: 12), maintenant: 2.2)
        #expect(m.phase == .connecte)
        #expect(!m.instantaneEnCours)
    }

    @Test func reponseCadenceAuJson1RenvoieJson1() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":3,"ms":1,"id":1,"etape":"fin","cmd":"json 1","ok":false,"code":"cadence"}"#),
                   maintenant: 0.1)
        #expect(Self.envois(m.tic(maintenant: 2.0)) == ["id=2 json 1\n"])
    }

    @Test func reponseAuJson1PerdueLibereLaFile() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0)
        _ = m.recu(Self.element(Self.hello), maintenant: 0.1)
        let (_, e) = m.soumettre("lampe on", origine: .interface, maintenant: 0.2)
        #expect(Self.envois(e).isEmpty)
        _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 2.9)
        #expect(Self.envois(m.tic(maintenant: 2.9)).isEmpty)
        #expect(Self.envois(m.tic(maintenant: 3.0)) == ["id=2 lampe on\n"])
    }

    @Test func numerosCroissantsApresReconnexion() {
        var m = MoteurSession()
        #expect(Self.envois(m.ouvert(maintenant: 0)).last == "id=1 json 1\n")
        m.ferme(maintenant: 1)
        #expect(Self.envois(m.ouvert(maintenant: 2)).last == "id=2 json 1\n")
    }

    @Test func pingAuTiersDUnBailCourt() {
        var m2 = MoteurSession()
        _ = m2.ouvert(maintenant: 0)
        _ = m2.recu(Self.element(Self.hello.replacingOccurrences(of: #""bail_s":30"#, with: #""bail_s":10"#)), maintenant: 0)
        _ = m2.recu(Self.element(#"{"v":1,"t":"reponse","n":11,"ms":1,"id":1,"etape":"fin","cmd":"json 1 bail 10","ok":true,"code":"ok","bail_s":10}"#),
                    maintenant: 0)
        #expect(m2.bailS == 10)
        _ = m2.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 3.3)
        #expect(Self.envois(m2.tic(maintenant: 3.3)).isEmpty)
        _ = m2.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 3.4)
        #expect(Self.envois(m2.tic(maintenant: 3.4)) == ["id=2 json ping\n"], "bail de 10 s : ping a 3,3 s")
    }

    @Test func reglagesSuivisDesCommandesJson() {
        var m = Self.connecte()
        #expect(m.reglages.trames == true)
        _ = m.soumettre("json trames 0", origine: .console, maintenant: 1)
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":12,"ms":1,"id":2,"etape":"fin","cmd":"json trames 0","ok":true,"code":"ok"}"#), maintenant: 1.1)
        #expect(m.reglages.trames == false)
        _ = m.soumettre("json compteurs 5000", origine: .console, maintenant: 2)
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":13,"ms":1,"id":3,"etape":"fin","cmd":"json compteurs 5000","ok":true,"code":"ok"}"#), maintenant: 2.1)
        #expect(m.reglages.compteursMs == 5000)
        // json 1 remet les reglages par defaut : le hello suivant les annonce.
        _ = m.recu(Self.element(Self.hello.replacingOccurrences(of: #""bail_s":30"#, with: #""bail_s":30,"trames":true"#)), maintenant: 3)
        #expect(m.reglages.trames == true)
    }

    @Test func redemarragePerdLesLivraisonsAttendues() throws {
        var m = Self.connecte()
        let (id, _) = m.soumettre("lampe niveau 200", origine: .interface, maintenant: 1)
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":12,"ms":1,"id":2,"etape":"fin","cmd":"lampe niveau 200","ok":true,"code":"accepte","suite":"livraison"}"#), maintenant: 1.1)
        #expect(m.correlateur.suivi(id)?.etat == .attenteLivraison)
        _ = m.recu(Self.element(#"{"v":1,"t":"hb","n":2,"ms":1000,"boot":"3FA2C901","up_s":1,"json_perdus":0}"#), maintenant: 2)
        #expect(m.correlateur.suivi(id)?.etat == .perdue, "la carte a redemarre : aucune livraison ne viendra")
    }

    @Test func debutTardifSuspendSilenceEtPing() {
        var m = Self.connecte()
        let (banc, e) = m.soumettre("txack", origine: .console, maintenant: 0.1)
        #expect(Self.envois(e) == ["id=2 txack\n"])
        _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 3.0)
        // Pas de reponse sous 3 s : "sans reponse", json etat demande.
        #expect(Self.envois(m.tic(maintenant: 3.1)) == ["id=3 json etat\n"])
        // Le debut arrive enfin : c'est une commande de banc, la boucle de la carte est bloquee.
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":12,"ms":1,"id":2,"etape":"debut","cmd":"txack","ok":true,"code":"en_cours"}"#), maintenant: 3.5)
        #expect(m.correlateur.commandeDeBanc?.id == banc)
        for t in stride(from: 4.0, through: 900, by: 7) {
            #expect(Self.envois(m.tic(maintenant: t)).isEmpty, "ni json 1 de silence, ni ping, ni commande (t = \(t))")
        }
        #expect(m.phase == .connecte)
        // La fin s'est perdue : le premier bloc etat de la carte revenue la clot.
        _ = m.recu(Self.element(#"{"v":1,"t":"etat","n":40,"ms":9,"bloc":"tranches","boot":"3FA2C901","up_s":990,"tranches":[]}"#), maintenant: 990)
        #expect(m.correlateur.suivi(banc)?.etat == .finPerdue)
        #expect(!m.correlateur.occupe)
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
        #expect(e.contains(.note(.ancienFirmware)))
        #expect(MoteurSession.Note.ancienFirmware.grave, "montree en bandeau")
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
        _ = m.recu(Self.finJson1(), maintenant: 0.2)
        // L'etat periodique arrive : pas de silence.
        for t in stride(from: 1.0, through: 9.0, by: 1.0) {
            _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: t)
            #expect(Self.envois(m.tic(maintenant: t)).isEmpty)
        }
        _ = m.recu(.texte(LigneTexte(texte: "x", classe: .commande)), maintenant: 10)
        #expect(Self.envois(m.tic(maintenant: 10)) == ["id=2 json ping\n"])
    }

    @Test func silencePuisReouverture() {
        var m = Self.connecte()
        // Une commande partie juste avant le silence : elle est perdue avec la session.
        let (enVol, _) = m.soumettre("lampe on", origine: .interface, maintenant: 5)
        // 3 x max(1 s, 2 s) = 6 s sans aucune ligne.
        #expect(Self.envois(m.tic(maintenant: 5.9)).isEmpty)
        let e = m.tic(maintenant: 6.0)
        #expect(Self.envois(e) == ["id=3 json 1\n"])
        #expect(m.phase == .resynchro)
        #expect(m.correlateur.suivi(enVol)?.etat == .perdue)
        #expect(m.correlateur.enVol == nil)
        let r = m.tic(maintenant: 11.0)
        #expect(r.contains { if case .rouvrir = $0 { return true } else { return false } })
    }

    @Test func pasDeSilencePendantUneCommandeDeBanc() {
        var m = Self.connecte()
        let (_, e) = m.soumettre("txack", origine: .console, maintenant: 0.1)
        #expect(Self.envois(e) == ["id=2 txack\n"])
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":1,"ms":1,"id":2,"etape":"debut","cmd":"txack","ok":true,"code":"en_cours"}"#), maintenant: 0.1)
        #expect(Self.envois(m.tic(maintenant: 300)).isEmpty)
        #expect(m.phase == .connecte)
        let long = m.tic(maintenant: 20 * 60 + 1)
        #expect(long.contains(.proposerFermeture))
    }

    @Test func commandeSansReponseDemandeUnInstantane() {
        var m = Self.connecte()
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
        var m = Self.connecte()
        _ = m.soumettre("json periode 5000", origine: .console, maintenant: 0.1)
        _ = m.recu(Self.element(#"{"v":1,"t":"reponse","n":1,"ms":1,"id":2,"etape":"fin","cmd":"json periode 5000","ok":true,"code":"ok"}"#), maintenant: 0.2)
        #expect(m.periodeMs == 5000)
        // 3 x 5 s = 15 s de silence toleres (ping a 10 s compris).
        _ = m.tic(maintenant: 10.2)
        #expect(m.phase == .connecte)
    }
}
