import Foundation
import Network
import Synchronization

/// Transport reseau (section 10) : UDP sur Thread vers `<nom>.local:5480`,
/// enveloppe H1. Pour le reste de l'app, un port de plus : chaque datagramme
/// valide recu devient une ligne RS + JSON + LF ; chaque ligne envoyee, un
/// datagramme scelle (les lignes vides ou de controle seul sont ecartees).
public final class TransportUDP: Transport {
    public struct Reglages: Sendable {
        public var port: UInt16 = 5480
        /// Connexion prete (resolution du nom, route) : au plus.
        public var attentePret: Duration = .seconds(5)
        /// DEFI juste apres chaque SALUT : au plus.
        public var attenteDefi: Duration = .seconds(2)
        public var essais = 3
        public init() {}
    }

    public let genre: GenreTransport = .udp
    public let hote: String
    public var nom: String { hote }
    private let cle: Data
    private let reglages: Reglages
    private let file = DispatchQueue(label: "fr.djoko.halo.udp", qos: .userInitiated)

    private struct Etat {
        var connexion: NWConnection?
        var pret = false
        var echoue = false
        var erreur: ErreurReseau?
        /// Datagrammes arrives pendant la poignee de main (16 au plus).
        var recus: [Data] = []
        var session: SessionH1?
        var suite: AsyncStream<EvenementTransport>.Continuation?
        var envoisEnCours = 0
        var fini = false
    }
    private let etat = Mutex(Etat())

    public init(hote: String, cle: Data, reglages: Reglages = Reglages()) {
        self.hote = hote
        self.cle = cle
        self.reglages = reglages
    }

    /// Datagrammes ecartes (forme, sid, MAC, rejeu) depuis l'ouverture.
    public var ecartes: Int { etat.withLock { $0.session?.ecartes ?? 0 } }

    public func ouvrir() async throws -> AsyncStream<EvenementTransport> {
        let parametres = NWParameters.udp
        if let ip = parametres.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options { ip.version = .v6 }
        guard let port = NWEndpoint.Port(rawValue: reglages.port) else { throw ErreurReseau.autre("port") }
        let c = NWConnection(host: NWEndpoint.Host(hote), port: port, using: parametres)
        etat.withLock { $0.connexion = c }
        c.stateUpdateHandler = { [weak self] s in self?.changement(s, c) }
        c.start(queue: file)
        do {
            try await attendrePret()
            recevoir(c)
            let session = try await poigneeDeMain(c)
            let (flux, suite) = AsyncStream.makeStream(of: EvenementTransport.self, bufferingPolicy: .unbounded)
            try adopter(session, suite)
            suite.onTermination = { [weak self] _ in self?.fermer() }
            return flux
        } catch {
            etat.withLock { e in
                e.fini = true
                e.connexion = nil
            }
            c.cancel()
            throw error
        }
    }

    /// Pose la session et le flux, sauf si un echec (`.failed`, erreur de
    /// reception) ou une fermeture est arrive entre le dernier coup d'oeil de
    /// la poignee de main et ce verrou : `echec` et `terminer` n'avaient alors
    /// aucun flux a fermer, et celui-ci ne recevrait jamais `.ferme`. L'erreur
    /// gardee est levee (ou, pour une fermeture, une erreur de transport) ;
    /// `ouvrir` annule alors la connexion.
    func adopter(_ session: SessionH1, _ suite: AsyncStream<EvenementTransport>.Continuation) throws {
        let refus: (any Error)? = etat.withLock { e in
            guard !e.fini, !e.echoue else {
                if let erreur = e.erreur { return erreur }
                return ErreurTransport(tr("session réseau fermée par l'app"))
            }
            e.session = session
            e.suite = suite
            e.recus.removeAll()
            return nil
        }
        if let refus { throw refus }
    }

    /// Raison de fermeture d'une session perdue apres son ouverture (4.6) :
    /// "Connexion réseau perdue : <cause>".
    static func raisonPerte(_ cause: ErreurReseau) -> String {
        ErreurReseau.cheminPerdu(cause.description).description
    }

    // MARK: - Connexion (file du transport)

    private func changement(_ s: NWConnection.State, _ c: NWConnection) {
        switch s {
        case .ready:
            etat.withLock { $0.pret = true }
        case .waiting(let e):
            // Avant .ready : cause candidate, la connexion peut encore aboutir.
            let err = ErreurReseau.depuis(e, chemin: c.currentPath, hote: hote)
            let ouverte = etat.withLock { et -> Bool in
                et.erreur = err
                return et.suite != nil
            }
            if ouverte { terminer(Self.raisonPerte(err)) }
        case .failed(let e):
            echec(ErreurReseau.depuis(e, chemin: c.currentPath, hote: hote))
        default:
            break
        }
    }

    private func attendrePret() async throws {
        let limite = ContinuousClock.now + reglages.attentePret
        while ContinuousClock.now < limite {
            let (pret, echoue, erreur) = etat.withLock { ($0.pret, $0.echoue, $0.erreur) }
            if pret { return }
            if echoue, let erreur { throw erreur }
            try await Task.sleep(for: .milliseconds(10))
        }
        throw etat.withLock { $0.erreur } ?? ErreurReseau.nomIntrouvable(hote)
    }

    private func recevoir(_ c: NWConnection) {
        c.receiveMessage { [weak self] donnees, _, _, erreur in
            guard let self else { return }
            if let erreur {
                self.echec(ErreurReseau.depuis(erreur, chemin: c.currentPath, hote: self.hote))
                return
            }
            if let donnees, !donnees.isEmpty { self.arrivee(donnees) }
            if !self.etat.withLock({ $0.fini }) { self.recevoir(c) }
        }
    }

    private func arrivee(_ d: Data) {
        let charge: Data? = etat.withLock { e in
            guard var s = e.session else {
                if e.recus.count < 16 { e.recus.append(d) }
                return nil
            }
            let c = s.ouvrir(d)
            e.session = s
            return c
        }
        guard let charge else { return }
        var ligne = Data([Octets.rs])
        ligne.append(charge)
        ligne.append(Octets.lf)
        _ = etat.withLock { $0.suite?.yield(.donnees(ligne)) }
    }

    /// Interne (et non privee) : les tests y simulent un `.failed` ou une
    /// erreur de reception, sans reseau.
    func echec(_ err: ErreurReseau) {
        let ouverte = etat.withLock { e -> Bool in
            e.erreur = err
            e.echoue = true
            return e.suite != nil
        }
        if ouverte { terminer(Self.raisonPerte(err)) }
    }

    // MARK: - Poignee de main (10.4)

    private func poigneeDeMain(_ c: NWConnection) async throws -> SessionH1 {
        for _ in 0..<reglages.essais {
            let na = H1.aleatoire(16)
            c.send(content: H1.salut(cle: cle, na: na), completion: .contentProcessed { _ in })
            let limite = ContinuousClock.now + reglages.attenteDefi
            while ContinuousClock.now < limite {
                let (recu, echoue, erreur) = etat.withLock { e -> (Data?, Bool, ErreurReseau?) in
                    (e.recus.isEmpty ? nil : e.recus.removeFirst(), e.echoue, e.erreur)
                }
                if echoue, let erreur { throw erreur }
                if let recu {
                    if let d = H1.verifierDefi(recu, cle: cle, na: na) {
                        return SessionH1(sid: d.sid, ks: H1.cleSession(cle: cle, na: na, nc: d.nc, sid: d.sid))
                    }
                    continue  // DEFI d'un essai precedent, ou autre datagramme
                }
                try await Task.sleep(for: .milliseconds(10))
            }
        }
        throw try await echecPoignee(c)
    }

    /// Aucun DEFI apres tous les essais : cle differente, ou pont sans cle
    /// (10.4). Sur ce SDK, `::1` ne remonte pas toujours tout de suite
    /// l'ICMPv6 "port injoignable" d'un port ferme (lwIP) : annuler la
    /// connexion la fait parfois apparaitre, portee par le `receiveMessage`
    /// deja en attente, quelques ms plus tard. On annule donc ici et on
    /// attend une derniere fois, brievement, avant de conclure `aucunDefi`.
    private func echecPoignee(_ c: NWConnection) async throws -> ErreurReseau {
        c.cancel()
        etat.withLock { $0.fini = true }
        let limite = ContinuousClock.now + .milliseconds(150)
        while ContinuousClock.now < limite {
            let (echoue, erreur) = etat.withLock { ($0.echoue, $0.erreur) }
            if echoue { return erreur == .portInjoignable ? .portInjoignable : .aucunDefi }
            try await Task.sleep(for: .milliseconds(10))
        }
        return .aucunDefi
    }

    // MARK: - Transport

    public func envoyer(_ donnees: Data) throws {
        let (c, datagrammes): (NWConnection?, [Data]) = etat.withLock { e in
            guard !e.fini, let c = e.connexion, var s = e.session else { return (nil, []) }
            var sortie: [Data] = []
            for ligne in donnees.split(separator: Octets.lf) where ligne.contains(where: { $0 >= 0x20 }) {
                sortie.append(s.sceller(Data(ligne)))
            }
            e.session = s
            e.envoisEnCours += sortie.count
            return (c, sortie)
        }
        guard let c else { throw ErreurTransport(tr("session réseau fermée")) }
        for d in datagrammes {
            c.send(content: d, completion: .contentProcessed { [weak self] _ in
                self?.etat.withLock { $0.envoisEnCours -= 1 }
            })
        }
    }

    public func fermer() {
        file.async { [weak self] in self?.terminer(tr("session réseau fermée par l'app")) }
    }

    /// Laisse partir ce qui est confie (`json 0`), 300 ms au plus, puis ferme.
    /// Hors de la file du transport : les confirmations d'envoi y arrivent.
    public func fermerApresVidage(synchrone: Bool) {
        let travail: @Sendable () -> Void = { [weak self] in
            guard let self else { return }
            let limite = ContinuousClock.now + .milliseconds(300)
            while ContinuousClock.now < limite, self.etat.withLock({ $0.envoisEnCours }) > 0 { usleep(5_000) }
            self.terminer(tr("session réseau fermée par l'app"))
        }
        if synchrone { travail() } else { DispatchQueue.global(qos: .userInitiated).async(execute: travail) }
    }

    private func terminer(_ raison: String) {
        let (c, suite) = etat.withLock { e -> (NWConnection?, AsyncStream<EvenementTransport>.Continuation?) in
            guard !e.fini else { return (nil, nil) }
            e.fini = true
            let r = (e.connexion, e.suite)
            e.connexion = nil
            e.suite = nil
            return r
        }
        c?.cancel()
        suite?.yield(.ferme(raison: raison))
        suite?.finish()
    }
}
