import CryptoKit
import Foundation

/// Enveloppe H1 du transport reseau (docs/PROTOCOLE-JSON.md 10.4), code pur.
/// MAC = 16 premiers octets de HMAC-SHA256, ecrits en 32 hexa MAJUSCULES ;
/// seuls les textes canoniques passent (hexa majuscule, `ctr` decimal sans
/// zero de tete).
public enum H1 {
    /// Hexa MAJUSCULE.
    public static func hexa<S: Sequence>(_ octets: S) -> String where S.Element == UInt8 {
        let chiffres = Array("0123456789ABCDEF".utf8)
        var s: [UInt8] = []
        for o in octets {
            s.append(chiffres[Int(o >> 4)])
            s.append(chiffres[Int(o & 0x0F)])
        }
        return String(decoding: s, as: UTF8.self)
    }

    /// Octets d'un texte hexa MAJUSCULE de longueur paire ; nil sinon.
    public static func octets(hexa: String) -> Data? {
        let u = Array(hexa.utf8)
        guard u.count % 2 == 0 else { return nil }
        func valeur(_ c: UInt8) -> UInt8? {
            switch c {
            case 0x30...0x39: return c - 0x30
            case 0x41...0x46: return c - 0x37
            default: return nil
            }
        }
        var d = Data(capacity: u.count / 2)
        for i in stride(from: 0, to: u.count, by: 2) {
            guard let h = valeur(u[i]), let l = valeur(u[i + 1]) else { return nil }
            d.append(h << 4 | l)
        }
        return d
    }

    /// `n` octets d'un generateur cryptographique.
    public static func aleatoire(_ n: Int) -> Data {
        SymmetricKey(size: SymmetricKeySize(bitCount: n * 8)).withUnsafeBytes { Data($0) }
    }

    /// `kid` : 8 premiers hexa de SHA-256(cle).
    public static func kid(cle: Data) -> String {
        String(hexa(SHA256.hash(data: cle)).prefix(8))
    }

    static func mac(_ cle: SymmetricKey, _ message: Data) -> String {
        hexa(HMAC<SHA256>.authenticationCode(for: message, using: cle).prefix(16))
    }

    /// SALUT signe (83 octets) : `H1 SALUT <kid> <na> <mac_salut>`.
    public static func salut(cle: Data, na: Data) -> Data {
        let k = kid(cle: cle), n = hexa(na)
        let m = mac(SymmetricKey(data: cle), Data("H1|SALUT|\(k)|\(n)".utf8))
        return Data("H1 SALUT \(k) \(n) \(m)".utf8)
    }

    /// DEFI au MAC juste pour ce `na` : `(sid, nc)`. Sinon nil (DEFI d'un
    /// essai precedent, faux, ou autre datagramme).
    public static func verifierDefi(_ datagramme: Data, cle: Data, na: Data) -> (sid: String, nc: String)? {
        let champs = datagramme.split(separator: 0x20, omittingEmptySubsequences: false)
        guard champs.count == 5, champs[0].elementsEqual("H1".utf8), champs[1].elementsEqual("DEFI".utf8),
              estHexa(champs[2], longueur: 8), estHexa(champs[3], longueur: 32), estHexa(champs[4], longueur: 32)
        else { return nil }
        let sid = String(decoding: champs[2], as: UTF8.self)
        let nc = String(decoding: champs[3], as: UTF8.self)
        let attendu = mac(SymmetricKey(data: cle), Data("H1|DEFI|\(kid(cle: cle))|\(hexa(na))|\(nc)|\(sid)".utf8))
        guard egaux(Data(attendu.utf8), champs[4]) else { return nil }
        return (sid, nc)
    }

    /// Ks = HMAC-SHA256(PSK, `"H1|SESSION|" na "|" nc "|" sid`).
    public static func cleSession(cle: Data, na: Data, nc: String, sid: String) -> SymmetricKey {
        let code = HMAC<SHA256>.authenticationCode(for: Data("H1|SESSION|\(hexa(na))|\(nc)|\(sid)".utf8),
                                                  using: SymmetricKey(data: cle))
        return SymmetricKey(data: Data(code))
    }

    static func estHexa(_ s: Data, longueur: Int) -> Bool {
        s.count == longueur && s.allSatisfy { (0x30...0x39).contains($0) || (0x41...0x46).contains($0) }
    }

    /// Comparaison en temps constant (les longueurs sont publiques).
    static func egaux(_ a: Data, _ b: Data) -> Bool {
        guard a.count == b.count else { return false }
        var d: UInt8 = 0
        for (x, y) in zip(a, b) { d |= x ^ y }
        return d == 0
    }
}

/// Fenetre anti-rejeu de 32 (10.4) : `ctr` strictement croissant, desordre
/// admis sur 32. A juger APRES le MAC : un `ctr` forge ne la pousse jamais.
public struct FenetreAntiRejeu: Sendable, Equatable {
    public private(set) var haut: UInt32 = 0
    private var bits: UInt32 = 0

    public init() {}

    public mutating func accepter(_ ctr: UInt32) -> Bool {
        guard ctr != 0 else { return false }
        if ctr > haut {
            let saut = ctr - haut
            bits = saut >= 32 ? 0 : bits << saut
            bits |= 1
            haut = ctr
            return true
        }
        let recul = haut - ctr
        guard recul < 32, bits & (1 << recul) == 0 else { return false }
        bits |= 1 << recul
        return true
    }
}

/// Session H1 etablie : scelle les lignes de l'app (sens `A`), ouvre les
/// datagrammes de la carte (sens `C`).
public struct SessionH1: Sendable {
    public let sid: String
    /// Ks, gardee en octets (valeur `Sendable`).
    private let ks: Data
    public private(set) var ctrEmis: UInt32 = 0
    /// Datagrammes ecartes (forme, sid, MAC, rejeu) depuis l'ouverture.
    public private(set) var ecartes = 0
    private var fenetre = FenetreAntiRejeu()

    public init(sid: String, ks: SymmetricKey) {
        self.sid = sid
        self.ks = ks.withUnsafeBytes { Data($0) }
    }

    /// `H1 <sid> <ctr> <mac> <ligne>`, `ctr` +1 a chaque appel (4 milliards
    /// de messages par session : jamais atteint, la session dure 10 min sans message).
    public mutating func sceller(_ ligne: Data) -> Data {
        ctrEmis &+= 1
        let m = H1.mac(SymmetricKey(data: ks), Data("A|\(sid)|\(ctrEmis)|".utf8) + ligne)
        return Data("H1 \(sid) \(ctrEmis) \(m) ".utf8) + ligne
    }

    /// Charge d'un message `C` valide (forme canonique, `sid`, MAC, fenetre) ;
    /// sinon nil, et `ecartes` +1.
    public mutating func ouvrir(_ datagramme: Data) -> Data? {
        guard let charge = verifier(datagramme) else {
            ecartes += 1
            return nil
        }
        return charge
    }

    private mutating func verifier(_ d: Data) -> Data? {
        // H1 <sid> <ctr> <mac> <charge> : les 4 premieres espaces separent les champs.
        let champs = d.split(separator: 0x20, maxSplits: 4, omittingEmptySubsequences: false)
        guard champs.count == 5, champs[0].elementsEqual("H1".utf8), champs[1].elementsEqual(sid.utf8),
              let ctr = Self.ctrCanonique(champs[2]), H1.estHexa(champs[3], longueur: 32)
        else { return nil }
        let charge = champs[4]
        let attendu = H1.mac(SymmetricKey(data: ks), Data("C|\(sid)|\(ctr)|".utf8) + charge)
        guard H1.egaux(Data(attendu.utf8), champs[3]), fenetre.accepter(ctr) else { return nil }
        return Data(charge)
    }

    /// `ctr` decimal sans zero de tete, 1..4294967295.
    static func ctrCanonique(_ s: Data) -> UInt32? {
        guard (1...10).contains(s.count), s.first != 0x30, s.allSatisfy({ (0x30...0x39).contains($0) }) else { return nil }
        return UInt32(String(decoding: s, as: UTF8.self))
    }
}
