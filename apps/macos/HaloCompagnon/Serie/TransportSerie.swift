import Darwin
import Foundation
import HaloProtocole
import Synchronization

/// Transport USB : port serie POSIX lu par une source Dispatch.
///
/// Toutes les lectures et ecritures passent par une meme file serie : l'ordre
/// des lignes envoyees est garanti, et la file principale ne bloque jamais.
final class TransportSerie: Transport {
    let genre: GenreTransport = .usb
    let chemin: String
    var nom: String { chemin }

    private struct Etat {
        var fd: Int32 = -1
        var source: (any DispatchSourceRead)?
        var suite: AsyncStream<EvenementTransport>.Continuation?
    }

    private let file = DispatchQueue(label: "fr.djoko.halo.serie", qos: .userInitiated)
    private let etat = Mutex(Etat())

    init(chemin: String) {
        self.chemin = chemin
    }

    func ouvrir() async throws -> AsyncStream<EvenementTransport> {
        let fd = try PortSerie.ouvrir(chemin)
        let (flux, suite) = AsyncStream.makeStream(of: EvenementTransport.self, bufferingPolicy: .unbounded)
        let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: file)
        source.setEventHandler { [weak self] in self?.lire() }
        source.setCancelHandler {
            // DTR et RTS sont deja a 0 et HUPCL est retire : fermer ne change rien aux lignes.
            close(fd)
        }
        etat.withLock { e in
            e.fd = fd
            e.source = source
            e.suite = suite
        }
        suite.onTermination = { [weak self] _ in self?.fermer() }
        source.resume()
        return flux
    }

    /// Sur la file serie : tout ce qui est disponible.
    private func lire() {
        let fd = etat.withLock { $0.fd }
        guard fd >= 0 else { return }
        var tampon = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = tampon.withUnsafeMutableBytes { read(fd, $0.baseAddress, $0.count) }
            if n > 0 {
                let donnees = Data(tampon[0..<n])
                _ = etat.withLock { $0.suite?.yield(.donnees(donnees)) }
                continue
            }
            if n == 0 {
                terminer("port fermé (EOF) : la carte a peut-être redémarré")
                return
            }
            let code = errno
            if code == EAGAIN || code == EWOULDBLOCK { return }
            if code == EINTR { continue }
            terminer("lecture impossible : \(String(cString: strerror(code))) (ré-énumération USB ?)")
            return
        }
    }

    func envoyer(_ donnees: Data) throws {
        guard etat.withLock({ $0.fd }) >= 0 else { throw ErreurTransport("port fermé") }
        file.async { [weak self] in self?.ecrire(donnees) }
    }

    /// Sur la file serie. Ligne de 128 octets au plus : quelques essais si le tampon est plein.
    private func ecrire(_ donnees: Data) {
        let fd = etat.withLock { $0.fd }
        guard fd >= 0 else { return }
        var reste = donnees[...]
        var essais = 0
        while !reste.isEmpty {
            let n = reste.withUnsafeBytes { write(fd, $0.baseAddress, $0.count) }
            if n > 0 {
                reste = reste.dropFirst(n)
                continue
            }
            let code = errno
            if n < 0, code == EAGAIN || code == EINTR, essais < 50 {
                essais += 1
                usleep(2000)
                continue
            }
            terminer("écriture impossible : \(String(cString: strerror(code)))")
            return
        }
    }

    private func terminer(_ raison: String) {
        let (source, suite) = etat.withLock { e -> ((any DispatchSourceRead)?, AsyncStream<EvenementTransport>.Continuation?) in
            let r = (e.source, e.suite)
            e.source = nil
            e.suite = nil
            e.fd = -1
            return r
        }
        source?.cancel()
        suite?.yield(.ferme(raison: raison))
        suite?.finish()
    }

    func fermer() {
        file.async { [weak self] in self?.terminer("port fermé par l'app") }
    }
}
