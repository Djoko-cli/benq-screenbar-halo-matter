import Foundation
import HaloProtocole
import Security
import Synchronization

/// Pont connu de ce Mac : son nom SRP (16 hexa, sans `.local`) et l'empreinte de sa cle.
struct PontConnu: Hashable, Sendable, Identifiable {
    let nom: String
    let empreinte: String
    var id: String { nom }
    var hote: String { "\(nom).local" }
}

/// Cles du transport reseau (10.4). La cle ne quitte le trousseau que pour
/// ouvrir une session.
protocol TrousseauCles: Sendable {
    func lister() -> [PontConnu]
    func lire(nom: String) throws -> Data
    func ranger(nom: String, cle: Data, empreinte: String) throws
    func oublier(nom: String) throws
}

enum ErreurTrousseau: Error, Equatable, Sendable, CustomStringConvertible {
    case absente(String)
    case systeme(Int32)

    var description: String {
        switch self {
        case .absente(let nom):
            tr("Clé absente de ce Mac pour \(nom).local : brancher le pont en USB, puis « Activer l'accès réseau ».")
        case .systeme(let s):
            tr("Trousseau : \(SecCopyErrorMessageString(s, nil) as String? ?? String(s))")
        }
    }
}

/// Trousseau de session du Mac : mot de passe generique, service
/// `fr.djoko.halo.pont`, compte = nom SRP (sans `.local`), valeur = 64 hexa MAJUSCULES,
/// commentaire = empreinte. Non synchronise (iCloud : phase 3).
struct TrousseauSysteme: TrousseauCles {
    let service: String

    init(service: String = "fr.djoko.halo.pont") {
        self.service = service
    }

    private func requete(_ nom: String? = nil) -> [String: Any] {
        var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service]
        if let nom { q[kSecAttrAccount as String] = nom }
        return q
    }

    func lister() -> [PontConnu] {
        var q = requete()
        q[kSecMatchLimit as String] = kSecMatchLimitAll
        q[kSecReturnAttributes as String] = true
        var r: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &r) == errSecSuccess, let elements = r as? [[String: Any]]
        else { return [] }
        return elements.compactMap { a in
            guard let nom = a[kSecAttrAccount as String] as? String else { return nil }
            return PontConnu(nom: nom, empreinte: a[kSecAttrComment as String] as? String ?? "?")
        }.sorted { $0.nom < $1.nom }
    }

    func lire(nom: String) throws -> Data {
        var q = requete(nom)
        q[kSecReturnData as String] = true
        var r: CFTypeRef?
        let s = SecItemCopyMatching(q as CFDictionary, &r)
        if s == errSecItemNotFound { throw ErreurTrousseau.absente(nom) }
        guard s == errSecSuccess else { throw ErreurTrousseau.systeme(s) }
        guard let d = r as? Data, let cle = H1.octets(hexa: String(decoding: d, as: UTF8.self)), cle.count == 32
        else { throw ErreurTrousseau.systeme(errSecDecode) }
        return cle
    }

    func ranger(nom: String, cle: Data, empreinte: String) throws {
        let valeurs: [String: Any] = [kSecValueData as String: Data(H1.hexa(cle).utf8),
                                      kSecAttrComment as String: empreinte,
                                      kSecAttrLabel as String: "Halo - pont \(nom)"]
        var s = SecItemUpdate(requete(nom) as CFDictionary, valeurs as CFDictionary)
        if s == errSecItemNotFound {
            s = SecItemAdd(requete(nom).merging(valeurs) { $1 } as CFDictionary, nil)
        }
        guard s == errSecSuccess else { throw ErreurTrousseau.systeme(s) }
    }

    func oublier(nom: String) throws {
        let s = SecItemDelete(requete(nom) as CFDictionary)
        guard s == errSecSuccess || s == errSecItemNotFound else { throw ErreurTrousseau.systeme(s) }
    }
}

/// Trousseau des tests : en memoire.
final class TrousseauMemoire: TrousseauCles {
    private let cles = Mutex<[String: (cle: Data, empreinte: String)]>([:])

    func lister() -> [PontConnu] {
        cles.withLock { $0.map { PontConnu(nom: $0.key, empreinte: $0.value.empreinte) } }.sorted { $0.nom < $1.nom }
    }

    func lire(nom: String) throws -> Data {
        guard let e = cles.withLock({ $0[nom] }) else { throw ErreurTrousseau.absente(nom) }
        return e.cle
    }

    func ranger(nom: String, cle: Data, empreinte: String) throws {
        cles.withLock { $0[nom] = (cle, empreinte) }
    }

    func oublier(nom: String) throws {
        _ = cles.withLock { $0.removeValue(forKey: nom) }
    }
}
