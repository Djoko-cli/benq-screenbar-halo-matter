import Foundation

/// Sens decode des messages, pour le journal des trames et la console, dans
/// la langue en vigueur (`Localisation`).
public enum Interpretation {
    public static func hexa(_ v: Int) -> String {
        String(format: "%02X", max(0, min(255, v)))
    }

    /// Nombre a virgule dans les formats de la langue en vigueur ("2,00", "2.00").
    public static func decimal(_ v: Double, chiffres: Int) -> String {
        v.formatted(.number.precision(.fractionLength(chiffres)).locale(Localisation.partagee.locale))
    }

    static func marche(_ m: Bool) -> String { m ? tr("allumée") : tr("éteinte") }

    /// Objet Etat en une ligne : "allumée · les deux · lum A5 (niveau 180) · temp 53 (268 mireds)".
    public static func etat(_ e: EtatLampe?) -> String {
        guard let e else { return "-" }
        var p: [String] = []
        if let m = e.marche { p.append(marche(m)) }
        if let l = e.lampes { p.append(l.libelle) }
        if let lum = e.lum {
            let h = hexa(lum)
            p.append(e.niveau.map { tr("lum \(h) (niveau \($0))") } ?? tr("lum \(h)"))
        }
        if let t = e.temp {
            p.append(e.mired.map { tr("temp \(t) (\($0) mireds)") } ?? tr("temp \(t)"))
        }
        return p.joined(separator: " · ")
    }

    public static func champs(_ c: [ChampConsigne]?) -> String {
        guard let c, !c.isEmpty else { return tr("aucun") }
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
            let de = t.de ?? "?"
            return tr("Thread : \(de) → \(t.vers)") + (t.total.map { tr(" (\($0) changements)") } ?? "")
        case .led(let l):
            var s = tr("Voyant : \(l.motif.libelle)")
            if let a = l.avant { s += tr(" (avant : \(a.libelle))") }
            if l.test == true { s += tr(" · test") }
            return s
        case .log(let l):
            return l.txt
        case .reponse(let r):
            return reponse(r)
        case .fin(let f):
            switch f.cause {
            case .bail: return tr("Fin du mode machine : bail échu (hôte muet)")
            case .commande: return tr("Fin du mode machine : json 0")
            default: return tr("Fin du mode machine")
            }
        case .battement(let b):
            return tr("Battement") + (b.upS.map { tr(" · en marche depuis \($0) s") } ?? "")
        case .helloBase(let h):
            let fw = h.fw ?? "?", env = h.env ?? "?", boot = h.boot ?? "?", reset = h.reset ?? "?"
            return tr("Hello : firmware \(fw) · \(env) · démarrage \(boot) (\(reset))")
        case .helloIdentite(let h):
            let serie = h.id?.serie ?? "?", caps = (h.caps ?? []).joined(separator: ", ")
            return tr("Identité : \(serie) · capacités \(caps)")
        case .config(let c):
            let adresse = c.lampe?.air ?? "?"
            let canal = c.lampe?.canal.map(String.init) ?? "?"
            let gamma = c.reglages?.gammaC.map { decimal(Double($0) / 100, chiffres: 2) } ?? "?"
            return tr("Configuration : adresse \(adresse) · canal \(canal) · gamma \(gamma)")
        case .etatLampe(let b):
            let consigne = etat(b.consigne), cru = etat(b.cru)
            return tr("État : consigne \(consigne) · cru \(cru)")
        case .etatTranches(let b):
            let n = b.tranches?.count ?? 0
            return n == 0 ? tr("Aucune tranche active") : tr("\(n) tranche(s) active(s)")
        case .etatSante(let b):
            let mode = b.radio?.mode?.libelle ?? "?"
            return tr("Santé : radio \(mode)") + (b.surveil?.panne == true ? tr(" · EN PANNE") : "")
        case .compteursPilote, .compteursRadio, .compteursMatter:
            return tr("Compteurs")
        case .reseauThread(let r):
            let role = r.thread?.role ?? "?"
            return tr("Thread : \(role)") + (r.thread?.parentRssi.map { tr(" · RSSI parent \($0) dBm") } ?? "")
        case .reseauAbonnements(let r):
            let actifs = r.abonnements?.actifs.map(String.init) ?? "?"
            return tr("Abonnements actifs : \(actifs)")
        case .inconnu:
            return tr("Message inconnu (ignoré)")
        }
    }

    public static func rx(_ r: TrameRx, correspondance: CorrespondanceLuminosite = .init()) -> String {
        var s: String
        let sens = r.sens
        func contexte() -> String {
            var p: [String] = []
            if let l = sens?.lampes { p.append(l.libelle) }
            if let m = sens?.marche { p.append(marche(m)) }
            return p.isEmpty ? "" : " · " + p.joined(separator: " · ")
        }
        let charge = r.charge ?? ""
        switch r.type {
        case .lum:
            if let lum = sens?.lum {
                let h = hexa(lum), n = correspondance.niveau(brut: lum)
                s = tr("Luminosité \(h) (niveau \(n))") + contexte()
            } else {
                s = tr("Luminosité") + contexte()
            }
        case .temp:
            if let t = sens?.temp {
                let m = CorrespondanceLuminosite.mired(temp: t)
                let k = CorrespondanceLuminosite.kelvin(mired: m)
                s = tr("Température \(t) (\(m) mireds, ~\(k) K)") + contexte()
            } else {
                s = tr("Température") + contexte()
            }
        case .a:
            let numero = sens?.numero.map(String.init) ?? "?"
            s = tr("Bouton A, appui n° \(numero)") + (sens?.copie == true ? tr(" (copie)") : "")
        case .accuseLampe:
            s = tr("Accusé de la lampe")
        case .service:
            s = charge.prefix(2) == "FA" ? tr("Trame de service \(charge) (annonce)") : tr("Trame de service \(charge) (réveil)")
        case .favori:
            s = tr("Favori \(charge) (sans effet visible)")
        case .invalide:
            s = tr("Trame invalide \(charge)")
        case .crcFaux:
            s = tr("CRC faux : charge douteuse \(charge)")
        case .inconnu:
            s = tr("Trame de type inconnu")
        }
        if r.source == .accuse { s += tr(" · reçue à la place d'un accusé") }
        if let n = r.sautes, n > 0 { s += tr(" · \(n) omise(s) avant") }
        return s
    }

    public static func tx(_ t: PaquetTx) -> String {
        // Numeros, id et versions : des identifiants, sans separateur de milliers.
        var s = t.num.map { tr("Paquet n° \(String($0))") } ?? tr("Paquet")
        s += " · \(t.tranche?.libelle ?? "?") \(t.charge ?? "")"
        if let e = t.essai { s += t.paquets.map { tr(" · essai \(e)/\($0)") } ?? tr(" · essai \(e)") }
        s += " · \(t.verdict.libelle)"
        if let us = t.us { s += tr(" en \(us) µs") }
        if let a = t.accuses { s += tr(" · \(a) accusé(s)") }
        if let n = t.sautes, n > 0 { s += tr(" · \(n) omis avant") }
        return s
    }

    public static func livraison(_ l: Livraison) -> String {
        var s: String
        switch l.issue {
        case .livree:
            s = l.derniere.map { tr("Consigne livrée (\($0.libelle))") } ?? tr("Consigne livrée")
        case .abandon:
            s = l.cause.map { tr("Consigne abandonnée : \($0.libelle)") } ?? tr("Consigne abandonnée")
        case .annulee:
            s = tr("Consigne annulée")
        case .inconnu:
            s = tr("Livraison")
        }
        if let v = l.version { s += tr(" · version \(String(v))") }
        if let ids = l.ids, !ids.isEmpty {
            let liste = ids.map(String.init).joined(separator: ", ")
            s += tr(" · id \(liste)")
        } else {
            s += tr(" · hors app (Matter, télécommande ou console)")
        }
        if let p = l.idsPerdus, p > 0 { s += tr(" · \(p) id perdus") }
        if let a = l.attenteMs { s += " · \(a) ms" }
        if let r = l.aLivrer, !r.isEmpty { s += tr(" · reste : \(champs(r))") }
        return s
    }

    public static func relance(_ r: Relance) -> String {
        var s = tr("Relance du module : \(r.cause.libelle)")
        if let rang = r.rang { s += tr(" (rang \(rang))") }
        if let d = r.detail {
            var p: [String] = []
            if let v = d.suite { p.append(tr("\(v) délais de suite")) }
            if let v = d.trames { p.append(tr("\(v) trames")) }
            if let v = d.crcFaux { p.append(tr("\(v) CRC faux")) }
            if let v = d.horsRx { p.append(tr("\(v) réarmements hors RX")) }
            if let v = d.verifRatees { p.append(tr("\(v) vérifications ratées")) }
            if let v = d.ms { p.append(tr("en \(v) ms")) }
            if !p.isEmpty { s += " · " + p.joined(separator: ", ") }
        }
        if let ok = r.ok { s += ok ? tr(" · réussie") : tr(" · ÉCHEC") }
        if let d = r.dureeMs { s += tr(" en \(d) ms") }
        if let t = r.total { s += tr(" · \(t) au total") }
        if r.panne == true { s += tr(" · EN PANNE") }
        return s
    }

    public static func module(_ e: EvenementModule) -> String {
        switch e.etat {
        case .panne:
            var s = tr("Module EN PANNE")
            if let n = e.sansGuerison { s += tr(" : \(n) relances sans guérison") }
            if let sy = e.symptome { s += " (\(sy.libelle))" }
            if let es = e.essaiS { s += tr(" · prochain essai dans \(es) s") }
            return s
        case .configRejetee:
            let rfch = e.rfch ?? "?", dm1 = e.dm1 ?? "?", rt1 = e.rt1 ?? "?"
            return tr("Configuration toujours rejetée : RFCH \(rfch) DM1 \(dm1) RT1 \(rt1) (attendus 05, 82, 73)")
        default:
            return tr("Module \(e.etat.libelle)")
        }
    }

    public static func intent(_ i: IntentMatter) -> String {
        var ordres: [String] = []
        if let r = i.recu {
            if let v = r.ep1 { ordres.append(v ? tr("EP1 allumé") : tr("EP1 éteint")) }
            if let v = r.niveau { ordres.append(tr("niveau \(v)")) }
            if let v = r.mireds { ordres.append("\(v) mireds") }
            if let v = r.avant { ordres.append(v ? tr("avant on") : tr("avant off")) }
            if let v = r.arriere { ordres.append(v ? tr("arrière on") : tr("arrière off")) }
            if r.a == true { ordres.append(tr("bouton A")) }
        }
        let liste = ordres.joined(separator: ", ")
        var s = ordres.isEmpty ? tr("Matter : ordre") : tr("Matter : \(liste)")
        if i.ignore == "demarrage" { return s + tr(" · ignoré (démarrage)") }
        if let c = i.champs {
            s += c.isEmpty ? tr(" · écarté") : tr(" · champs \(champs(c))")
        }
        if let c = i.consigne { s += " → \(etat(c))" }
        if let a = i.a { s += tr(" · A : \(a)") }
        return s
    }

    public static func abonnement(_ a: EvenementAbonnement) -> String {
        var s: String
        let abonne = a.abonne ?? "?"
        switch a.quoi {
        case .demande:
            s = tr("Abonnement demandé par \(abonne)")
            if let m = a.maxS { s += " · max \(m) s" }
            if let ap = a.appliqueS { s += tr(" (appliqué \(ap) s)") }
        case .etabli:
            let origine = a.origine ?? "?"
            s = tr("Abonnement établi (\(origine))")
            if let mi = a.minS, let ma = a.maxS { s += " · \(mi)..\(ma) s" }
        case .termine:
            s = tr("Abonnement terminé")
        case .reprise:
            let mode = a.mode ?? "?", verdict = a.verdict ?? "?"
            s = tr("Reprise des abonnements (\(mode)) : \(verdict)")
        case .session:
            let erreur = a.erreur ?? ""
            s = a.ok == true ? tr("Session de reprise vers \(abonne) : ouverte")
                : tr("Session de reprise vers \(abonne) : échec \(erreur)")
        case .repriseAbonne:
            let verdict = a.verdict ?? "?"
            s = tr("Reprise pour \(abonne) : \(verdict)")
        case .inconnu:
            s = tr("Abonnement")
        }
        if let t = a.totaux {
            let d = t.demandes ?? 0, e = t.etablis ?? 0, f = t.termines ?? 0
            s += tr(" · totaux \(d) demandés, \(e) établis, \(f) terminés")
        }
        return s
    }

    public static func reponse(_ r: Reponse) -> String {
        var s = "id=\(r.id)"
        if let c = r.cmd { s += tr(" « \(c) »") }
        switch r.etape {
        case .debut: s += tr(" : commence")
        default:
            s += r.ok ? tr(" : \(r.code.libelle)") : tr(" : ÉCHEC, \(r.code.libelle)")
        }
        if let m = r.msg { s += " (\(m))" }
        if let d = r.dureeMs, r.etape == .fin { s += " · \(d) ms" }
        if r.cle != nil { s += tr(" · clé reçue (masquée)") }
        return s
    }
}
