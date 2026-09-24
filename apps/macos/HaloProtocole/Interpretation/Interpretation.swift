import Foundation

/// Sens decode des messages, en francais, pour le journal des trames et la console.
public enum Interpretation {
    public static func hexa(_ v: Int) -> String {
        String(format: "%02X", max(0, min(255, v)))
    }

    /// Objet Etat en une ligne : "allumée · les deux · lum A5 (niveau 180) · temp 53 (268 mireds)".
    public static func etat(_ e: EtatLampe?) -> String {
        guard let e else { return "-" }
        var p: [String] = []
        if let m = e.marche { p.append(m ? "allumée" : "éteinte") }
        if let l = e.lampes { p.append(l.libelle) }
        if let lum = e.lum {
            p.append("lum \(hexa(lum))" + (e.niveau.map { " (niveau \($0))" } ?? ""))
        }
        if let t = e.temp {
            p.append("temp \(t)" + (e.mired.map { " (\($0) mireds)" } ?? ""))
        }
        return p.joined(separator: " · ")
    }

    public static func champs(_ c: [ChampConsigne]?) -> String {
        guard let c, !c.isEmpty else { return "aucun" }
        return c.map(\.libelle).joined(separator: ", ")
    }

    /// Resume d'un message.
    public static func resume(_ m: MessageCarte, correspondance: CorrespondanceLuminosite = .init()) -> String {
        switch m {
        case .rx(let r): return rx(r, correspondance: correspondance)
        case .tx(let t): return tx(t)
        case .livraison(let l): return livraison(l)
        case .relance(let r): return relance(r)
        case .module(let e): return module(e)
        case .intent(let i): return intent(i)
        case .abonnement(let a): return abonnement(a)
        case .thread(let t):
            return "Thread : \(t.de ?? "?") → \(t.vers)" + (t.total.map { " (\($0) changements)" } ?? "")
        case .led(let l):
            var s = "Voyant : \(l.motif.libelle)"
            if let a = l.avant { s += " (avant : \(a.libelle))" }
            if l.test == true { s += " · test" }
            return s
        case .log(let l):
            return l.txt
        case .reponse(let r):
            return reponse(r)
        case .fin(let f):
            switch f.cause {
            case .bail: return "Fin du mode machine : bail échu (hôte muet)"
            case .commande: return "Fin du mode machine : json 0"
            default: return "Fin du mode machine"
            }
        case .battement(let b):
            return "Battement" + (b.upS.map { " · en marche depuis \($0) s" } ?? "")
        case .helloBase(let h):
            return "Hello : firmware \(h.fw ?? "?") · \(h.env ?? "?") · démarrage \(h.boot ?? "?") (\(h.reset ?? "?"))"
        case .helloIdentite(let h):
            return "Identité : \(h.id?.serie ?? "?") · capacités \((h.caps ?? []).joined(separator: ", "))"
        case .config(let c):
            return "Configuration : adresse \(c.lampe?.air ?? "?") · canal \(c.lampe?.canal.map(String.init) ?? "?")"
                + " · gamma \(c.reglages?.gammaC.map { String(format: "%.2f", Double($0) / 100) } ?? "?")"
        case .etatLampe(let b):
            return "État : consigne \(etat(b.consigne)) · cru \(etat(b.cru))"
        case .etatTranches(let b):
            let n = b.tranches?.count ?? 0
            return n == 0 ? "Aucune tranche active" : "\(n) tranche(s) active(s)"
        case .etatSante(let b):
            return "Santé : radio \(b.radio?.mode?.libelle ?? "?")"
                + (b.surveil?.panne == true ? " · EN PANNE" : "")
        case .compteursPilote, .compteursRadio, .compteursMatter:
            return "Compteurs"
        case .reseauThread(let r):
            return "Thread : \(r.thread?.role ?? "?")" + (r.thread?.parentRssi.map { " · RSSI parent \($0) dBm" } ?? "")
        case .reseauAbonnements(let r):
            return "Abonnements actifs : \(r.abonnements?.actifs.map(String.init) ?? "?")"
        case .inconnu:
            return "Message inconnu (ignoré)"
        }
    }

    public static func rx(_ r: TrameRx, correspondance: CorrespondanceLuminosite = .init()) -> String {
        var s: String
        let sens = r.sens
        func contexte() -> String {
            var p: [String] = []
            if let l = sens?.lampes { p.append(l.libelle) }
            if let m = sens?.marche { p.append(m ? "allumée" : "éteinte") }
            return p.isEmpty ? "" : " · " + p.joined(separator: " · ")
        }
        switch r.type {
        case .lum:
            if let lum = sens?.lum {
                s = "Luminosité \(hexa(lum)) (niveau \(correspondance.niveau(brut: lum)))" + contexte()
            } else {
                s = "Luminosité" + contexte()
            }
        case .temp:
            if let t = sens?.temp {
                let m = CorrespondanceLuminosite.mired(temp: t)
                s = "Température \(t) (\(m) mireds, ~\(CorrespondanceLuminosite.kelvin(mired: m)) K)" + contexte()
            } else {
                s = "Température" + contexte()
            }
        case .a:
            s = "Bouton A, appui n° \(sens?.numero.map(String.init) ?? "?")" + (sens?.copie == true ? " (copie)" : "")
        case .accuseLampe:
            s = "Accusé de la lampe"
        case .service:
            let premier = r.charge?.prefix(2) ?? ""
            s = "Trame de service \(r.charge ?? "")" + (premier == "FA" ? " (annonce)" : " (réveil)")
        case .favori:
            s = "Favori \(r.charge ?? "") (sans effet visible)"
        case .invalide:
            s = "Trame invalide \(r.charge ?? "")"
        case .crcFaux:
            s = "CRC faux : charge douteuse \(r.charge ?? "")"
        case .inconnu:
            s = "Trame de type inconnu"
        }
        if r.source == .accuse { s += " · reçue à la place d'un accusé" }
        if let n = r.sautes, n > 0 { s += " · \(n) omise(s) avant" }
        return s
    }

    public static func tx(_ t: PaquetTx) -> String {
        var s = "Paquet"
        if let n = t.num { s += " n° \(n)" }
        s += " · \(t.tranche?.libelle ?? "?") \(t.charge ?? "")"
        if let e = t.essai { s += " · essai \(e)" + (t.paquets.map { "/\($0)" } ?? "") }
        s += " · \(t.verdict.libelle)"
        if let us = t.us { s += " en \(us) µs" }
        if let a = t.accuses { s += " · \(a) accusé(s)" }
        if let n = t.sautes, n > 0 { s += " · \(n) omis avant" }
        return s
    }

    public static func livraison(_ l: Livraison) -> String {
        var s: String
        switch l.issue {
        case .livree:
            s = "Consigne livrée" + (l.derniere.map { " (\($0.libelle))" } ?? "")
        case .abandon:
            s = "Consigne abandonnée" + (l.cause.map { " : \($0.libelle)" } ?? "")
        case .annulee:
            s = "Consigne annulée"
        case .inconnu:
            s = "Livraison"
        }
        if let v = l.version { s += " · version \(v)" }
        if let ids = l.ids, !ids.isEmpty { s += " · id " + ids.map(String.init).joined(separator: ", ") }
        else { s += " · hors app (Matter, télécommande ou console)" }
        if let p = l.idsPerdus, p > 0 { s += " · \(p) id perdus" }
        if let a = l.attenteMs { s += " · \(a) ms" }
        if let r = l.aLivrer, !r.isEmpty { s += " · reste : \(champs(r))" }
        return s
    }

    public static func relance(_ r: Relance) -> String {
        var s = "Relance du module : \(r.cause.libelle)"
        if let rang = r.rang { s += " (rang \(rang))" }
        if let d = r.detail {
            var p: [String] = []
            if let v = d.suite { p.append("\(v) délais de suite") }
            if let v = d.trames { p.append("\(v) trames") }
            if let v = d.crcFaux { p.append("\(v) CRC faux") }
            if let v = d.horsRx { p.append("\(v) réarmements hors RX") }
            if let v = d.verifRatees { p.append("\(v) vérifications ratées") }
            if let v = d.ms { p.append("en \(v) ms") }
            if !p.isEmpty { s += " · " + p.joined(separator: ", ") }
        }
        if let ok = r.ok { s += ok ? " · réussie" : " · ÉCHEC" }
        if let d = r.dureeMs { s += " en \(d) ms" }
        if let t = r.total { s += " · \(t) au total" }
        if r.panne == true { s += " · EN PANNE" }
        return s
    }

    public static func module(_ e: EvenementModule) -> String {
        switch e.etat {
        case .panne:
            var s = "Module EN PANNE"
            if let n = e.sansGuerison { s += " : \(n) relances sans guérison" }
            if let sy = e.symptome { s += " (\(sy.libelle))" }
            if let es = e.essaiS { s += " · prochain essai dans \(es) s" }
            return s
        case .configRejetee:
            return "Configuration toujours rejetée : RFCH \(e.rfch ?? "?") DM1 \(e.dm1 ?? "?") RT1 \(e.rt1 ?? "?")"
                + " (attendus 05, 82, 73)"
        default:
            return "Module \(e.etat.libelle)"
        }
    }

    public static func intent(_ i: IntentMatter) -> String {
        var ordres: [String] = []
        if let r = i.recu {
            if let v = r.ep1 { ordres.append("EP1 \(v ? "allumé" : "éteint")") }
            if let v = r.niveau { ordres.append("niveau \(v)") }
            if let v = r.mireds { ordres.append("\(v) mireds") }
            if let v = r.avant { ordres.append("avant \(v ? "on" : "off")") }
            if let v = r.arriere { ordres.append("arrière \(v ? "on" : "off")") }
            if r.a == true { ordres.append("bouton A") }
        }
        var s = "Matter : " + (ordres.isEmpty ? "ordre" : ordres.joined(separator: ", "))
        if i.ignore == "demarrage" { return s + " · ignoré (démarrage)" }
        if let c = i.champs {
            s += c.isEmpty ? " · écarté" : " · champs \(champs(c))"
        }
        if let c = i.consigne { s += " → \(etat(c))" }
        if let a = i.a { s += " · A : \(a)" }
        return s
    }

    public static func abonnement(_ a: EvenementAbonnement) -> String {
        var s: String
        switch a.quoi {
        case .demande:
            s = "Abonnement demandé par \(a.abonne ?? "?")"
            if let m = a.maxS { s += " · max \(m) s" }
            if let ap = a.appliqueS { s += " (appliqué \(ap) s)" }
        case .etabli:
            s = "Abonnement établi (\(a.origine ?? "?"))"
            if let mi = a.minS, let ma = a.maxS { s += " · \(mi)..\(ma) s" }
        case .termine:
            s = "Abonnement terminé"
        case .reprise:
            s = "Reprise des abonnements (\(a.mode ?? "?")) : \(a.verdict ?? "?")"
        case .session:
            s = "Session de reprise vers \(a.abonne ?? "?") : " + (a.ok == true ? "ouverte" : "échec \(a.erreur ?? "")")
        case .repriseAbonne:
            s = "Reprise pour \(a.abonne ?? "?") : \(a.verdict ?? "?")"
        case .inconnu:
            s = "Abonnement"
        }
        if let t = a.totaux {
            s += " · totaux \(t.demandes ?? 0) demandés, \(t.etablis ?? 0) établis, \(t.termines ?? 0) terminés"
        }
        return s
    }

    public static func reponse(_ r: Reponse) -> String {
        var s = "id=\(r.id)"
        if let c = r.cmd { s += " « \(c) »" }
        switch r.etape {
        case .debut: s += " : commence"
        default:
            s += r.ok ? " : \(r.code.libelle)" : " : ÉCHEC, \(r.code.libelle)"
        }
        if let m = r.msg { s += " (\(m))" }
        if let d = r.dureeMs, r.etape == .fin { s += " · \(d) ms" }
        if r.cle != nil { s += " · clé reçue (masquée)" }
        return s
    }
}
