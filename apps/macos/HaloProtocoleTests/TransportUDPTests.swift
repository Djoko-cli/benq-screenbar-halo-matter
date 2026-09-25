import CryptoKit
import Darwin
import Foundation
import Synchronization
import Testing
@testable import HaloProtocole

/// Pont local sur [::1] : repond au SALUT par un DEFI, ouvre les messages A,
/// scelle des lignes C. `muet` : ne repond jamais. `mode` : deformations
/// volontaires du DEFI pour verifier que l'app les rejette (10.4).
final class PontLocal: Sendable {
    /// Deformations volontaires du DEFI (le SALUT reste verifie normalement : kid, MAC).
    enum Mode: Sendable, Equatable {
        case normal
        /// DEFI calcule pour le `na` de l'essai PRECEDENT (jamais celui de l'essai en cours).
        case defiNaPerime
        /// DEFI au bon kid/sid/nc/na, mais au MAC faux.
        case defiMacFaux
    }

    let port: UInt16
    private let fd: Int32
    private let cle: Data
    private let muet: Bool
    private let mode: Mode
    private struct Etat {
        var saluts = 0
        /// `na` de chaque SALUT recu (meme mute ou de mauvaise cle) : verifie qu'il est neuf a chaque essai.
        var nas: [Data] = []
        var naPrecedent: Data?
        var recues: [String] = []
        /// `ctr` de chaque message A accepte, dans le meme ordre que `recues`.
        var ctrsRecus: [UInt32] = []
        var session: (sid: String, ks: SymmetricKey, ctr: UInt32)?
        var pair: sockaddr_in6?
        var fini = false
    }
    private let etat = Mutex(Etat())

    init(cle: Data, muet: Bool = false, mode: Mode = .normal) throws {
        self.cle = cle
        self.muet = muet
        self.mode = mode
        // Descripteur garde dans un `let` local : une fermeture qui lirait la
        // propriete `fd` capturerait `self`, refuse avant que `port` le soit.
        let descripteur = socket(AF_INET6, SOCK_DGRAM, 0)
        fd = descripteur
        var a = sockaddr_in6()
        a.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        a.sin6_family = sa_family_t(AF_INET6)
        a.sin6_addr = in6addr_loopback
        let lie = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            bind(descripteur, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } }
        try #require(lie == 0)
        var l = socklen_t(MemoryLayout<sockaddr_in6>.size)
        _ = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            getsockname(descripteur, $0, &l) } }
        port = UInt16(bigEndian: a.sin6_port)
        var tv = timeval(tv_sec: 0, tv_usec: 100_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        let moi = self
        Thread.detachNewThread { moi.boucle() }
    }

    var saluts: Int { etat.withLock { $0.saluts } }
    var nas: [Data] { etat.withLock { $0.nas } }
    var recues: [String] { etat.withLock { $0.recues } }
    var ctrsRecus: [UInt32] { etat.withLock { $0.ctrsRecus } }

    func arreter() {
        etat.withLock { $0.fini = true }
    }

    /// Scelle une ligne de la carte (sens C) et l'envoie a l'app.
    func envoyer(_ json: String) {
        let (datagramme, pair): (Data?, sockaddr_in6?) = etat.withLock { e in
            guard var s = e.session else { return (nil, nil) }
            s.ctr += 1
            e.session = s
            let charge = Data(json.utf8)
            let m = H1.mac(s.ks, Data("C|\(s.sid)|\(s.ctr)|".utf8) + charge)
            return (Data("H1 \(s.sid) \(s.ctr) \(m) ".utf8) + charge, e.pair)
        }
        guard let datagramme, var pair else { return }
        _ = datagramme.withUnsafeBytes { b in withUnsafePointer(to: &pair) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            sendto(fd, b.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } } }
    }

    private func boucle() {
        var tampon = [UInt8](repeating: 0, count: 2048)
        while !etat.withLock({ $0.fini }) {
            var de = sockaddr_in6()
            var l = socklen_t(MemoryLayout<sockaddr_in6>.size)
            let n = withUnsafeMutablePointer(to: &de) { p in p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                recvfrom(fd, &tampon, tampon.count, 0, $0, &l) } }
            guard n > 0 else { continue }
            recu(Data(tampon[0..<n]), de: de)
        }
        close(fd)
    }

    private func recu(_ d: Data, de: sockaddr_in6) {
        let champs = d.split(separator: 0x20, maxSplits: 4, omittingEmptySubsequences: false).map { String(decoding: $0, as: UTF8.self) }
        if champs.count == 5, champs[1] == "SALUT" {
            salut(kid: champs[2], naHexa: champs[3], macSalut: champs[4], de: de)
            return
        }
        // Message A : MAC verifie avec Ks, charge et ctr gardes.
        etat.withLock { e in
            guard let s = e.session, champs.count == 5, champs[1] == s.sid, let ctr = UInt32(champs[2]) else { return }
            let attendu = H1.mac(s.ks, Data("A|\(s.sid)|\(champs[2])|".utf8) + Data(champs[4].utf8))
            if attendu == champs[3] {
                e.recues.append(champs[4])
                e.ctrsRecus.append(ctr)
                e.pair = de
            }
        }
    }

    private func salut(kid: String, naHexa: String, macSalut: String, de: sockaddr_in6) {
        let na = H1.octets(hexa: naHexa)
        etat.withLock { e in
            e.saluts += 1
            if let na { e.nas.append(na) }
        }
        guard !muet, let na, kid == H1.kid(cle: cle) else { return }
        // Le vrai pont ne repond qu'a un SALUT au MAC juste (10.4).
        let macAttendu = H1.mac(SymmetricKey(data: cle), Data("H1|SALUT|\(kid)|\(naHexa)".utf8))
        guard macAttendu == macSalut else { return }
        let sid = "5A5A0001", nc = H1.hexa(H1.aleatoire(16))
        let naPrecedent = etat.withLock { e -> Data? in
            let p = e.naPrecedent
            e.naPrecedent = na
            return p
        }
        if mode == .defiNaPerime, naPrecedent == nil { return }  // 1er essai : rien a perimer encore
        let naDuDefi = mode == .defiNaPerime ? (naPrecedent ?? na) : na
        var mac = H1.mac(SymmetricKey(data: cle), Data("H1|DEFI|\(kid)|\(H1.hexa(naDuDefi))|\(nc)|\(sid)".utf8))
        if mode == .defiMacFaux { mac = Self.abimer(mac) }
        etat.withLock { $0.session = (sid, H1.cleSession(cle: cle, na: na, nc: nc, sid: sid), 0); $0.pair = de }
        var pair = de
        let defi = Data("H1 DEFI \(sid) \(nc) \(mac)".utf8)
        _ = defi.withUnsafeBytes { b in withUnsafePointer(to: &pair) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            sendto(fd, b.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } } }
    }

    /// Un hexa MAJUSCULE valide change en un autre, tout aussi valide (MAC faux, meme forme).
    private static func abimer(_ hexa: String) -> String {
        var c = Array(hexa)
        c[0] = c[0] == "A" ? "B" : "A"
        return String(c)
    }
}

/// Attend une condition, au plus `delai`.
func attendreQue(_ delai: Duration = .seconds(5), _ condition: () -> Bool) async -> Bool {
    let fin = ContinuousClock.now + delai
    while ContinuousClock.now < fin {
        if condition() { return true }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return condition()
}

/// Premier evenement du flux, au plus `delai`.
func premier(_ flux: AsyncStream<EvenementTransport>, _ delai: Duration = .seconds(5)) async -> EvenementTransport? {
    await withTaskGroup(of: EvenementTransport?.self) { g in
        g.addTask { for await e in flux { return e }; return nil }
        g.addTask { try? await Task.sleep(for: delai); return nil }
        let r = await g.next() ?? nil
        g.cancelAll()
        return r
    }
}

@Suite("Transport UDP (10.2 a 10.4)", .serialized)
struct TransportUDPTests {
    static func rapides(_ port: UInt16) -> TransportUDP.Reglages {
        var r = TransportUDP.Reglages()
        r.port = port
        r.attentePret = .seconds(2)
        r.attenteDefi = .milliseconds(300)
        return r
    }

    @Test func poigneeDeMainEtDialogue() async throws {
        let pont = try PontLocal(cle: VecteursH1.psk)
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        let flux = try await t.ouvrir()
        try t.envoyer(Data([0x15, 0x0A]) + Data("id=1 json 1\n".utf8))
        #expect(await attendreQue { pont.recues == ["id=1 json 1"] }, "Ctrl-U ecarte, ligne scellee")
        // Renvoi (10.2) : memes octets renvoyes (meme id) ; ctr strictement croissant a chaque envoi.
        try t.envoyer(Data("id=1 json 1\n".utf8))
        #expect(await attendreQue { pont.recues == ["id=1 json 1", "id=1 json 1"] }, "meme ligne renvoyee, recue deux fois")
        #expect(pont.ctrsRecus.count == 2)
        #expect(pont.ctrsRecus[0] < pont.ctrsRecus[1], "ctr neuf a chaque envoi, meme ligne renvoyee")
        pont.envoyer(#"{"v":1,"t":"hb","n":0,"ms":1}"#)
        let e = await premier(flux)
        #expect(e == .donnees(Data([0x1E]) + Data(#"{"v":1,"t":"hb","n":0,"ms":1}"#.utf8) + Data([0x0A])))
        try t.envoyer(Data("id=2 json 0\n".utf8))
        t.fermerApresVidage(synchrone: false)
        #expect(await attendreQue { pont.recues.last == "id=2 json 0" }, "json 0 part avant la fermeture")
    }

    @Test func pontMuet() async throws {
        let pont = try PontLocal(cle: VecteursH1.psk, muet: true)
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        await #expect(throws: ErreurReseau.aucunDefi) { _ = try await t.ouvrir() }
        #expect(pont.saluts == 3, "trois SALUT, na neuf a chaque essai")
        #expect(Set(pont.nas).count == 3, "les 3 na sont distincts")
    }

    @Test func autreCle() async throws {
        let pont = try PontLocal(cle: Data(repeating: 7, count: 32))
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        await #expect(throws: ErreurReseau.aucunDefi) { _ = try await t.ouvrir() }
    }

    @Test func defiPourUnNaPerime() async throws {
        // Le pair repond, mais toujours avec le DEFI de l'essai precedent (jamais l'essai en cours) :
        // H1.verifierDefi le rejette a chaque fois (mac du DEFI juge sur le na courant).
        let pont = try PontLocal(cle: VecteursH1.psk, mode: .defiNaPerime)
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        await #expect(throws: ErreurReseau.aucunDefi) { _ = try await t.ouvrir() }
    }

    @Test func defiAuMacFaux() async throws {
        // Le pair repond avec le bon kid/sid/nc/na mais un MAC de DEFI faux : rejete a chaque essai.
        let pont = try PontLocal(cle: VecteursH1.psk, mode: .defiMacFaux)
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        await #expect(throws: ErreurReseau.aucunDefi) { _ = try await t.ouvrir() }
    }

    // Sur ce SDK, ::1 ne remonte PAS l'ICMPv6 "port injoignable" pendant les essais
    // (connexion .ready, les send de SALUT se terminent sans erreur, aucun changement
    // d'etat) : le `receiveMessage` deja pose ne recoit "Connection refused" que
    // 6 a 8 ms APRES `c.cancel()`, une fois les 3 essais deja epuises. D'ou
    // TransportUDP.echecPoignee : apres le dernier essai, annuler puis attendre
    // brievement (10 ms x 15, 150 ms au plus) l'erreur portee par ce receive en
    // attente. Fiable ici : 3/3 seul, 5/5 dans cette suite serialisee.
    @Test func portFerme() async throws {
        // Un port libre puis ferme : l'ICMPv6 "port injoignable" remonte (apres annulation).
        let libre = try PontLocal(cle: VecteursH1.psk)
        let port = libre.port
        libre.arreter()
        try await Task.sleep(for: .milliseconds(300))  // la boucle du pair ferme sa socket
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(port))
        await #expect(throws: ErreurReseau.portInjoignable) { _ = try await t.ouvrir() }
    }

    /// Course de l'ouverture : un `.failed` ou une erreur de reception arrive
    /// apres le dernier coup d'oeil de la poignee de main, avant que le flux
    /// soit pose. `echec` n'avait alors aucun flux a fermer : le poser quand
    /// meme rendrait un flux qui ne recevrait jamais `.ferme`.
    @Test func echecAvantLaPoseDuFluxLaRefuse() throws {
        let session = SessionH1(sid: "5A5A0001", ks: SymmetricKey(data: VecteursH1.psk))
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk)
        t.echec(.pasDeRoute)
        let (_, suite) = AsyncStream.makeStream(of: EvenementTransport.self)
        #expect(throws: ErreurReseau.pasDeRoute) { try t.adopter(session, suite) }
        // Fermeture demandee pendant l'ouverture, sans erreur gardee : refusee aussi.
        let f = TransportUDP(hote: "::1", cle: VecteursH1.psk)
        f.fermerApresVidage(synchrone: true)
        let (_, suiteF) = AsyncStream.makeStream(of: EvenementTransport.self)
        #expect(throws: ErreurTransport.self) { try f.adopter(session, suiteF) }
        // Sans echec ni fermeture : la session et le flux sont poses.
        let o = TransportUDP(hote: "::1", cle: VecteursH1.psk)
        let (_, suiteO) = AsyncStream.makeStream(of: EvenementTransport.self)
        try o.adopter(session, suiteO)
        o.fermerApresVidage(synchrone: true)
    }

    /// Session perdue apres l'ouverture (`.failed`, erreur de reception :
    /// ENETDOWN...) : le flux se ferme sur "Connexion réseau perdue : <cause>",
    /// pas sur le texte brut de la cause (4.6).
    @Test func perteEnSessionFermeSurConnexionPerdue() async throws {
        let pont = try PontLocal(cle: VecteursH1.psk)
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        let flux = try await t.ouvrir()
        t.echec(.pasDeRoute)
        let attendu = ErreurReseau.cheminPerdu(ErreurReseau.pasDeRoute.description).description
        #expect(await premier(flux) == .ferme(raison: attendu))
        #expect(TransportUDP.raisonPerte(.pasDeRoute) == attendu)
        #expect(attendu != ErreurReseau.pasDeRoute.description)
    }

    @Test func textes() {
        #expect(!ErreurReseau.portInjoignable.repriseAutomatique)
        #expect(!ErreurReseau.reseauLocalRefuse.repriseAutomatique)
        #expect(ErreurReseau.pasDeRoute.repriseAutomatique)
        #expect(ErreurReseau.depuis(.posix(.EHOSTUNREACH), chemin: nil, hote: "x.local") == .pasDeRoute)
        #expect(ErreurReseau.depuis(.posix(.ENETUNREACH), chemin: nil, hote: "x.local") == .pasDeRoute)
        #expect(ErreurReseau.depuis(.posix(.ENETDOWN), chemin: nil, hote: "x.local") == .pasDeRoute)
        // ICMPv6 "adresse injoignable" : le noeud ne repond pas, la route n'y est pour rien.
        #expect(ErreurReseau.depuis(.posix(.EHOSTDOWN), chemin: nil, hote: "x.local") == .nomIntrouvable("x.local"))
        #expect(ErreurReseau.depuis(.posix(.ECONNREFUSED), chemin: nil, hote: "x.local") == .portInjoignable)
        #expect(ErreurReseau.depuis(.dns(-65554), chemin: nil, hote: "x.local") == .nomIntrouvable("x.local"))
        #expect(ErreurReseau.depuis(.dns(-65570), chemin: nil, hote: "x.local") == .reseauLocalRefuse)
    }
}
