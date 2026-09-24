import Foundation
import HaloProtocole

/// Carte simulee du mode demo.
///
/// Elle rejoue la chronologie de `demo-halo.jsonl` a vitesse reelle et repond
/// aux lignes de l'app comme le ferait un firmware 0.4.0 (sections 3 et 6) :
/// `json 1` et l'instantane, bail et `json ping`, periodes, commandes `lampe`
/// asynchrones (`reponse`, `tx`, `livraison`, voyant), commandes historiques
/// (`reponse debut`, texte, `reponse fin`). Les blocs periodiques sont ceux du
/// fichier, re-emis a la cadence de la session ; les commandes de l'app les
/// surchargent (consigne, compteurs) jusqu'au prochain changement venu du fichier.
actor SimulateurDemo {
    struct Lampe: Equatable, Sendable {
        var marche: Bool
        var lampes: String
        var lum: Int
        var temp: Int
    }

    enum Action: Sendable {
        case ligne(String)
        case texte(String)
        case led(String, avant: String, test: Bool)
        case livrer(succes: Bool, derniere: String, debut: Int)
        case fermer(String)
    }

    private struct Programme: Sendable {
        let du: Int
        let groupe: Int
        let action: Action
    }

    private let script: ScriptDemo
    private let sortie: AsyncStream<EvenementTransport>.Continuation
    private let boot: String
    private let horloge = ContinuousClock()
    private let ouverture: ContinuousClock.Instant
    private let msOuverture: Int
    private let correspondance = CorrespondanceLuminosite(gammaC: 200)
    /// Facteur de vitesse du temps de la carte (1 : temps reel ; les tests accelerent).
    private let vitesse: Double

    // Session (3.4, 3.5)
    private var modeMachine = false
    private var periodeMs = 1000
    private var compteursMs = 1000
    private var reseauMs = 5000
    private var bailS = 30
    private var trames = true
    private var journalLog = false
    private var prochainEtat = 0
    private var prochainCompteurs = 0
    private var prochainReseau = 0
    private var prochainHb = 0
    private var dernierOctet: ContinuousClock.Instant

    // Rejeu
    private var decalage: Int?
    private var index = 0
    private var blocs: [String: (ligne: String, ms: Int)] = [:]
    private var n: UInt32 = 0
    private var couperProchaine: String?
    private var lampeDebranchee = false
    private var ecoute = true
    private var finDuScriptA: Int?
    private var termine = false
    private var ligneEnCours: [UInt8] = []
    private var motifLed = "operationnel"

    // Surcharge par les commandes de l'app
    private var consigne: Lampe?
    private var cru: Lampe?
    private var aLivrer: [String] = []
    private var derniereConsigneFichier: String?
    private var decalageVersion = 0
    private var ajoutsPilote: [String: Int] = [:]
    private var ajoutsLivrees = 0
    private var ajoutsAbandons = 0
    private var raz = 0
    /// Valeurs au dernier `lampe stats raz`, retranchees ensuite (5.4) : blocs
    /// `pilote` et `radio` (sauf `tx.total`), et `etat.sante.surveil.relances`.
    private var baseRaz: [String: [(cle: String, valeur: Int)]] = [:]
    private var baseRelancesSante = 0
    private var idsEnAttente: [Int] = []
    private var idsPerdus = 0
    private var numeroTx = 1000
    private var dernierA = 0
    private var groupeRafale = 0
    private var programme: [Programme] = []

    init(script: ScriptDemo, sortie: AsyncStream<EvenementTransport>.Continuation, boot: String, vitesse: Double = 1) {
        self.script = script
        self.vitesse = max(0.1, vitesse)
        self.sortie = sortie
        self.boot = boot
        let maintenant = ContinuousClock.now
        ouverture = maintenant
        dernierOctet = maintenant
        msOuverture = script.msDebut - 1500
        for e in script.instantane {
            if case .machine(let ligne, let t, let bloc) = e.genre { blocs[Self.cle(t, bloc)] = (ligne, e.ms) }
        }
        derniereConsigneFichier = blocs["etat.lampe"].flatMap { LigneJSON.valeur($0.ligne, "consigne") }.map(String.init)
    }

    private static func cle(_ t: String, _ bloc: String?) -> String {
        bloc.map { "\(t).\($0)" } ?? t
    }

    // MARK: - Boucle

    func executer(entrees: AsyncStream<Data>) async {
        // Reste d'une session humaine : une annonce, puis l'invite (sans fin de ligne).
        texte("[lampe] ecoute de fond active")
        octets(Array("> ".utf8))
        await withTaskGroup(of: Void.self) { groupe in
            groupe.addTask {
                for await d in entrees { await self.recevoir(d) }
            }
            groupe.addTask {
                while !Task.isCancelled, await !self.estTermine {
                    try? await Task.sleep(for: .milliseconds(40))
                    await self.tic()
                }
            }
            _ = await groupe.next()
            groupe.cancelAll()
        }
    }

    var estTermine: Bool { termine }

    private func msCarte() -> Int {
        msOuverture + Int((horloge.now - ouverture) / .milliseconds(1) * vitesse)
    }

    private func tic() {
        guard !termine else { return }
        let ms = msCarte()

        if modeMachine, bailS > 0, horloge.now - dernierOctet > .seconds(bailS) {
            modeMachine = false
            emettre(LigneJSON.machine("fin", [("cause", "bail")]))
            texte("json : mode machine coupe (hote muet depuis \(bailS) s)")
            octets(Array("> ".utf8))
        }

        if let d = decalage {
            while index < script.suite.count, script.suite[index].ms + d <= ms, !termine {
                jouer(script.suite[index], ms: ms)
                index += 1
            }
            if index >= script.suite.count {
                if finDuScriptA == nil { finDuScriptA = ms }
                if let f = finDuScriptA, ms - f > 15_000 {
                    fermer("Démo : fin de la chronologie, la carte redémarre (ré-énumération USB simulée)")
                    return
                }
            }
        }

        let dus = programme.filter { $0.du <= ms }.sorted { $0.du < $1.du }
        if !dus.isEmpty {
            programme.removeAll { $0.du <= ms }
            for p in dus where !termine { executer(p.action) }
        }

        guard modeMachine, !termine else { return }
        if periodeMs > 0, ms >= prochainEtat {
            prochainEtat = ms + periodeMs
            for b in ["etat.lampe", "etat.tranches", "etat.sante"] { emettreBloc(b) }
        }
        if compteursMs > 0, ms >= prochainCompteurs {
            prochainCompteurs = ms + compteursMs
            for b in ["compteurs.pilote", "compteurs.radio", "compteurs.matter"] { emettreBloc(b) }
        }
        if reseauMs > 0, ms >= prochainReseau {
            prochainReseau = ms + reseauMs
            for b in ["reseau.thread", "reseau.abonnements"] { emettreBloc(b) }
        }
        if periodeMs == 0 || periodeMs > 2000, ms >= prochainHb {
            prochainHb = ms + 2000
            emettre(LigneJSON.machine("hb", [("boot", .texte(boot)), ("up_s", .entier(ms / 1000)), ("json_perdus", 0)]))
        }
    }

    private func jouer(_ e: ScriptDemo.Element, ms: Int) {
        switch e.genre {
        case .machine(let ligne, let t, let bloc):
            switch t {
            case "etat", "compteurs", "reseau", "hello", "config":
                let cle = Self.cle(t, bloc)
                blocs[cle] = (ligne, e.ms)
                if cle == "etat.lampe" {
                    let c = LigneJSON.valeur(ligne, "consigne").map(String.init)
                    if c != derniereConsigneFichier {
                        // La chronologie reprend la main sur la consigne.
                        derniereConsigneFichier = c
                        consigne = nil
                        cru = nil
                        aLivrer = []
                    }
                }
            case "rx":
                if modeMachine, trames, ecoute { emettre(ligne) }
            case "tx":
                if modeMachine, trames { emettre(ligne) }
            case "led":
                if let m = LigneJSON.valeur(ligne, "motif") { motifLed = m.replacingOccurrences(of: "\"", with: "") }
                if modeMachine { emettre(ligne) }
            default:
                if modeMachine { emettre(avecVersion(ligne)) }
            }
        case .texte(let s):
            if modeMachine, journalLog, s.hasPrefix("[lampe] ") || s.hasPrefix("[matter] ") {
                let src = s.hasPrefix("[lampe] ") ? "lampe" : "matter"
                emettre(LigneJSON.machine("log", [("src", .texte(src)), ("niv", "notice"), ("txt", .texte(String(s.prefix(191))))]))
            } else {
                texte(s)
            }
        case .lampeDebranchee(let b):
            lampeDebranchee = b
        case .ligneCoupee(let log):
            couperProchaine = log
        case .sautN(let k):
            n &+= UInt32(k)
        }
    }

    private func executer(_ action: Action) {
        switch action {
        case .ligne(let l):
            emettre(l)
        case .texte(let s):
            texte(s)
        case .led(let motif, let avant, let test):
            motifLed = motif
            emettre(LigneJSON.machine("led", [("motif", .texte(motif)), ("avant", .texte(avant)), ("test", .booleen(test))]))
        case .livrer(let succes, let derniere, let debut):
            livrer(succes: succes, derniere: derniere, debut: debut)
        case .fermer(let raison):
            fermer(raison)
        }
    }

    private func fermer(_ raison: String) {
        guard !termine else { return }
        termine = true
        sortie.yield(.ferme(raison: raison))
        sortie.finish()
    }

    // MARK: - Emission

    private func octets(_ o: [UInt8]) {
        guard !termine else { return }
        sortie.yield(.donnees(Data(o)))
    }

    private func texte(_ s: String) {
        octets(Array(s.utf8) + [Octets.cr, Octets.lf])
    }

    /// Ligne machine : `n`, `ms`, `boot` et `up_s` reecrits, puis RS + JSON + LF.
    private func emettre(_ ligne: String) {
        let ms = msCarte()
        var l = LigneJSON.remplacerEntier(ligne, "n", valeur: Int(n))
        n &+= 1
        l = LigneJSON.remplacerEntier(l, "ms", valeur: ms)
        if boot != script.boot { l = l.replacingOccurrences(of: "\"boot\":\"\(script.boot)\"", with: "\"boot\":\"\(boot)\"") }
        if l.contains("\"up_s\":") { l = LigneJSON.remplacerEntier(l, "up_s", valeur: ms / 1000) }
        let json = Array(l.utf8)
        if let log = couperProchaine {
            // Un log IDF s'intercale entre deux paquets USB de 64 octets (2.1).
            couperProchaine = nil
            let coupe = min(63, json.count / 2)
            octets([Octets.rs] + json[0..<coupe])
            octets(Array(log.utf8) + [Octets.cr, Octets.lf])
            octets(Array(json[coupe...]) + [Octets.lf])
        } else {
            octets([Octets.rs] + json + [Octets.lf])
        }
    }

    /// Bloc periodique courant, avec les surcharges de l'app.
    private func emettreBloc(_ cle: String) {
        guard let l = ligneBloc(cle) else { return }
        emettre(l)
    }

    /// Champs jamais remis a zero par `lampe stats raz` (5.4), en plus de l'enveloppe.
    private static let horsRaz: [String: Set<String>] = [
        "compteurs.pilote": ["v", "n", "ms", "raz", "total"],
        "compteurs.radio": ["v", "n", "ms", "raz"],
    ]

    /// Ligne d'un bloc, surcharges comprises ; `apresRaz` : valeurs du dernier raz retranchees.
    private func ligneBloc(_ cle: String, apresRaz: Bool = true) -> String? {
        guard let entree = blocs[cle] else { return nil }
        var l = entree.ligne
        let age = decalage.map { msCarte() - (entree.ms + $0) } ?? 0
        switch cle {
        case "etat.lampe":
            l = LigneJSON.remplacerEntier(l, "accuse_ms") { $0 + max(0, age) }
            if let consigne, let cru {
                l = LigneJSON.remplacerValeur(l, "consigne", par: json(consigne).texteJSON)
                l = LigneJSON.remplacerValeur(l, "cru", par: json(cru).texteJSON)
                l = LigneJSON.remplacerValeur(l, "a_livrer", par: JSONValeur.tableau(aLivrer.map { .texte($0) }).texteJSON)
            }
            l = avecVersion(l)
            l = LigneJSON.remplacerEntier(l, "livrees") { $0 + ajoutsLivrees }
            l = LigneJSON.remplacerEntier(l, "abandons") { $0 + ajoutsAbandons }
            l = LigneJSON.remplacerValeur(l, "ecoute", par: ecoute ? "true" : "false")
            l = LigneJSON.remplacerEntier(l, "dernier_a") { dernierA > 0 ? dernierA : $0 }
        case "etat.sante":
            l = LigneJSON.remplacerEntier(l, "il_y_a_s") { $0 + max(0, age) / 1000 }
            if apresRaz, raz > 0 {
                var relances = 0
                l = LigneJSON.remplacerEntier(l, "relances") { v in
                    relances = max(0, v - baseRelancesSante)
                    return relances
                }
                if relances == 0 { l = LigneJSON.remplacerValeur(l, "derniere", par: "null") }
            }
        case "compteurs.pilote":
            for (champ, ajout) in ajoutsPilote { l = LigneJSON.remplacerEntier(l, champ) { $0 + ajout } }
            l = LigneJSON.remplacerEntier(l, "raz") { $0 + raz }
        case "compteurs.radio":
            l = LigneJSON.remplacerEntier(l, "raz") { $0 + raz }
        case "hello.base":
            l = LigneJSON.remplacerEntier(l, "periode_ms", valeur: periodeMs)
            l = LigneJSON.remplacerEntier(l, "compteurs_ms", valeur: compteursMs)
            l = LigneJSON.remplacerEntier(l, "reseau_ms", valeur: reseauMs)
            l = LigneJSON.remplacerEntier(l, "bail_s", valeur: bailS)
            l = LigneJSON.remplacerValeur(l, "trames", par: trames ? "true" : "false")
            l = LigneJSON.remplacerValeur(l, "log", par: journalLog ? "true" : "false")
        default:
            break
        }
        if apresRaz, let base = baseRaz[cle], let sauf = Self.horsRaz[cle] {
            l = LigneJSON.soustraire(l, base: base, sauf: sauf)
        }
        return l
    }

    private func avecVersion(_ l: String) -> String {
        decalageVersion == 0 ? l : LigneJSON.remplacerEntier(l, "version") { $0 + decalageVersion }
    }

    private func instantane(hello: Bool, etat: Bool) {
        if hello {
            for b in ["hello.base", "hello.identite", "config"] { emettreBloc(b) }
        }
        if etat {
            for b in ["etat.lampe", "etat.tranches", "etat.sante", "compteurs.pilote", "compteurs.radio",
                      "compteurs.matter", "reseau.thread", "reseau.abonnements"] { emettreBloc(b) }
        }
    }

    private func reponse(_ id: Int?, _ cmd: String, etape: String = "fin", ok: Bool = true, code: String,
                         msg: String? = nil, duree: Int = 0, suite: [(String, JSONValeur)] = []) {
        guard let id else { return }
        var champs: [(String, JSONValeur)] = [("id", .entier(id)), ("etape", .texte(etape)),
                                              ("cmd", .texte(String(cmd.prefix(40)))), ("ok", .booleen(ok)),
                                              ("code", .texte(code))]
        if let msg { champs.append(("msg", .texte(String(msg.prefix(120))))) }
        if etape == "fin" { champs.append(("duree_ms", .entier(duree))) }
        emettre(LigneJSON.machine("reponse", champs + suite))
    }

    // MARK: - Reception des lignes de l'app (2.6)

    private func recevoir(_ d: Data) {
        dernierOctet = horloge.now
        for o in d {
            switch o {
            case Octets.lf:
                let ligne = String(decoding: ligneEnCours, as: UTF8.self)
                let longueur = ligneEnCours.count
                ligneEnCours.removeAll()
                if !modeMachine { octets([Octets.cr, Octets.lf]) }
                traiter(ligne, longueur: longueur)
                if !modeMachine, !termine { octets(Array("> ".utf8)) }
            case Octets.cr:
                continue
            case Octets.ctrlU:
                ligneEnCours.removeAll()
            case 0x08, 0x7F:
                if !ligneEnCours.isEmpty { ligneEnCours.removeLast() }
            case 0x20...0x7E:
                ligneEnCours.append(o)
                if !modeMachine { octets([o]) }  // echo en mode humain seulement (3.8)
            default:
                continue  // hors 0x20..0x7E : ignore
            }
        }
    }

    private func traiter(_ brute: String, longueur: Int) {
        let ligne = brute.trimmingCharacters(in: .whitespaces)
        guard !ligne.isEmpty else { return }
        var id: Int?
        var cmd = ligne
        if ligne.hasPrefix("id=") {
            let reste = ligne.dropFirst(3)
            let chiffres = reste.prefix { $0.isNumber }
            id = Int(chiffres)
            cmd = reste.dropFirst(chiffres.count).trimmingCharacters(in: .whitespaces)
        }
        if longueur > LigneCommande.octetsMax {
            reponse(id, cmd, ok: false, code: "trop_long", msg: "ligne de plus de 127 octets : rien d'execute")
            return
        }
        let mots = LigneCommande.mots(cmd)
        switch mots.first {
        case "json":
            commandeJson(id, cmd, mots)
        case "lampe" where id != nil && Self.lampeAsynchrone(mots):
            commandeLampe(id ?? 0, cmd, mots)
        default:
            historique(id, cmd, mots)
        }
    }

    // MARK: - Famille json (3.4)

    private func commandeJson(_ id: Int?, _ cmd: String, _ m: [String]) {
        func usage(_ msg: String) { reponse(id, cmd, ok: false, code: "usage", msg: msg) }
        func entier(_ i: Int) -> Int? { i < m.count ? Int(m[i]) : nil }
        let sousCommande = m.count > 1 ? m[1] : ""
        switch sousCommande {
        case "1":
            var bail = 30
            if m.count >= 4, m[2] == "bail" {
                guard let b = entier(3), b == 0 || (10...600).contains(b) else { return usage("bail : 0 ou 10..600 s") }
                bail = b
            }
            modeMachine = true
            periodeMs = 1000
            compteursMs = 1000
            reseauMs = 5000
            bailS = bail
            trames = true
            journalLog = false
            if decalage == nil { decalage = msCarte() - script.msDebut }
            instantane(hello: true, etat: true)
            let ms = msCarte()
            prochainEtat = ms + periodeMs
            prochainCompteurs = ms + compteursMs
            prochainReseau = ms + reseauMs
            prochainHb = ms + 2000
            reponse(id, cmd, code: "ok", duree: 12, suite: [("bail_s", .entier(bailS)), ("up_s", .entier(ms / 1000))])
        case "0":
            reponse(id, cmd, code: "ok")
            if modeMachine { emettre(LigneJSON.machine("fin", [("cause", "commande")])) }
            modeMachine = false
        case "etat":
            instantane(hello: false, etat: true)
            reponse(id, cmd, code: "ok", duree: 9)
        case "hello":
            instantane(hello: true, etat: false)
            reponse(id, cmd, code: "ok", duree: 4)
        case "ping":
            reponse(id, cmd, code: "ok", suite: [("bail_s", .entier(bailS)), ("up_s", .entier(msCarte() / 1000))])
        case "periode", "compteurs", "reseau":
            let min = sousCommande == "reseau" ? 1000 : 200
            guard m.count == 3, let v = entier(2), v == 0 || (min...60000).contains(v) else {
                return usage("\(sousCommande) : 0 ou \(min)..60000 ms")
            }
            switch sousCommande {
            case "periode": periodeMs = v; prochainEtat = msCarte() + v
            case "compteurs": compteursMs = v; prochainCompteurs = msCarte() + v
            default: reseauMs = v; prochainReseau = msCarte() + v
            }
            reponse(id, cmd, code: "ok")
        case "trames", "log":
            guard m.count == 3, m[2] == "0" || m[2] == "1" else { return usage("\(sousCommande) 0|1") }
            if sousCommande == "trames" { trames = m[2] == "1" } else { journalLog = m[2] == "1" }
            reponse(id, cmd, code: "ok")
        case "":
            texte("json : mode \(modeMachine ? "machine" : "humain") (usb), periode \(periodeMs) ms, "
                  + "compteurs \(compteursMs) ms, reseau \(reseauMs) ms, bail \(bailS) s")
            reponse(id, cmd, code: "ok")
        case "cle":
            reponse(id, cmd, ok: false, code: "refuse", msg: "(demo) pas de transport reseau ni de cle")
        default:
            usage("json [1|0|etat|hello|ping|periode|compteurs|reseau|trames|log|cle]")
        }
    }

    // MARK: - Commandes lampe asynchrones (6.2)

    private static func lampeAsynchrone(_ m: [String]) -> Bool {
        guard m.count >= 2 else { return false }
        return ["on", "off", "avant", "arriere", "mode", "lum", "niveau", "temp", "mired", "auto", "sync"].contains(m[1])
    }

    private func lampeFichier(_ champ: String) -> Lampe {
        let defaut = Lampe(marche: true, lampes: "deux", lum: 0xA5, temp: 53)
        guard let bloc = blocs["etat.lampe"]?.ligne, let v = LigneJSON.valeur(bloc, champ),
              let o = try? JSONSerialization.jsonObject(with: Data(v.utf8)) as? [String: Any] else { return defaut }
        return Lampe(marche: o["marche"] as? Bool ?? defaut.marche, lampes: o["lampes"] as? String ?? defaut.lampes,
                     lum: o["lum"] as? Int ?? defaut.lum, temp: o["temp"] as? Int ?? defaut.temp)
    }

    private func json(_ l: Lampe) -> JSONValeur {
        .objet([("marche", .booleen(l.marche)), ("lampes", .texte(l.lampes)), ("lum", .entier(l.lum)),
                ("niveau", .entier(correspondance.niveau(brut: l.lum))), ("temp", .entier(l.temp)),
                ("mired", .entier(CorrespondanceLuminosite.mired(temp: l.temp)))])
    }

    private func versionCourante() -> Int {
        let v = blocs["etat.lampe"].flatMap { LigneJSON.valeur($0.ligne, "version") }.flatMap { Int($0) } ?? 0
        return v + decalageVersion
    }

    private func commandeLampe(_ id: Int, _ cmd: String, _ m: [String]) {
        func usage(_ msg: String) { reponse(id, cmd, ok: false, code: "usage", msg: msg) }
        func valeur() -> String? { m.count >= 3 ? m[2] : nil }
        var c = consigne ?? lampeFichier("consigne")
        let base = cru ?? lampeFichier("cru")
        var champs: [String] = []
        var appuiA = false

        switch m[1] {
        case "on", "off":
            c.marche = m[1] == "on"
            champs = ["marche"]
        case "avant", "arriere":
            guard let v = valeur(), v == "on" || v == "off" else { return usage("lampe \(m[1]) on|off") }
            var avant = c.marche && (c.lampes == "avant" || c.lampes == "deux")
            var arriere = c.marche && (c.lampes == "arriere" || c.lampes == "deux")
            if m[1] == "avant" { avant = v == "on" } else { arriere = v == "on" }
            if avant || arriere {
                c.marche = true
                c.lampes = avant && arriere ? "deux" : (avant ? "avant" : "arriere")
            } else {
                c.marche = false
            }
            champs = ["marche"]
        case "mode":
            guard let v = valeur(), ["avant", "arriere", "deux"].contains(v) else { return usage("lampe mode avant|arriere|deux") }
            c.marche = true
            c.lampes = v
            champs = ["marche"]
        case "niveau":
            guard let v = valeur().flatMap({ Int($0) }), (1...254).contains(v) else { return usage("niveau : 1..254") }
            c.lum = correspondance.brut(niveau: v)
            champs = ["lum"]
        case "lum":
            guard let v = valeur().flatMap({ Int($0, radix: 16) }), (0x4C...0xFE).contains(v) else {
                return usage("lum : 4C..FE, en hexa")
            }
            c.lum = v
            champs = ["lum"]
        case "temp":
            guard let v = valeur().flatMap({ Int($0) }), (0...100).contains(v) else { return usage("temp : 0..100, en decimal") }
            c.temp = v
            champs = ["temp"]
        case "mired":
            guard let v = valeur().flatMap({ Int($0) }), (153...370).contains(v) else { return usage("mired : 153..370") }
            c.temp = CorrespondanceLuminosite.temp(mired: v)
            champs = ["temp"]
        case "auto":
            guard c.marche else {
                return reponse(id, cmd, ok: false, code: "refuse", msg: "lampe eteinte : A n'est pas emis")
            }
            appuiA = true
        case "sync":
            champs = ["marche", "lum", "temp"]
        default:
            return usage("lampe ...")
        }

        consigne = c
        cru = base
        decalageVersion += 1
        let version = versionCourante()

        if !c.marche, !champs.contains("marche"), !appuiA {
            // Lampe eteinte : la valeur partira a l'allumage (6.2, differe).
            for ch in champs where !aLivrer.contains(ch) { aLivrer.append(ch) }
            aLivrer = ["marche", "lum", "temp"].filter { aLivrer.contains($0) }
            reponse(id, cmd, code: "differe", duree: 1, suite: [
                ("suite", "aucune"), ("consigne", json(c)),
                ("a_livrer", .tableau(aLivrer.map { .texte($0) })), ("version", .entier(version))])
            return
        }

        var dus = Set(aLivrer).union(champs)
        if c.marche, dus.contains("marche") { dus.insert("lum") }  // A4 (a) : l'allumage emporte la luminosite
        aLivrer = ["marche", "lum", "temp"].filter { dus.contains($0) }
        idsEnAttente.append(id)
        if idsEnAttente.count > 8 {
            idsEnAttente.removeFirst()
            idsPerdus += 1
        }
        reponse(id, cmd, code: "accepte", duree: 1, suite: [
            ("suite", "livraison"), ("consigne", json(c)),
            ("a_livrer", .tableau(aLivrer.map { .texte($0) })), ("version", .entier(version))])

        var tranches: [String] = []
        if appuiA { tranches.append("a") }
        if dus.contains("marche") || dus.contains("lum") { tranches.append("lum") }
        if dus.contains("temp") { tranches.append("temp") }
        planifierRafale(tranches, consigne: c)
    }

    private func charge(_ tranche: String, _ c: Lampe) -> String {
        let bits = ["avant": 0x40, "arriere": 0x01, "deux": 0x41][c.lampes] ?? 0x41
        let f = (c.marche ? 0x80 : 0) | bits
        switch tranche {
        case "a":
            dernierA = dernierA >= 255 ? 1 : dernierA + 1
            return String(format: "%02X%02X", f | 0x20, dernierA)
        case "temp":
            return String(format: "%02X%02X", f | 0x02, c.temp)
        default:
            return String(format: "%02X%02X", f | 0x04, c.lum)
        }
    }

    /// Rafale d'une consigne : une nouvelle preempte la precedente (la carte fond les consignes).
    private func planifierRafale(_ tranches: [String], consigne c: Lampe) {
        programme.removeAll { $0.groupe == groupeRafale }
        groupeRafale += 1
        let g = groupeRafale
        let debut = msCarte()
        var t = debut + 2
        var derniere = "lum"
        func tx(_ tranche: String, _ chargeTx: String, _ essai: Int, _ accuses: Int, echec: Bool) {
            let champs: [(String, JSONValeur)] = [
                ("num", .entier(numeroTx)), ("tranche", .texte(tranche)), ("charge", .texte(chargeTx)),
                ("essai", .entier(essai)), ("paquets", 3), ("accuses", .entier(accuses)),
                ("verdict", .texte(echec ? "max_rt" : "ack")),
                ("us", .entier(echec ? Int.random(in: 11462...11481) : Int.random(in: 1600...1720))),
                ("rt2", .texte(echec ? "10" : "00")), ("irq1", .texte(echec ? "1E" : "2E")),
                ("status", .texte(echec ? "01" : "11"))]
            numeroTx += 1
            ajoutsPilote["paquets", default: 0] += 1
            ajoutsPilote["total", default: 0] += 1
            ajoutsPilote[echec ? "max_rt" : "accuses", default: 0] += 1
            if trames { programme.append(Programme(du: t, groupe: g, action: .ligne(LigneJSON.machine("tx", champs)))) }
        }
        ajoutsPilote["consignes", default: 0] += 1

        if lampeDebranchee {
            // 12.4 : 5 paquets en MAX_RT par tour, 3 tours (reprises a +1 s et +2 s), puis abandon.
            let tranche = tranches.first ?? "lum"
            let chargeTx = charge(tranche, c)
            for tour in 0..<3 {
                for essai in 1...5 {
                    tx(tranche, chargeTx, essai, 0, echec: true)
                    t += 100
                }
                if tour < 2 { t += 1000 * (tour + 1) }
            }
            t += 40
            ajoutsPilote["abandons", default: 0] += 1
            programme.append(Programme(du: t, groupe: g, action: .livrer(succes: false, derniere: tranche, debut: debut)))
            programme.append(Programme(du: t + 1, groupe: g, action: .led("injoignable", avant: "operationnel", test: false)))
            programme.append(Programme(du: t + 1, groupe: g, action: .texte("[lampe] injoignable : consigne abandonnee")))
            programme.append(Programme(du: t + 1201, groupe: g, action: .led("operationnel", avant: "injoignable", test: false)))
        } else {
            // 12.2 : 3 paquets accuses par tranche, 100 ms d'ecart, puis la livraison.
            for tranche in tranches {
                let chargeTx = charge(tranche, c)
                for essai in 1...3 {
                    tx(tranche, chargeTx, essai, essai, echec: false)
                    t += 100
                }
                derniere = tranche
            }
            t -= 98
            programme.append(Programme(du: t, groupe: g, action: .livrer(succes: true, derniere: derniere, debut: debut)))
            programme.append(Programme(du: t + 1, groupe: g, action: .led("livree", avant: "operationnel", test: false)))
            programme.append(Programme(du: t + 151, groupe: g, action: .led("operationnel", avant: "livree", test: false)))
        }
    }

    private func livrer(succes: Bool, derniere: String, debut: Int) {
        guard var c = consigne, var r = cru else { return }
        var champs: [(String, JSONValeur)]
        if succes {
            r = c
            ajoutsLivrees += 1
            champs = [("issue", "livree"), ("derniere", .texte(derniere))]
        } else {
            c = r
            decalageVersion += 1
            ajoutsAbandons += 1
            champs = [("issue", "abandon"), ("cause", "injoignable")]
        }
        consigne = c
        cru = r
        aLivrer = []
        let livrees = (blocs["etat.lampe"].flatMap { LigneJSON.valeur($0.ligne, "livrees") }.flatMap { Int($0) } ?? 0) + ajoutsLivrees
        let abandons = (blocs["etat.lampe"].flatMap { LigneJSON.valeur($0.ligne, "abandons") }.flatMap { Int($0) } ?? 0) + ajoutsAbandons
        champs += [("version", .entier(versionCourante())), ("consigne", json(c)), ("cru", json(r)), ("a_livrer", .tableau([])),
                   ("ids", .tableau(idsEnAttente.map { .entier($0) })), ("ids_perdus", .entier(idsPerdus)),
                   ("attente_ms", .entier(msCarte() - debut)), ("livrees", .entier(livrees)), ("abandons", .entier(abandons))]
        idsEnAttente = []
        idsPerdus = 0
        emettre(LigneJSON.machine("livraison", champs))
    }

    // MARK: - Commandes historiques (texte humain encadre par reponse debut / fin)

    private static let connues: Set<String> = [
        "help", "lampe", "led", "matter", "reboot", "decommission", "info", "chiplog", "thread", "stats", "erase",
        "wifi", "calib", "rfinit", "regs", "txack", "ecoute", "prxack", "sniffspi", "addr", "chan", "xo", "debit",
        "amble", "aw", "holtek", "regcfg", "etalon",
    ]

    private func historique(_ id: Int?, _ cmd: String, _ m: [String]) {
        guard let premier = m.first, Self.connues.contains(premier) else {
            texte("Commande inconnue : \"\(cmd)\". Tape 'help'.")
            reponse(id, cmd, ok: false, code: "inconnue", msg: "commande inconnue")
            return
        }
        reponse(id, cmd, etape: "debut", code: "en_cours")
        var duree = 3
        switch (premier, m.count > 1 ? m[1] : "") {
        case ("help", _):
            for l in ["Commandes (demo) :", "  lampe [on|off|avant|arriere|mode|niveau|lum|temp|mired|auto|sync|stats|ecoute]",
                      "  led test|stop", "  json 1|0|etat|hello|ping|periode|compteurs|reseau|trames|log",
                      "  matter, reboot, help"] { texte(l) }
        case ("lampe", "stats") where m.count >= 3 && m[2] == "raz":
            // Les compteurs du fichier restent cumulatifs : retenir leurs valeurs pour les retrancher.
            for b in ["compteurs.pilote", "compteurs.radio"] {
                baseRaz[b] = ligneBloc(b, apresRaz: false).map(LigneJSON.entiers)
            }
            if let s = ligneBloc("etat.sante", apresRaz: false), let v = LigneJSON.valeur(s, "relances").flatMap({ Int($0) }) {
                baseRelancesSante = v
            }
            raz += 1
            texte("  statistiques du pilote et de la radio remises a zero (raz \(raz))")
        case ("lampe", "stats"):
            texte("  emission : consignes, paquets, accuses : voir compteurs.pilote (demo)")
            texte("  reception : trames de la telecommande et accuses de la lampe (demo)")
            texte("  ... (texte de Halo1Lamp::printStats)")
            duree = 7
        case ("lampe", "ecoute"):
            if m.count >= 3 { ecoute = m[2] == "1" }
            texte("  ecoute de fond : \(ecoute ? "active" : "coupee")")
        case ("lampe", ""):
            let c = consigne ?? lampeFichier("consigne")
            texte("  consigne    : \(c.marche ? "allumee" : "eteinte") \(c.lampes) lum \(String(format: "%02X", c.lum)) temp \(c.temp)")
            texte("  radio       : BM5602 present, ecoute (demo)")
        case ("led", "test"):
            texte("led : sequence de test (7 motifs)")
            let motifs = ["identification", "injoignable", "panne_radio", "livree", "non_appaire", "hors_reseau", "operationnel"]
            var t = msCarte() + 50
            var avant = motifLed
            for motif in motifs {
                programme.append(Programme(du: t, groupe: -1, action: .led(motif, avant: avant, test: true)))
                avant = motif
                t += 1500
            }
            programme.append(Programme(du: t, groupe: -1, action: .led("operationnel", avant: avant, test: false)))
        case ("led", "stop"):
            programme.removeAll { $0.groupe == -1 }
            texte("led : test arrete")
            emettre(LigneJSON.machine("led", [("motif", "operationnel"), ("avant", .texte(motifLed)), ("test", false)]))
            motifLed = "operationnel"
        case ("reboot", _):
            texte("Redemarrage...")
            programme.append(Programme(du: msCarte() + 300, groupe: -2, action: .fermer("La carte redémarre (démo) : ré-énumération USB")))
            return  // la carte redemarre avant la fin
        case ("matter", _):
            texte("  Matter : en service, connecte (Thread, child), 1 fabrique (demo)")
        default:
            texte("(demo) \"\(cmd)\" : pas de carte reelle, rien n'est execute.")
        }
        reponse(id, cmd, code: "execute", duree: duree)
    }
}
