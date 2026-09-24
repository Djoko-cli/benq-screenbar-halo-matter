import Foundation
import Testing
@testable import HaloProtocole

/// Couverture des cles : tout champ non nul d'une ligne de la carte doit
/// ressortir non nul du modele decode. Une faute de frappe dans un nom de
/// propriete ou une `CodingKey` decoderait sinon en nil, sans rien dire
/// (tous les champs sont optionnels, 9.1).
enum CouvertureCles {
    /// Cle JSON -> nom de propriete, comme `.convertFromSnakeCase`.
    static func camel(_ cle: String) -> String {
        let parties = cle.split(separator: "_")
        guard parties.count > 1 else { return cle }
        return ([parties[0].lowercased()] + parties.dropFirst().map(\.capitalized)).joined()
    }

    /// Chemins des valeurs non nulles : objets parcourus, tableaux indexes
    /// (un tableau vide compte comme une valeur presente).
    static func feuilles(_ v: Any, _ chemin: String = "", convertir: Bool) -> Set<String> {
        switch v {
        case let o as [String: Any]:
            var s = Set<String>()
            for (k, x) in o {
                let c = convertir ? camel(k) : k
                s.formUnion(feuilles(x, chemin.isEmpty ? c : chemin + "." + c, convertir: convertir))
            }
            return s
        case let a as [Any]:
            if a.isEmpty { return [chemin] }
            var s = Set<String>()
            for (i, x) in a.enumerated() { s.formUnion(feuilles(x, "\(chemin)[\(i)]", convertir: convertir)) }
            return s
        case is NSNull:
            return []
        default:
            return [chemin]
        }
    }

    static func encoder(_ m: MessageCarte) throws -> Data? {
        let e = JSONEncoder()
        switch m {
        case .helloBase(let v): return try e.encode(v)
        case .helloIdentite(let v): return try e.encode(v)
        case .config(let v): return try e.encode(v)
        case .etatLampe(let v): return try e.encode(v)
        case .etatTranches(let v): return try e.encode(v)
        case .etatSante(let v): return try e.encode(v)
        case .compteursPilote(let v): return try e.encode(v)
        case .compteursRadio(let v): return try e.encode(v)
        case .compteursMatter(let v): return try e.encode(v)
        case .reseauThread(let v): return try e.encode(v)
        case .reseauAbonnements(let v): return try e.encode(v)
        case .reseauIp(let v): return try e.encode(v)
        case .battement(let v): return try e.encode(v)
        case .fin(let v): return try e.encode(v)
        case .reponse(let v): return try e.encode(v)
        case .rx(let v): return try e.encode(v)
        case .tx(let v): return try e.encode(v)
        case .livraison(let v): return try e.encode(v)
        case .relance(let v): return try e.encode(v)
        case .module(let v): return try e.encode(v)
        case .intent(let v): return try e.encode(v)
        case .abonnement(let v): return try e.encode(v)
        case .thread(let v): return try e.encode(v)
        case .led(let v): return try e.encode(v)
        case .log(let v): return try e.encode(v)
        case .inconnu: return nil
        }
    }

    /// Champs non nuls de la ligne que le modele decode a perdus.
    static func perdus(_ json: String) throws -> [String] {
        var r = RecepteurLignes()
        let e = r.alimenter([Octets.rs] + Array(json.utf8) + [Octets.lf])
        guard e.count == 1, case .machine(let l) = e[0] else { return ["<ligne rejetee : \(e)>"] }
        guard let source = try JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any],
              let d = try encoder(l.message)
        else { return ["<type non gere : \(l.cle)>"] }
        let attendus = feuilles(source, convertir: true).subtracting(["v", "t", "n", "ms", "bloc"])
        let obtenus = feuilles(try JSONSerialization.jsonObject(with: d), convertir: false)
        return attendus.subtracting(obtenus).sorted()
    }

    /// Champs que les exemples de la section 12 laissent nuls, vides ou absents.
    static let synthetiques: [String] = [
        // etat.sante : derniere relance (cle il_y_a_s), symptome, panne.
        #"{"v":1,"t":"etat","n":1,"ms":1,"bloc":"sante","boot":"3FA2C901","up_s":83,"radio":{"presente":true,"perdue":true,"mode":"veille","configuree":false,"quartz":false,"calib":false},"surveil":{"panne":true,"defaut":true,"symptome":"bruit","delais_suite":1,"fen_trames":120,"fen_crc_faux":110,"hors_rx_10s":12,"sans_guerison":3,"attente_ms":590000,"relances":4,"derniere":{"cause":"sourde","il_y_a_s":42}},"led":{"motif":"panne_radio","test":true},"matter":{"en_service":true,"connecte":false,"identify":true},"sys":{"heap":1,"heap_min":2,"heap_bloc":3,"pile_boucle":4,"boucle_max_ms":5,"json_perdus":6,"json_trop_longs":7,"rejets":8}}"#,
        // etat.lampe en reprise.
        #"{"v":1,"t":"etat","n":2,"ms":2,"bloc":"lampe","boot":"3FA2C901","up_s":83,"consigne":{"marche":true,"lampes":"avant","lum":186,"niveau":200,"temp":0,"mired":153},"cru":{"marche":false,"lampes":"arriere","lum":76,"niveau":4,"temp":100,"mired":370},"a_livrer":["lum","temp"],"confirme":["marche"],"version":13,"phase":"reprise","reprise_ms":640,"echecs":1,"lien":"perdu","accuse_ms":null,"dernier_a":7,"a_entendus":9,"memoire":"avant","livrees":4,"abandons":1,"sauve_attente":true,"ecoute":false,"trace":true}"#,
        // etat.tranches non vide.
        #"{"v":1,"t":"etat","n":3,"ms":3,"bloc":"tranches","boot":"3FA2C901","up_s":83,"tranches":[{"tranche":"lum","charge":"C5A5","accuses":1,"essais":2,"paquets":3},{"tranche":"a","charge":"C8A1","accuses":0,"essais":1,"paquets":3}]}"#,
        // relance : details bruit, verif, delais.
        #"{"v":1,"t":"relance","n":4,"ms":4,"cause":"bruit","rang":2,"detail":{"trames":130,"crc_faux":121,"ms":8200},"ok":true,"quartz":true,"calib":false,"duree_ms":305,"total":2,"panne":false}"#,
        #"{"v":1,"t":"relance","n":5,"ms":5,"cause":"verif","rang":1,"detail":{"verif_ratees":3},"ok":false,"quartz":null,"calib":null,"duree_ms":480,"total":3,"panne":true}"#,
        #"{"v":1,"t":"relance","n":6,"ms":6,"cause":"delais","rang":3,"detail":{"suite":3},"ok":true,"quartz":true,"calib":true,"duree_ms":301,"total":4,"panne":false}"#,
        // abonnement : reprise, session, reprise_abonne.
        #"{"v":1,"t":"abonnement","n":7,"ms":7,"quoi":"reprise","mode":"auto","verdict":"lance","sauves":2,"abonnes":1,"lances":1,"servis":0,"en_cours":true,"totaux":{"demandes":2,"etablis":2,"termines":1,"passages":2}}"#,
        #"{"v":1,"t":"abonnement","n":8,"ms":8,"quoi":"session","abonne":"0x000000000001B669","ok":false,"erreur":"0x00000032","duree_ms":1200,"totaux":{"demandes":2,"etablis":2,"termines":1,"passages":2}}"#,
        #"{"v":1,"t":"abonnement","n":9,"ms":9,"quoi":"reprise_abonne","abonne":"0x000000000001B669","verdict":"repris","repris":1,"sans_readhandler":0,"rates":2,"totaux":{"demandes":2,"etablis":2,"termines":1,"passages":3}}"#,
        // module : configuration rejetee (registres relus).
        #"{"v":1,"t":"module","n":10,"ms":10,"etat":"config_rejetee","rfch":"05","dm1":"80","rt1":"73"}"#,
        // compteurs.matter avec EP4 (a_*).
        #"{"v":1,"t":"compteurs","n":11,"ms":11,"bloc":"matter","fenetres":5,"ignorees":1,"a_appuis":2,"a_refuses":1,"a_entendus":3,"a_perdus":4,"reflets":11,"ecritures":14,"echecs":1,"verrou":2,"traces_perdues":3,"identify":1}"#,
        // reponse lampe asynchrone complete, puis json cle (empreinte).
        #"{"v":1,"t":"reponse","n":12,"ms":12,"id":17,"etape":"fin","cmd":"lampe niveau 200","ok":true,"code":"accepte","msg":"occupe","duree_ms":1,"suite":"livraison","consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":53,"mired":268},"a_livrer":["lum"],"version":13,"bail_s":30,"up_s":90}"#,
        #"{"v":1,"t":"reponse","n":13,"ms":13,"id":18,"etape":"fin","cmd":"json cle","ok":true,"code":"ok","duree_ms":2,"empreinte":"1A2B3C4D"}"#,
        // intent complet (EP4).
        #"{"v":1,"t":"intent","n":14,"ms":14,"recu":{"ep1":true,"niveau":200,"mireds":300,"avant":true,"arriere":false,"a":true},"fenetre_ms":130,"ignore":"demarrage","champs":["lum","temp"],"consigne":{"marche":true,"lampes":"deux","lum":186,"niveau":200,"temp":67,"mired":300},"version":14,"a":"appui"}"#,
        // livraison d'un abandon (cause, ids perdus).
        #"{"v":1,"t":"livraison","n":15,"ms":15,"issue":"abandon","cause":"module","derniere":null,"version":14,"consigne":{"marche":true,"lampes":"deux","lum":165,"niveau":180,"temp":53,"mired":268},"cru":{"marche":true,"lampes":"deux","lum":165,"niveau":180,"temp":53,"mired":268},"a_livrer":[],"ids":[3,4],"ids_perdus":2,"attente_ms":null,"livrees":4,"abandons":2}"#,
        // rx d'une copie de A, tx avec sautes.
        #"{"v":1,"t":"rx","n":16,"ms":16,"source":"ecoute","brut":"0962D2D86B000000","len":2,"pid":1,"no_ack":0,"charge":"C801","crc":"D86B","crc_ok":true,"type":"a","sens":{"numero":1,"copie":true},"sautes":3}"#,
        #"{"v":1,"t":"tx","n":17,"ms":17,"num":1204,"tranche":"brut","charge":"C5A5","essai":6,"paquets":3,"accuses":0,"verdict":"fifo","us":0,"rt2":"00","irq1":"0E","status":"0E","sautes":2}"#,
    ]
}

@Suite("Couverture des cles (9.1)")
struct CouvertureClesTests {
    @Test(arguments: (try? ExemplesSpec.lignes()) ?? [])
    func chaqueChampDesExemplesEstLu(_ json: String) throws {
        let perdus = try CouvertureCles.perdus(json)
        #expect(perdus.isEmpty, "champs perdus au decodage : \(perdus)")
    }

    @Test(arguments: CouvertureCles.synthetiques)
    func chaqueChampHorsExemplesEstLu(_ json: String) throws {
        let perdus = try CouvertureCles.perdus(json)
        #expect(perdus.isEmpty, "champs perdus au decodage : \(perdus)")
    }

    @Test func leTestVoitUneCleMalNommee() throws {
        // Temoin : un champ inconnu du modele est bien signale.
        let perdus = try CouvertureCles.perdus(#"{"v":1,"t":"module","n":1,"ms":1,"etat":"panne","champ_futur":3}"#)
        #expect(perdus == ["champFutur"])
    }

    @Test func etapeInconnueDecodeeEtIgnoree() throws {
        var r = RecepteurLignes()
        let e = r.alimenter(ligneMachine(#"{"v":1,"t":"reponse","n":1,"ms":1,"id":1,"etape":"progression","cmd":"txack","ok":true,"code":"en_cours"}"#))
        guard case .machine(let l) = e.first, case .reponse(let rep) = l.message else {
            Issue.record("reponse attendue : \(e)")
            return
        }
        #expect(rep.etape == .inconnu)
        var c = Correlateur()
        _ = c.soumettre("txack", origine: .console, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.recevoir(rep, maintenant: 0.1) == .inattendue)
        #expect(c.enVol != nil)
    }
}
