import Darwin
import Foundation

/// Ouverture du port serie USB du C6 sans jamais le redemarrer (sections 3.1 et 3.2).
///
/// Le peripherique USB Serial/JTAG lit DTR et RTS comme esptool : RTS=1 et
/// DTR=0, meme un instant, REDEMARRE LA PUCE. D'ou :
/// - DTR et RTS poses a 0 ensemble, en un seul `ioctl(TIOCMSET)`, juste apres
///   l'ouverture (passage direct 1,1 -> 0,0), et plus jamais touches ;
/// - `HUPCL` retire : a la fermeture, le pilote ne baisse pas DTR avant RTS.
enum PortSerie {
    // Macros de sys/ttycom.h non importees en Swift (_IO, _IOR, _IOW).
    /// `_IO('t', 13)` : acces exclusif (pio device monitor ne pourra pas s'y attacher).
    static let tiocexcl: UInt = 0x2000_740D
    /// `_IOR('t', 106, int)` : lire les lignes de controle.
    static let tiocmget: UInt = 0x4004_746A
    /// `_IOW('t', 109, int)` : ecrire les lignes de controle, toutes a la fois.
    static let tiocmset: UInt = 0x8004_746D
    /// `_IOR('t', 115, int)` : octets encore dans la file de sortie du tty.
    static let tiocoutq: UInt = 0x4004_7473

    static let debit: speed_t = 115_200

    static func erreur(_ quoi: String) -> ErreurPort {
        ErreurPort(quoi: quoi, errno: errno)
    }

    /// Ouvre `/dev/cu.*` (jamais `/dev/tty.*`, qui attend DCD) ; renvoie le descripteur.
    static func ouvrir(_ chemin: String) throws -> Int32 {
        guard chemin.hasPrefix("/dev/cu.") else { throw ErreurPort(quoi: "chemin \(chemin) : /dev/cu.* attendu", errno: 0) }
        let fd = open(chemin, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { throw erreur("ouverture de \(chemin)") }
        do {
            // 1. Acces exclusif.
            guard ioctl(fd, tiocexcl) != -1 else { throw erreur("accès exclusif (TIOCEXCL)") }

            // 2. DTR = RTS = 0 dans un seul appel : jamais l'etat RTS=1, DTR=0.
            var lignes: Int32 = 0
            guard withUnsafeMutablePointer(to: &lignes, { ioctl(fd, tiocmget, $0) }) != -1 else {
                throw erreur("lecture de DTR et RTS (TIOCMGET)")
            }
            lignes &= ~(TIOCM_DTR | TIOCM_RTS)
            guard withUnsafeMutablePointer(to: &lignes, { ioctl(fd, tiocmset, $0) }) != -1 else {
                throw erreur("DTR et RTS à 0 (TIOCMSET)")
            }

            // 3. Mode brut, 8N1, CLOCAL | CREAD, HUPCL retire, 115200 (ignore par l'USB natif).
            var t = termios()
            guard tcgetattr(fd, &t) != -1 else { throw erreur("lecture des réglages (tcgetattr)") }
            cfmakeraw(&t)
            t.c_cflag |= tcflag_t(CLOCAL | CREAD | CS8)
            t.c_cflag &= ~tcflag_t(HUPCL | PARENB | CSTOPB | CRTSCTS)
            t.c_iflag &= ~tcflag_t(IXON | IXOFF | IXANY)
            guard cfsetspeed(&t, debit) != -1 else { throw erreur("débit (cfsetspeed)") }
            guard tcsetattr(fd, TCSANOW, &t) != -1 else { throw erreur("réglages (tcsetattr)") }
            return fd
        } catch {
            // Ouvrir a pose DTR = RTS = 1 : les baisser ensemble et retirer HUPCL
            // avant de fermer, pour que la fermeture ne passe jamais par RTS=1, DTR=0.
            desarmer(fd)
            close(fd)
            throw error
        }
    }

    /// Au mieux, sur un chemin d'erreur : DTR = RTS = 0 en un seul `TIOCMSET`, puis `HUPCL` retire.
    static func desarmer(_ fd: Int32) {
        var lignes: Int32 = 0
        if withUnsafeMutablePointer(to: &lignes, { ioctl(fd, tiocmget, $0) }) == -1 { lignes = 0 }
        lignes &= ~(TIOCM_DTR | TIOCM_RTS)
        _ = withUnsafeMutablePointer(to: &lignes, { ioctl(fd, tiocmset, $0) })
        var t = termios()
        if tcgetattr(fd, &t) != -1 {
            t.c_cflag &= ~tcflag_t(HUPCL)
            _ = tcsetattr(fd, TCSANOW, &t)
        }
    }

    /// Etat actuel de DTR et RTS (diagnostic).
    static func lignesDeControle(_ fd: Int32) -> (dtr: Bool, rts: Bool)? {
        var lignes: Int32 = 0
        guard withUnsafeMutablePointer(to: &lignes, { ioctl(fd, tiocmget, $0) }) != -1 else { return nil }
        return ((lignes & TIOCM_DTR) != 0, (lignes & TIOCM_RTS) != 0)
    }
}

struct ErreurPort: Error, CustomStringConvertible {
    var quoi: String
    var errno: Int32

    var description: String {
        guard errno != 0 else { return quoi }
        let detail = String(cString: strerror(errno))
        if errno == EBUSY {
            return "\(quoi) : port occupé (pio device monitor ou une autre app le tient)"
        }
        return "\(quoi) : \(detail)"
    }
}
