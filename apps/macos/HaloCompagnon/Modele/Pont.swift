import AppKit
import Foundation
import HaloProtocole
import Observation

/// Modele central de l'app : un transport, le recepteur, le moteur de session
/// et tout ce que les ecrans affichent. Tout se passe sur l'acteur principal ;
/// la couche protocole (HaloProtocole) est du code pur appele d'ici.
@MainActor
@Observable
final class Pont {
    enum Source: Hashable, Sendable {
        case serie(chemin: String, serie: String?)
        case demo

        var estDemo: Bool { self == .demo }
    }

    enum EtatTransport: Equatable, Sendable {
        case ferme
        case ouverture
        case ouvert
        /// Reouverture programmee (re-enumeration USB, silence...).
        case attente(prochain: Date, raison: String)
        /// "Liberer le port" : rien ne se rouvre avant un clic.
        case libere
        case erreur(String)
    }

    enum ResultatConsole: Equatable {
        case envoyee
        case confirmation(String)
        case refusee(String)
    }

    // MARK: Etat publie

    private(set) var source: Source?
    private(set) var ports: [PortUSB] = []
    private(set) var etatTransport: EtatTransport = .ferme
    private(set) var nomTransport = ""
    private(set) var phase: MoteurSession.Phase = .ferme
    private(set) var etat = EtatPont()
    private(set) var reception = CompteursReception()
    private(set) var statistiques = StatistiquesLien()
    private(set) var suivis: [SuiviCommande] = []
    private(set) var trames = Borne<EntreeTrame>(capacite: 5000)
    private(set) var console = Borne<LigneConsole>(capacite: 4000)
    private(set) var rejets = Borne<Rejet>(capacite: 100)
    private(set) var pilote = Borne<Echantillon>(capacite: 7200)
    private(set) var radio = Borne<Echantillon>(capacite: 7200)
    private(set) var abonnes = Borne<PointMesure>(capacite: 3000)
    private(set) var rssi = Borne<PointMesure>(capacite: 3000)
    private(set) var marqueurs = Borne<Marqueur>(capacite: 500)
    /// Annonce grave (ancien firmware, aucune reponse...) montree en bandeau.
    private(set) var alerte: String?
    private(set) var derniereReception: Date?
    /// Commande de banc de plus de 20 min : proposer de fermer le port (6.5).
    var propositionFermeture = false
    /// Flux `rx`/`tx` coupe par `json trames 0`.
    private(set) var tramesCoupees = false
    /// Facteur de temps du mode demo (1 : temps reel ; les tests accelerent).
    @ObservationIgnored var vitesseDemo: Double = 1

    // MARK: Interne

    @ObservationIgnored private var moteur = MoteurSession()
    @ObservationIgnored private var recepteur = RecepteurLignes()
    @ObservationIgnored private var transport: (any Transport)?
    @ObservationIgnored private var demo: TransportDemo?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var tacheLecture: Task<Void, Never>?
    @ObservationIgnored private var tacheReconnexion: Task<Void, Never>?
    @ObservationIgnored private var tacheTic: Task<Void, Never>?
    @ObservationIgnored private var reconnexionAuto = false
    @ObservationIgnored private var essaisReconnexion = 0
    @ObservationIgnored private var compteur = 0
    @ObservationIgnored private var dernierCurseur: [String: TimeInterval] = [:]
    @ObservationIgnored private let origine = ContinuousClock.now
    @ObservationIgnored private let surveillant = SurveillantUSB()
    @ObservationIgnored private var observateurReveil: (any NSObjectProtocol)?

    /// Delais de reouverture apres une fermeture : 300 ms, puis 1 s, 2 s, 5 s (3.1).
    static let delaisReconnexion: [Double] = [0.3, 1, 2, 5]

    init() {
        ports = SurveillantUSB.lister()
        surveillant.changement = { [weak self] ports in self?.portsChanges(ports) }
        surveillant.demarrer()
        observateurReveil = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                // Pause de lecture : la premiere ligne lue ensuite peut etre un fragment (2.4).
                self?.recepteur.signalerPause()
            }
        }
        tacheTic = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(250))
                guard let self else { return }
                self.tic()
            }
        }
    }

    private func maintenant() -> TimeInterval {
        (ContinuousClock.now - origine) / .seconds(1)
    }

    private func prochainId() -> Int {
        compteur += 1
        return compteur
    }

    // MARK: - Connexion

    /// Source proposee par defaut : le premier port Espressif, sinon la demo.
    var sourceParDefaut: Source {
        if let p = ports.first(where: \.estEspressif) { return .serie(chemin: p.chemin, serie: p.serie) }
        return .demo
    }

    func connecter(_ s: Source) {
        fermerTransport()
        source = s
        alerte = nil
        reconnexionAuto = true
        essaisReconnexion = 0
        ouvrir()
    }

    func deconnecter() {
        reconnexionAuto = false
        tacheReconnexion?.cancel()
        fermerTransport()
        etatTransport = .ferme
    }

    /// "Liberer le port" (3.1, etape 7) : `json 0`, fermeture, pas de reouverture avant un clic.
    func libererPort() {
        reconnexionAuto = false
        tacheReconnexion?.cancel()
        executer(moteur.liberer(maintenant: maintenant()))
        let t = transport
        transport = nil
        generation += 1
        tacheLecture?.cancel()
        // Laisser partir json 0 avant de fermer ; DTR et RTS restent a 0.
        Task {
            try? await Task.sleep(for: .milliseconds(150))
            t?.fermer()
        }
        synchroniser()
        etatTransport = .libere
        note("Port libéré : json 0 envoyé, port fermé. Flasher est possible ; « Reconnecter » pour reprendre.", grave: false)
    }

    func reconnecter() {
        guard source != nil else { return }
        reconnexionAuto = true
        essaisReconnexion = 0
        alerte = nil
        ouvrir()
    }

    /// Nouvel essai de `json 1` (apres un flash de l'ancien firmware, par exemple).
    func reessayer() {
        alerte = nil
        executer(moteur.reessayer(maintenant: maintenant()))
    }

    private func ouvrir() {
        guard let source else { return }
        tacheReconnexion?.cancel()
        fermerTransport()
        let t: any Transport
        switch source {
        case .demo:
            do {
                if demo == nil { demo = try TransportDemo(vitesse: vitesseDemo) }
                t = demo!
            } catch {
                etatTransport = .erreur(String(describing: error))
                return
            }
        case .serie(let chemin, let serie):
            // La meme carte (meme numero de serie USB) peut revenir sous un autre nom.
            let port = ports.first { serie != nil && $0.serie == serie } ?? ports.first { $0.chemin == chemin }
            t = TransportSerie(chemin: port?.chemin ?? chemin)
        }
        transport = t
        nomTransport = t.nom
        etatTransport = .ouverture
        generation += 1
        let g = generation
        tacheLecture = Task { [weak self] in
            do {
                let flux = try await t.ouvrir()
                guard let self, self.generation == g else {
                    t.fermer()
                    return
                }
                self.transportOuvert()
                var raison = "flux terminé"
                for await ev in flux {
                    guard self.generation == g else { break }
                    if case .ferme(let r) = ev { raison = r }
                    self.recevoir(ev)
                }
                if self.generation == g { self.transportFerme(raison) }
            } catch {
                guard let self, self.generation == g else { return }
                self.echecOuverture(error)
            }
        }
    }

    private func fermerTransport() {
        generation += 1
        tacheLecture?.cancel()
        tacheLecture = nil
        transport?.fermer()
        transport = nil
        if moteur.phase != .ferme {
            moteur.ferme(maintenant: maintenant())
            synchroniser()
        }
    }

    private func transportOuvert() {
        etatTransport = .ouvert
        recepteur.resynchroniser()
        note("Port ouvert : \(nomTransport) (DTR = RTS = 0).", grave: false)
        executer(moteur.ouvert(maintenant: maintenant()))
    }

    private func transportFerme(_ raison: String) {
        transport = nil
        moteur.ferme(maintenant: maintenant())
        synchroniser()
        note("Transport fermé : \(raison)", grave: false)
        if reconnexionAuto { planifierReconnexion(raison) } else { etatTransport = .ferme }
    }

    private func echecOuverture(_ erreur: any Error) {
        transport = nil
        let texte = String(describing: erreur)
        if reconnexionAuto, essaisReconnexion < 40 {
            planifierReconnexion(texte)
        } else {
            etatTransport = .erreur(texte)
        }
    }

    private func planifierReconnexion(_ raison: String) {
        let delai = Self.delaisReconnexion[min(essaisReconnexion, Self.delaisReconnexion.count - 1)]
        essaisReconnexion += 1
        etatTransport = .attente(prochain: Date().addingTimeInterval(delai), raison: raison)
        tacheReconnexion?.cancel()
        tacheReconnexion = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delai))
            guard !Task.isCancelled else { return }
            self?.ouvrir()
        }
    }

    /// Arrivee ou depart d'un port (IOKit) : on rouvre des que la carte revient.
    private func portsChanges(_ nouveaux: [PortUSB]) {
        ports = nouveaux
        guard case .attente = etatTransport, case .serie(let chemin, let serie)? = source else { return }
        let revenu = nouveaux.contains { ($0.serie != nil && $0.serie == serie) || $0.chemin == chemin }
        if revenu {
            essaisReconnexion = 0
            planifierReconnexion("port revenu")
        }
    }

    // MARK: - Boucle

    private func tic() {
        guard moteur.phase != .ferme else { return }
        executer(moteur.tic(maintenant: maintenant()))
    }

    private func recevoir(_ ev: EvenementTransport) {
        guard case .donnees(let d) = ev else { return }
        derniereReception = Date()
        for element in recepteur.alimenter(d) { traiter(element) }
        synchroniser()
    }

    private func traiter(_ element: ElementRecu) {
        let effets = moteur.recu(element, maintenant: maintenant())
        let historique = moteur.historique
        // Un redemarrage vide les etats derives AVANT d'appliquer la ligne qui l'a revele
        // (le hello du nouveau demarrage doit rester).
        let (redemarrages, autres) = effets.reduce(into: ([MoteurSession.Effet](), [MoteurSession.Effet]())) { r, e in
            if case .redemarrage = e { r.0.append(e) } else { r.1.append(e) }
        }
        executer(redemarrages)
        switch element {
        case .machine(let l):
            traiterMachine(l, historique: historique)
        case .texte(let t):
            if t.classe != .invite {
                ajouterConsole(.texte(t.classe), t.texte, numero: moteur.correlateur.commandeDeBanc?.numero)
            }
        case .fragment(let s):
            ajouterConsole(.fragment, s)
        case .abimee(let raison, let brut):
            rejets.ajouter(Rejet(id: prochainId(), date: Date(), raison: "abîmée : \(raison)", brut: brut))
        case .versionInconnue(let v, let t):
            rejets.ajouter(Rejet(id: prochainId(), date: Date(), raison: "version \(v) inconnue", brut: t))
        case .invalide(let t, let raison):
            rejets.ajouter(Rejet(id: prochainId(), date: Date(), raison: "\(t) invalide : \(raison)", brut: ""))
        case .debordement(let s):
            ajouterConsole(.texte(.commande), s)
        }
        executer(autres)
    }

    private func traiterMachine(_ l: LigneMachine, historique: Bool) {
        let recueA = Date()
        let date = historique ? etat.dater(ms: l.enveloppe.ms, recueA: recueA) : etat.appliquer(l, recueA: recueA)
        let c = etat.correspondance
        switch l.message {
        case .compteursPilote(let v) where !historique:
            pilote.ajouter(.pilote(v, date: date, boot: etat.boot))
        case .compteursRadio(let v) where !historique:
            radio.ajouter(.radio(v, date: date, boot: etat.boot))
        case .reseauThread(let v) where !historique:
            rssi.ajouter(PointMesure(id: prochainId(), date: date, valeur: v.thread?.parentRssi.map(Double.init)))
        case .reseauAbonnements(let v) where !historique:
            abonnes.ajouter(PointMesure(id: prochainId(), date: date, valeur: v.abonnements?.actifs.map(Double.init)))
        case .relance(let r):
            marqueurs.ajouter(Marqueur(id: prochainId(), date: date, genre: .relance, texte: r.cause.libelle))
        case .module(let m):
            marqueurs.ajouter(Marqueur(id: prochainId(), date: date, genre: .module, texte: m.etat.libelle))
        case .thread(let t):
            marqueurs.ajouter(Marqueur(id: prochainId(), date: date, genre: .role, texte: "\(t.de ?? "?") → \(t.vers)"))
        case .livraison(let liv):
            if liv.issue == .abandon {
                marqueurs.ajouter(Marqueur(id: prochainId(), date: date, genre: .abandon,
                                           texte: liv.cause?.libelle ?? "abandon"))
            }
            for id in liv.ids ?? [] {
                guard let s = moteur.correlateur.suivi(numero: id) else { continue }
                ajouterConsole(.retour(ok: liv.issue == .livree, session: s.origine == .session),
                               "‹ id=\(id) « \(s.commande) » : " + Interpretation.livraison(liv), numero: id)
            }
        case .reponse(let r):
            if let s = moteur.correlateur.suivi(numero: r.id) {
                ajouterConsole(.retour(ok: r.ok, session: s.origine == .session),
                               "‹ " + PolitiqueCommandes.masquerCle(Interpretation.reponse(r)), numero: r.id)
            } else {
                ajouterConsole(.retour(ok: r.ok, session: true), "‹ " + Interpretation.reponse(r), numero: r.id)
            }
        case .log(let lg):
            ajouterConsole(.log, lg.txt)
        default:
            break
        }
        if !l.message.estPeriodique {
            trames.ajouter(EntreeTrame(id: prochainId(), date: date, n: l.enveloppe.n, ms: l.enveloppe.ms,
                                       type: l.enveloppe.t, categorie: .de(l.message), message: l.message,
                                       json: PolitiqueCommandes.masquerCle(l.json), historique: historique,
                                       resume: Interpretation.resume(l.message, correspondance: c)))
        }
    }

    private func executer(_ effets: [MoteurSession.Effet]) {
        for e in effets {
            switch e {
            case .envoyer(let d):
                do {
                    try transport?.envoyer(d)
                } catch {
                    note("Envoi impossible : \(error)", grave: false)
                }
                journaliserEnvoi(d)
            case .rouvrir(let raison):
                note(raison, grave: false)
                transport?.fermer()
            case .redemarrage(let ancien, let nouveau):
                etat.viderDerives()
                marqueurs.ajouter(Marqueur(id: prochainId(), date: Date(), genre: .redemarrage,
                                           texte: "boot \(nouveau ?? "?")"))
                note("Redémarrage de la carte détecté (boot \(ancien ?? "?") → \(nouveau ?? "?")) : "
                     + "états vidés, nouveau segment de courbes.", grave: false)
            case .note(let texte, let grave):
                note(texte, grave: grave)
            case .commandeSansReponse(let id):
                if let s = moteur.correlateur.suivi(id) {
                    ajouterConsole(.retour(ok: false, session: s.origine == .session),
                                   "‹ id=\(s.numero ?? 0) « \(s.commande) » : sans réponse sous 3 s (pas de réémission)",
                                   numero: s.numero)
                }
            case .proposerFermeture:
                propositionFermeture = true
            }
        }
        synchroniser()
    }

    private func journaliserEnvoi(_ d: Data) {
        let texte = String(decoding: d, as: UTF8.self).trimmingCharacters(in: .newlines)
        guard !texte.isEmpty, texte != "\u{15}" else { return }
        var numero: Int?
        var origine = OrigineCommande.session
        if texte.hasPrefix("id="), let espace = texte.firstIndex(of: " ") {
            numero = Int(texte[texte.index(texte.startIndex, offsetBy: 3)..<espace])
            if let n = numero, let s = moteur.correlateur.suivi(numero: n) { origine = s.origine }
        } else {
            origine = .console
        }
        ajouterConsole(.envoi(origine), "› " + PolitiqueCommandes.masquerCle(texte), numero: numero)
    }

    private func synchroniser() {
        if phase != moteur.phase {
            phase = moteur.phase
            if phase == .connecte { essaisReconnexion = 0 }
        }
        if suivis != moteur.correlateur.suivis { suivis = moteur.correlateur.suivis }
        if statistiques != moteur.statistiques { statistiques = moteur.statistiques }
        if reception != recepteur.compteurs { reception = recepteur.compteurs }
    }

    private func ajouterConsole(_ genre: LigneConsole.Genre, _ texte: String, numero: Int? = nil) {
        console.ajouter(LigneConsole(id: prochainId(), date: Date(), genre: genre, texte: texte, numero: numero))
    }

    private func note(_ texte: String, grave: Bool) {
        ajouterConsole(.note(grave: grave), texte)
        if grave { alerte = texte }
    }

    // MARK: - Commandes

    var peutCommander: Bool { phase.modeMachine && transport != nil }

    /// Commande d'un bouton ou d'un curseur, avec `id` et correlation.
    @discardableResult
    func envoyer(_ commande: String, fusion: String? = nil) -> UUID? {
        guard peutCommander else {
            note("Pas de session machine : « \(commande) » n'est pas envoyée.", grave: false)
            return nil
        }
        let (id, effets) = moteur.soumettre(commande, origine: .interface, fusion: fusion, maintenant: maintenant())
        executer(effets)
        return id
    }

    /// Curseurs : une commande toutes les 150 ms au plus pendant le glissement,
    /// toujours la valeur finale au relachement (6.4).
    func curseur(_ commande: String, cle: String, fini: Bool) {
        let t = maintenant()
        guard fini || t - (dernierCurseur[cle] ?? -1) >= 0.15 else { return }
        dernierCurseur[cle] = t
        envoyer(commande, fusion: cle)
    }

    /// Console brute : regles de 2.6, confirmations et interdits de 6.4.
    func console(_ ligne: String, confirme: Bool = false) -> ResultatConsole {
        let genre = transport?.genre ?? .usb
        switch PolitiqueCommandes.verdictConsole(ligne, transport: genre) {
        case .interdite(let raison):
            return .refusee(raison)
        case .confirmation(let raison) where !confirme:
            return .confirmation(raison)
        default:
            break
        }
        guard let transport else { return .refusee("Aucune carte connectée.") }
        let propre = ligne.trimmingCharacters(in: .whitespaces)
        if moteur.phase.modeMachine {
            let (_, effets) = moteur.soumettre(propre, origine: .console, maintenant: maintenant())
            executer(effets)
        } else {
            // Ancien firmware, mode humain... : ligne brute, sans id.
            switch moteur.ligneBrute(propre) {
            case .success(let d):
                do { try transport.envoyer(d) } catch { return .refusee("Envoi impossible : \(error)") }
                journaliserEnvoi(d)
            case .failure(let e):
                return .refusee(e.description)
            }
        }
        if PolitiqueCommandes.attendReenumeration(propre) {
            note("Attente de la ré-énumération USB (la carte redémarre).", grave: false)
        }
        return .envoyee
    }

    func rafraichir() { envoyer("json etat") }

    func couperTrames(_ coupees: Bool) {
        if envoyer("json trames \(coupees ? 0 : 1)") != nil { tramesCoupees = coupees }
    }

    func viderJournal() {
        trames.vider()
    }

    func viderConsole() {
        console.vider()
    }

    func viderCourbes() {
        pilote.vider()
        radio.vider()
        abonnes.vider()
        rssi.vider()
        marqueurs.vider()
    }

    // MARK: - Lectures pour les ecrans

    var estDemo: Bool { source?.estDemo ?? false }

    /// Commande de banc en cours (`reponse debut` recue, pas de `fin`).
    var commandeDeBanc: SuiviCommande? {
        suivis.last { $0.etat == .enCours }
    }
}
