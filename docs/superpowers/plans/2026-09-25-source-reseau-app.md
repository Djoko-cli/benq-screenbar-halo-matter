# Source "Reseau" de Halo Compagnon : plan d'implementation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** l'app macOS joint le pont par UDP sur Thread (enveloppe H1) avec le meme tableau de bord que par l'USB, la cle etant creee par l'USB et gardee dans le trousseau.

**Architecture:** approche A de la spec : `TransportUDP` se presente comme un `Transport` de plus (un datagramme recu devient une ligne RS + JSON + LF) ; `MoteurSession` et le correlateur n'apprennent que les regles propres au reseau (renvoi du meme `id`, pas de Ctrl-U, delais). L'enveloppe H1 est du code pur teste avec les vecteurs de la spec 10.4 ; le trousseau est derriere un protocole (en memoire pour les tests).

**Tech Stack:** Swift 6 (concurrence stricte, avertissements = erreurs), SwiftUI, Network.framework (`NWConnection`), CryptoKit (HMAC-SHA256), Security (`SecItem`), Swift Testing, XcodeGen ; Python 3 pour `tools/halo_udp.py`.

**Spec:** `docs/superpowers/specs/2026-09-25-source-reseau-app-design.md` (a lire avec ce plan) ; protocole : `docs/PROTOCOLE-JSON.md` section 10.

**Ecarts a la spec (assumes) :**
1. `TransportUDP` et `ErreurReseau` vont dans le framework `HaloProtocole` (dossiers `Transport/` et `Reseau/`), pas dans l'app. Raison : leurs tests ont besoin d'un pair UDP local qui recoit des datagrammes non sollicites ; les tests de l'app tournent dans l'app sandboxee, ou il faudrait le droit `network.server`, alors que `HaloProtocoleTests` tourne hors sandbox. Le code reste sans AppKit (il servira a iOS).
2. Sans reponse a `json cle nouvelle`, l'app ne relit pas `json cle` : le bloc `ip` (toutes les 5 s par l'USB) donne deja l'empreinte du pont, et la carte Thread affiche alors "cle inconnue de ce Mac" avec "Nouvelle cle...".
3. L'alea de `json cle nouvelle` reste dans le suivi de la commande (memoire seulement), masque partout ou il s'affiche (`PolitiqueCommandes.masquerCle`, deja en place) ; il ne suffit pas a retrouver la cle (qui melange un alea de la carte). La cle, elle, est retiree du suivi des qu'elle est rangee.

## Global Constraints

- macOS 15.0 minimum ; Swift 6.0, `SWIFT_STRICT_CONCURRENCY: complete`, `SWIFT_TREAT_WARNINGS_AS_ERRORS: YES` (tout avertissement casse le build).
- Port UDP **5480**, IPv6 impose, hote `<nom>.local` (nom SRP : 16 hexa, bloc `ip`, `srp.nom`).
- H1 (10.4) : MAC = 16 premiers octets de HMAC-SHA256 en **32 hexa MAJUSCULES** ; `ctr` decimal sans zero de tete, 1..4294967295 ; fenetre anti-rejeu de 32 jugee **apres** le MAC ; sens `A` (app -> carte), `C` (carte -> app).
- Poignee de main : 3 essais, 2 s d'attente d'un DEFI juste chacun, `na` neuf a chaque essai ; connexion prete en 5 s au plus.
- Renvois (UDP seulement) : meme `id` a 2 s et 4 s, `sansReponse` a 6 s. USB : pas de renvoi, `sansReponse` a 3 s (inchange).
- Profil distant tel quel : l'app ne demande aucune cadence de plus (R3 non mesure).
- Cle : trousseau de session, mot de passe generique, service `fr.djoko.halo.pont`, compte = nom SRP (sans `.local`), valeur = 64 hexa MAJUSCULES, commentaire = empreinte, libelle `Halo - pont <nom>`, non synchronise. **Jamais** dans la console, le journal, les suivis de commandes, les preferences ou un fichier.
- Textes : chaque cible a sa fonction `tr(...)` et son catalogue (`HaloProtocole/Localizable.xcstrings`, `HaloCompagnon/Ressources/Localizable.xcstrings`) ; francais = source, anglais obligatoire ; specificateurs numerotes (`%1$@`, `%2$lld`) des qu'il y en a deux ; `LocalisationTests` doivent passer.
- Code : identifiants et commentaires en francais **sans accents** ; textes affiches avec accents. Tests en Swift Testing (`import Testing`, `@Suite`, `@Test`, `#expect`, `#require`).
- Commandes (depuis `apps/macos`) :
  - apres ajout ou retrait d'un fichier : `xcodegen generate` ;
  - une suite : `xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' -derivedDataPath build/dd test -only-testing:<Cible>/<Suite>` ;
  - tout : la meme sans `-only-testing` ;
  - catalogues apres un changement de texte : `I=build/dd/Build/Intermediates.noindex/HaloCompagnon.build/Debug` puis `xcrun xcstringstool sync HaloProtocole/Localizable.xcstrings --stringsdata $I/HaloProtocole.build/Objects-normal/arm64/*.stringsdata` et `xcrun xcstringstool sync HaloCompagnon/Ressources/*.xcstrings --stringsdata $I/HaloCompagnon.build/Objects-normal/arm64/*.stringsdata`, puis traductions par `python3 Outils/traduire.py` (tache 3).
- Commits : un par tache, message en francais sans accents, termine par la ligne `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` ; **seulement si Djoko a autorise les commits pour cette execution**. Jamais de push sans sa demande.
- Interdits pour les agents : flasher, ouvrir un port serie, `sudo`, changer une route ou un reglage reseau, ecrire dans le vrai trousseau hors du test explicitement active (tache 5).

## Carte des fichiers

Nouveaux :

| Fichier | Role |
|---|---|
| `apps/macos/HaloProtocole/Reseau/EnveloppeH1.swift` | `H1` (hexa, kid, SALUT, DEFI, Ks), `FenetreAntiRejeu`, `SessionH1` |
| `apps/macos/HaloProtocole/Reseau/CleReseau.swift` | creation de la cle par l'USB (commande, verification), `EtatAccesReseau` |
| `apps/macos/HaloProtocole/Reseau/ErreurReseau.swift` | erreurs typees du transport, textes, reprise |
| `apps/macos/HaloProtocole/Transport/TransportUDP.swift` | `Transport` sur `NWConnection` |
| `apps/macos/HaloCompagnon/Reseau/Trousseau.swift` | `PontConnu`, `TrousseauCles`, `TrousseauSysteme`, `TrousseauMemoire`, `ErreurTrousseau` |
| `apps/macos/HaloCompagnon/Reseau/AlerteReseau.swift` | alerte du bandeau (transport, trousseau, sans hello) |
| `apps/macos/HaloCompagnon/Ressources/InfoPlist.xcstrings` | texte de l'autorisation reseau local (fr, en) |
| `apps/macos/Signature.xcconfig` | signature ad hoc par defaut, `#include? "Local.xcconfig"` |
| `apps/macos/Outils/traduire.py` | ajoute des traductions a un catalogue, au format de Xcode |
| `apps/macos/HaloProtocoleTests/EnveloppeH1Tests.swift`, `ReseauSessionTests.swift`, `CleReseauTests.swift`, `TransportUDPTests.swift` | tests |
| `apps/macos/HaloCompagnonTests/TrousseauTests.swift`, `PontReseauTests.swift` | tests |
| `tools/test_halo_udp.py` | tests du client de banc |

Modifies : `HaloProtocole/Commandes/Correlateur.swift`, `HaloProtocole/Session/MoteurSession.swift`, `HaloCompagnon/Modele/Pont.swift`, `HaloCompagnon/Demo/SimulateurDemo.swift`, `HaloCompagnon/Vues/{ContenuPrincipal,TableauDeBord,Graphiques,TramesEnDirect}.swift`, `HaloCompagnon/HaloCompagnon.entitlements`, `project.yml`, `apps/macos/.gitignore`, `HaloProtocoleTests/LocalisationTests.swift`, `HaloCompagnonTests/DemoBoutEnBoutTests.swift`, `tools/halo_udp.py`, `apps/macos/README.md`, `docs/PROTOCOLE-JSON.md`, `docs/ETUDE-THREAD-COMPAGNON.md`.

---

### Task 1: Enveloppe H1 (code pur)

**Files:**
- Create: `apps/macos/HaloProtocole/Reseau/EnveloppeH1.swift`
- Test: `apps/macos/HaloProtocoleTests/EnveloppeH1Tests.swift`

**Interfaces:**
- Consumes: rien.
- Produces:
  - `public enum H1` : `static func hexa<S: Sequence>(_:) -> String where S.Element == UInt8` (MAJUSCULES) ; `static func octets(hexa: String) -> Data?` (MAJUSCULES seulement) ; `static func aleatoire(_ n: Int) -> Data` ; `static func kid(cle: Data) -> String` ; `static func salut(cle: Data, na: Data) -> Data` ; `static func verifierDefi(_: Data, cle: Data, na: Data) -> (sid: String, nc: String)?` ; `static func cleSession(cle: Data, na: Data, nc: String, sid: String) -> SymmetricKey` ; internes : `mac(_: SymmetricKey, _: Data) -> String`, `estHexa(_: Data, longueur: Int) -> Bool`, `egaux(_: Data, _: Data) -> Bool`.
  - `public struct FenetreAntiRejeu` : `init()`, `mutating func accepter(_ ctr: UInt32) -> Bool`, `haut: UInt32`.
  - `public struct SessionH1` : `init(sid: String, ks: SymmetricKey)`, `mutating func sceller(_ ligne: Data) -> Data`, `mutating func ouvrir(_ datagramme: Data) -> Data?`, `sid`, `ctrEmis: UInt32`, `ecartes: Int`.

- [ ] **Step 1: Write the failing test**

`apps/macos/HaloProtocoleTests/EnveloppeH1Tests.swift` :

```swift
import CryptoKit
import Foundation
import Testing
@testable import HaloProtocole

/// Vecteurs de docs/PROTOCOLE-JSON.md 10.4 (les memes que tools/host_tests/test_h1.cpp).
enum VecteursH1 {
    static let psk = Data((0..<32).map { UInt8($0) })
    static let na = Data((0xA0...0xAF).map { UInt8($0) })
    static let nc = "505152535455565758595A5B5C5D5E5F"
    static let sid = "1234ABCD"
    static let ks = "20D6D83D97ED44F2BBF8CE56389BD475CBE2B625CE6CE24768B6B4C1C625012F"
    static let salut = "H1 SALUT 630DCD29 A0A1A2A3A4A5A6A7A8A9AAABACADAEAF 52D853E3FFE9E9CCEFFA98BB5304B32D"
    static let defi = "H1 DEFI 1234ABCD 505152535455565758595A5B5C5D5E5F BFF13F71B42243E6017D2807F8E6171F"
    static let a1 = "H1 1234ABCD 1 FD97A0C9E604524B49C763452D0310CE id=1 json 1"
    static let chargeC1 = #"{"v":1,"t":"hb","n":7,"ms":1234}"#
    static let c1 = "H1 1234ABCD 1 347A2E6A129BC822ECFF39BEC910451C " + chargeC1

    static func session() -> SessionH1 {
        SessionH1(sid: sid, ks: H1.cleSession(cle: psk, na: na, nc: nc, sid: sid))
    }
}

func octets(_ s: String) -> Data { Data(s.utf8) }

@Suite("Enveloppe H1 (10.4)")
struct EnveloppeH1Tests {
    @Test func kidEtSalut() {
        #expect(H1.kid(cle: VecteursH1.psk) == "630DCD29")
        #expect(H1.salut(cle: VecteursH1.psk, na: VecteursH1.na) == octets(VecteursH1.salut))
    }

    @Test func hexa() {
        #expect(H1.hexa([0x00, 0xAB, 0x0F]) == "00AB0F")
        #expect(H1.octets(hexa: "00AB0F") == Data([0x00, 0xAB, 0x0F]))
        #expect(H1.octets(hexa: "00ab0f") == nil, "majuscules seulement")
        #expect(H1.octets(hexa: "ABC") == nil, "longueur impaire")
        #expect(H1.aleatoire(16).count == 16)
        #expect(H1.aleatoire(32) != H1.aleatoire(32))
    }

    @Test func defiEtCleDeSession() throws {
        let r = try #require(H1.verifierDefi(octets(VecteursH1.defi), cle: VecteursH1.psk, na: VecteursH1.na))
        #expect(r.sid == VecteursH1.sid)
        #expect(r.nc == VecteursH1.nc)
        let ks = H1.cleSession(cle: VecteursH1.psk, na: VecteursH1.na, nc: r.nc, sid: r.sid)
        #expect(ks.withUnsafeBytes { H1.hexa($0) } == VecteursH1.ks)
    }

    @Test func defiRefuse() {
        let autreNa = Data(repeating: 0x11, count: 16)
        #expect(H1.verifierDefi(octets(VecteursH1.defi), cle: VecteursH1.psk, na: autreNa) == nil, "autre na")
        let minuscules = VecteursH1.defi.replacingOccurrences(of: "BFF13F71B42243E6017D2807F8E6171F",
                                                              with: "bff13f71b42243e6017d2807f8e6171f")
        #expect(H1.verifierDefi(octets(minuscules), cle: VecteursH1.psk, na: VecteursH1.na) == nil, "minuscules")
        var faux = Array(VecteursH1.defi.utf8)
        faux[faux.count - 1] = UInt8(ascii: "E")
        #expect(H1.verifierDefi(Data(faux), cle: VecteursH1.psk, na: VecteursH1.na) == nil, "MAC faux")
        #expect(H1.verifierDefi(octets(VecteursH1.salut), cle: VecteursH1.psk, na: VecteursH1.na) == nil, "pas un DEFI")
    }

    @Test func scellerCommeLeFirmware() {
        var s = VecteursH1.session()
        #expect(s.sceller(octets("id=1 json 1")) == octets(VecteursH1.a1))
        #expect(s.ctrEmis == 1)
    }

    @Test func ouvrirUnMessageDeLaCarte() {
        var s = VecteursH1.session()
        #expect(s.ouvrir(octets(VecteursH1.c1)) == octets(VecteursH1.chargeC1))
        #expect(s.ouvrir(octets(VecteursH1.c1)) == nil, "rejeu")
        #expect(s.ecartes == 1)
    }

    @Test func formeCanonique() {
        var s = VecteursH1.session()
        let mac = "347A2E6A129BC822ECFF39BEC910451C"
        let c = VecteursH1.chargeC1
        let faux = [
            "H1 1234ABCD 01 \(mac) \(c)",              // zero de tete
            "H1 1234ABCD 0 \(mac) \(c)",               // ctr nul
            "H1 1234abcd 1 \(mac) \(c)",               // sid en minuscules
            "H1 1234ABCD 1 \(mac.lowercased()) \(c)",  // MAC en minuscules
            "H1 9999ABCD 1 \(mac) \(c)",               // autre session
            "H1 1234ABCD 1 \(mac)",                    // charge absente
        ]
        for f in faux { #expect(s.ouvrir(octets(f)) == nil, "\(f)") }
        #expect(s.ecartes == faux.count)
        #expect(s.ouvrir(octets(VecteursH1.c1)) != nil, "le vrai passe ensuite")
    }

    @Test func messageDeLAppNeSOuvrePasCommeMessageDeLaCarte() {
        var s = VecteursH1.session()
        #expect(s.ouvrir(octets(VecteursH1.a1)) == nil, "sens A presente comme C")
    }

    @Test func fenetre() {
        var f = FenetreAntiRejeu()
        #expect(!f.accepter(0))
        #expect(f.accepter(1))
        #expect(f.accepter(3))
        #expect(f.accepter(2), "desordre admis")
        #expect(!f.accepter(2), "rejeu")
        #expect(f.accepter(40), "saut")
        #expect(!f.accepter(8), "trop ancien : 40 - 8 >= 32")
        #expect(f.accepter(9), "dans la fenetre : 40 - 9 = 31")
        #expect(f.haut == 40)
    }

    @Test func macFauxNePoussePasLaFenetre() {
        var s = VecteursH1.session()
        #expect(s.ouvrir(octets("H1 1234ABCD 1000 00000000000000000000000000000000 {}")) == nil)
        #expect(s.ouvrir(octets(VecteursH1.c1)) != nil, "ctr 1 encore admis : la fenetre n'a pas bouge")
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodegen generate && xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' -derivedDataPath build/dd test -only-testing:HaloProtocoleTests/EnveloppeH1Tests`
Expected: echec de compilation (`cannot find 'H1' in scope`).

- [ ] **Step 3: Write minimal implementation**

`apps/macos/HaloProtocole/Reseau/EnveloppeH1.swift` :

```swift
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: la commande du Step 2.
Expected: `EnveloppeH1Tests` : tous les tests passent (vecteurs a l'octet pres). Si `SymmetricKey` ou `HashedAuthenticationCode` ne se convertit pas comme ecrit, corriger l'appel (pas les vecteurs).

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloProtocole/Reseau/EnveloppeH1.swift apps/macos/HaloProtocoleTests/EnveloppeH1Tests.swift
git commit -m "Ajouter l'enveloppe H1 du transport reseau a l'app

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Regles du reseau dans le correlateur et le moteur de session

**Files:**
- Modify: `apps/macos/HaloProtocole/Commandes/Correlateur.swift` (constante `delaiReponse` l.68, `SuiviCommande` l.46-62, `verifierDelais` l.275-282)
- Modify: `apps/macos/HaloProtocole/Session/MoteurSession.swift` (`Parametres` l.24-47, `ouvert` l.167-180, `reessayer` l.212-216, `tic` l.248-318, `envoyerJson1` l.329-335, `traiter` `.reponse` l.398-409)
- Test: `apps/macos/HaloProtocoleTests/ReseauSessionTests.swift`

**Interfaces:**
- Consumes: `GenreTransport` (existant), helpers de test `texte(_:)`, `reponse(...)` (CorrelationTests.swift) et `MoteurSessionTests.element/envois/finJson1/hello` (existants).
- Produces:
  - `public struct PolitiqueDelais: Sendable, Equatable { delaiRenvoi, renvois, delaiReponse; static let usb, reseau; static func pour(_: GenreTransport) }`.
  - `Correlateur.politique: PolitiqueDelais` (defaut `.usb`), `Correlateur.renvoisDus(maintenant:) -> [Data]`, `SuiviCommande.renvois: Int`.
  - `MoteurSession.ouvert(maintenant:genre:)` (`genre` defaut `.usb`), `MoteurSession.genre`, `Parametres.delaiFinJson1Reseau` (8 s).

- [ ] **Step 1: Write the failing test**

`apps/macos/HaloProtocoleTests/ReseauSessionTests.swift` :

```swift
import Foundation
import Testing
@testable import HaloProtocole

@Suite("Regles du reseau (10.2)")
struct ReseauSessionTests {
    typealias M = MoteurSessionTests

    @Test func renvoisDuMemeIdPuisSansReponse() throws {
        var c = Correlateur()
        c.politique = .reseau
        let a = c.soumettre("lampe auto", origine: .interface, maintenant: 0)
        let e = try #require(c.prochainEnvoi(maintenant: 0))
        #expect(texte(e.octets) == "id=1 lampe auto\n")
        #expect(c.renvoisDus(maintenant: 1.9).isEmpty)
        #expect(c.renvoisDus(maintenant: 2.0).map(texte) == ["id=1 lampe auto\n"])
        #expect(c.renvoisDus(maintenant: 3.9).isEmpty)
        #expect(c.renvoisDus(maintenant: 4.0).map(texte) == ["id=1 lampe auto\n"])
        #expect(c.renvoisDus(maintenant: 5.9).isEmpty, "deux renvois au plus")
        #expect(c.verifierDelais(maintenant: 5.9).isEmpty)
        #expect(c.verifierDelais(maintenant: 6.0).map(\.id) == [a])
        #expect(c.suivi(a)?.etat == .sansReponse)
        #expect(c.suivi(a)?.renvois == 2)
    }

    @Test func reponseArreteLesRenvoisEtDoublonIgnore() {
        var c = Correlateur()
        c.politique = .reseau
        _ = c.soumettre("json ping", origine: .session, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.renvoisDus(maintenant: 2)
        #expect(c.recevoir(reponse(1), maintenant: 2.3) != .inattendue)
        #expect(c.renvoisDus(maintenant: 4).isEmpty)
        #expect(c.recevoir(reponse(1), maintenant: 2.4) == .inattendue, "reponse rejouee depuis le cache : ignoree")
    }

    @Test func usbSansRenvoi() {
        var c = Correlateur()
        let a = c.soumettre("lampe on", origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        #expect(c.renvoisDus(maintenant: 2.5).isEmpty)
        #expect(c.verifierDelais(maintenant: 3).map(\.id) == [a])
    }

    @Test func ouvertureReseauSansCtrlU() {
        var m = MoteurSession()
        #expect(M.envois(m.ouvert(maintenant: 0, genre: .udp)) == ["id=1 json 1\n"])
        #expect(m.correlateur.politique == .reseau)
        var u = MoteurSession()
        #expect(M.envois(u.ouvert(maintenant: 0)) == ["\u{15}\n", "id=1 json 1\n"])
        #expect(u.correlateur.politique == .usb)
    }

    @Test func json1RenvoyeAvecLeMemeIdADistance() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        #expect(M.envois(m.tic(maintenant: 2.0)) == ["id=1 json 1\n"], "la carte ne refait pas l'instantane")
        var u = MoteurSession()
        _ = u.ouvert(maintenant: 0)
        #expect(M.envois(u.tic(maintenant: 2.0)) == ["id=2 json 1\n"], "USB : inchange")
    }

    @Test func finDuJson1AttendPlusLongtempsADistance() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        _ = m.recu(M.element(M.hello), maintenant: 0.3)
        #expect(!m.tic(maintenant: 3.5).contains(.note(.reponseJson1Perdue)), "instantane de ~6 Ko sur Thread")
        #expect(m.instantaneEnCours)
        _ = m.recu(M.element(M.hello), maintenant: 5.0)  // la carte parle : pas de silence
        #expect(m.tic(maintenant: 8.0).contains(.note(.reponseJson1Perdue)))
    }

    @Test func listeBlancheRefuseLaCleEtLeRedemarrage() {
        for c in ["json cle", "json cle nouvelle " + String(repeating: "AB", count: 32), "json cle efface", "reboot"] {
            #expect(!PolitiqueCommandes.autoriseeADistance(c), "\(c)")
            if case .interdite = PolitiqueCommandes.verdictConsole(c, transport: .udp) {} else {
                Issue.record("\(c) doit etre refusee a distance")
            }
        }
    }

    @Test func dejaTraiteDemandeUnEtat() {
        var m = MoteurSession()
        _ = m.ouvert(maintenant: 0, genre: .udp)
        _ = m.recu(M.element(M.hello), maintenant: 0.1)
        _ = m.recu(M.finJson1(), maintenant: 0.2)
        let (_, e1) = m.soumettre("lampe auto", origine: .interface, maintenant: 0.3)
        #expect(M.envois(e1) == ["id=2 lampe auto\n"])
        let deja = M.element(#"{"v":1,"t":"reponse","n":12,"ms":83530,"id":2,"etape":"fin","cmd":"lampe auto","ok":false,"code":"deja_traite","duree_ms":0}"#)
        #expect(M.envois(m.recu(deja, maintenant: 0.5)) == ["id=3 json etat\n"])
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodegen generate && xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' -derivedDataPath build/dd test -only-testing:HaloProtocoleTests/ReseauSessionTests`
Expected: echec de compilation (`value of type 'Correlateur' has no member 'politique'`).

- [ ] **Step 3: Write minimal implementation**

Dans `Correlateur.swift`, avant `public struct Correlateur`, ajouter :

```swift
/// Delais de la correlation selon le transport (6.5, 10.2). L'USB ne perd
/// rien : pas de renvoi, "sans reponse" a 3 s. Le reseau perd : une commande
/// sans aucune reponse repart avec le meme `id` a 2 s puis 4 s (la carte
/// repond depuis son cache sans reexecuter), "sans reponse" a 6 s.
public struct PolitiqueDelais: Sendable, Equatable {
    public var delaiRenvoi: TimeInterval
    public var renvois: Int
    public var delaiReponse: TimeInterval

    public static let usb = PolitiqueDelais(delaiRenvoi: 0, renvois: 0, delaiReponse: 3)
    public static let reseau = PolitiqueDelais(delaiRenvoi: 2, renvois: 2, delaiReponse: 6)

    public static func pour(_ genre: GenreTransport) -> PolitiqueDelais {
        genre == .udp ? .reseau : .usb
    }
}
```

Dans `SuiviCommande`, apres `termineeA` :

```swift
    /// Renvois du meme `id` (reseau, 10.2).
    public internal(set) var renvois = 0
```

Dans `Correlateur` : remplacer `public static let delaiReponse: TimeInterval = 3` par

```swift
    /// Delais en vigueur : USB par defaut ; le moteur de session regle le reseau.
    public var politique = PolitiqueDelais.usb
```

remplacer dans `verifierDelais` `maintenant - t >= Self.delaiReponse` par `maintenant - t >= politique.delaiReponse` (et le commentaire "sous 3 s" par "sous le delai de la politique"), puis ajouter apres `verifierDelais` :

```swift
    /// A distance (10.2) : la commande en vol sans aucune `reponse` repart,
    /// memes octets (meme `id`), a `delaiRenvoi` puis a 2 x `delaiRenvoi`.
    public mutating func renvoisDus(maintenant: TimeInterval) -> [Data] {
        guard politique.renvois > 0, let id = enVol, let i = index(id), suivis[i].etat == .envoyee,
              suivis[i].renvois < politique.renvois, let t = suivis[i].envoyeeA, let n = suivis[i].numero,
              maintenant - t >= politique.delaiRenvoi * Double(suivis[i].renvois + 1),
              case .success(let octets) = LigneCommande.octets(suivis[i].commande, id: n)
        else { return [] }
        suivis[i].renvois += 1
        dernierEnvoiA = maintenant
        return [octets]
    }
```

Dans `MoteurSession` :

1. `Parametres`, apres `delaiFinJson1` :
```swift
        /// A distance : l'instantane de `json 1` (~6 Ko) passe a 3 Ko/s sur
        /// Thread, ~4 s avec deux sessions (10.2) ; sa `fin` est attendue 8 s.
        public var delaiFinJson1Reseau: TimeInterval = 8
```
2. Apres `public private(set) var phase` :
```swift
    /// Transport de la connexion en cours (regles du reseau, 10.2).
    public private(set) var genre: GenreTransport = .usb
```
3. `ouvert` devient :
```swift
    /// Transport ouvert : `\x15\n` puis `id=1 json 1` (3.3). A distance, pas de
    /// Ctrl-U (pas de ligne en cours a effacer ; la carte ignore une ligne sans id).
    public mutating func ouvert(maintenant: TimeInterval, genre: GenreTransport = .usb) -> [Effet] {
        self.genre = genre
        correlateur.politique = .pour(genre)
        correlateur.reinitialiser(maintenant: maintenant)
        statistiques.connexions += 1
        phase = .attenteHello(essai: 1)
        historique = true
        dernierN = nil
        ouvertA = maintenant
        dernierRecuA = maintenant
        resynchroDepuis = nil
        bancSignale = false
        return effacement + envoyerJson1(maintenant: maintenant)
    }

    private var effacement: [Effet] { genre == .udp ? [] : [.envoyer(LigneCommande.effacement)] }
```
4. `reessayer` : `return effacement + envoyerJson1(maintenant: maintenant)`.
5. `tic`, cas `.attenteHello` : `effets += envoyerJson1(maintenant: maintenant, renvoi: true)` ; cas `.sansReponse` : remplacer `effets.append(.envoyer(LigneCommande.effacement))` par `effets += effacement` ; cas `.connecte` : remplacer `maintenant - j.envoyeA >= parametres.delaiFinJson1` par
```swift
            let delaiFin = genre == .udp ? parametres.delaiFinJson1Reseau : parametres.delaiFinJson1
            if let j = json1, maintenant - j.envoyeA >= delaiFin {
```
   et dans le bloc `if phase.modeMachine`, avant la boucle `verifierDelais` :
```swift
            for d in correlateur.renvoisDus(maintenant: maintenant) { effets.append(.envoyer(d)) }
```
6. `envoyerJson1` :
```swift
    /// `json 1`. A distance, un renvoi garde son `id` : si le premier est
    /// arrive, la carte ne refait pas l'instantane (10.2).
    private mutating func envoyerJson1(maintenant: TimeInterval, renvoi: Bool = false) -> [Effet] {
        let n: Int
        if renvoi, genre == .udp, let j = json1 { n = j.numero } else { n = correlateur.reserverNumero() }
        json1 = (n, maintenant)
        dernierEssaiA = maintenant
        correlateur.noterEnvoiHorsFile(maintenant: maintenant)
        return [.envoyer(Data("id=\(n) json 1\n".utf8))]
    }
```
7. `traiter`, cas `.reponse` : remplacer la branche `else if case .fin(...)` par
```swift
            } else if case .fin(let id, _) = correlateur.recevoir(r, maintenant: maintenant) {
                if r.ok, let s = correlateur.suivi(id) { appliquerReglage(s.commande) }
                // Reseau : id deja traite, sa reponse n'est plus en cache (10.2) : rafraichir l'etat.
                if r.code == .dejaTraite { correlateur.soumettre("json etat", origine: .session, maintenant: maintenant) }
            }
```

- [ ] **Step 4: Run test to verify it passes**

Run: la commande du Step 2, puis toute la cible : `... test -only-testing:HaloProtocoleTests`.
Expected: `ReseauSessionTests` passe ; les suites existantes (`CorrelateurTests`, `MoteurSessionTests`) passent toujours (l'USB est inchange).

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloProtocole/Commandes/Correlateur.swift apps/macos/HaloProtocole/Session/MoteurSession.swift apps/macos/HaloProtocoleTests/ReseauSessionTests.swift
git commit -m "Renvoyer une commande reseau avec le meme id, sans Ctrl-U

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Cle reseau (commande, verification, etat de l'acces) et outil de traduction

**Files:**
- Create: `apps/macos/HaloProtocole/Reseau/CleReseau.swift`
- Create: `apps/macos/Outils/traduire.py`
- Modify: `apps/macos/HaloProtocole/Localizable.xcstrings` (par sync + outil)
- Test: `apps/macos/HaloProtocoleTests/CleReseauTests.swift`

**Interfaces:**
- Consumes: `H1.hexa`, `H1.octets(hexa:)`, `H1.aleatoire`, `H1.kid` (tache 1) ; `Reponse` (`ok`, `code`, `msg`, `cle`, `empreinte`), `ReseauIp` (existants).
- Produces:
  - `public enum CleReseau` : `static func alea() -> Data` (32 octets), `static func commande(alea: Data) -> String`, `struct Creee { cle: Data; empreinte: String }`, `enum Erreur { refusee(String), cleIllisible, empreinteIncoherente }` (`CustomStringConvertible`), `static func verifier(_: Reponse) -> Result<Creee, Erreur>`.
  - `public enum EtatAccesReseau: Equatable { inconnu, sansCle(nom:), cleConnue(nom:empreinte:), cleInconnue(nom:empreinte:) ; static func depuis(ip: ReseauIp?, empreinteDuMac: (String) -> String?) -> EtatAccesReseau }`.
  - `Outils/traduire.py <catalogue> <traductions.json>`.

- [ ] **Step 1: Write the failing test**

`apps/macos/HaloProtocoleTests/CleReseauTests.swift` :

```swift
import Foundation
import Testing
@testable import HaloProtocole

@Suite("Cle du transport reseau (10.4)")
struct CleReseauTests {
    static func reponseCle(ok: Bool = true, code: CodeReponse = .ok, cle: String?, empreinte: String?) -> Reponse {
        Reponse(id: 7, etape: .fin, cmd: "json cle nouvelle", ok: ok, code: code, msg: ok ? nil : "tampon USB occupe",
                dureeMs: 1, suite: nil, consigne: nil, aLivrer: nil, version: nil, bailS: nil, upS: nil,
                cle: cle, empreinte: empreinte)
    }

    @Test func commande() {
        let c = CleReseau.commande(alea: Data(repeating: 0xAB, count: 32))
        #expect(c == "json cle nouvelle " + String(repeating: "AB", count: 32))
        if case .failure(let e) = LigneCommande.valider(c, id: LigneCommande.idMax) { Issue.record("\(e)") }
        #expect(CleReseau.alea().count == 32)
    }

    @Test func verifierUneBonneCle() throws {
        let r = Self.reponseCle(cle: H1.hexa(VecteursH1.psk), empreinte: "630DCD29")
        let c = try CleReseau.verifier(r).get()
        #expect(c.cle == VecteursH1.psk)
        #expect(c.empreinte == "630DCD29")
    }

    @Test func refus() {
        #expect(CleReseau.verifier(Self.reponseCle(ok: false, code: .refuse, cle: nil, empreinte: nil))
                == .failure(.refusee("tampon USB occupe")))
        #expect(CleReseau.verifier(Self.reponseCle(cle: nil, empreinte: "630DCD29")) == .failure(.cleIllisible))
        #expect(CleReseau.verifier(Self.reponseCle(cle: H1.hexa(VecteursH1.psk).lowercased(), empreinte: "630DCD29"))
                == .failure(.cleIllisible))
        #expect(CleReseau.verifier(Self.reponseCle(cle: H1.hexa(VecteursH1.psk), empreinte: "00000000"))
                == .failure(.empreinteIncoherente))
    }

    @Test func etatDeLAcces() {
        func ip(_ nom: String?, ouvert: Bool?, empreinte: String?) -> ReseauIp {
            ReseauIp(srp: .init(nom: nom), udp: .init(port: 5480, ouvert: ouvert, empreinte: empreinte))
        }
        let mac: (String) -> String? = { $0 == "56B1E064401F74EF" ? "630DCD29" : nil }
        #expect(EtatAccesReseau.depuis(ip: nil, empreinteDuMac: mac) == .inconnu)
        #expect(EtatAccesReseau.depuis(ip: ip(nil, ouvert: true, empreinte: "630DCD29"), empreinteDuMac: mac) == .inconnu)
        #expect(EtatAccesReseau.depuis(ip: ReseauIp(srp: .init(nom: "56B1E064401F74EF")), empreinteDuMac: mac) == .inconnu,
                "firmware sans transport reseau (pas de bloc udp)")
        #expect(EtatAccesReseau.depuis(ip: ip("56B1E064401F74EF", ouvert: false, empreinte: nil), empreinteDuMac: mac)
                == .sansCle(nom: "56B1E064401F74EF"))
        #expect(EtatAccesReseau.depuis(ip: ip("56B1E064401F74EF", ouvert: true, empreinte: "630DCD29"), empreinteDuMac: mac)
                == .cleConnue(nom: "56B1E064401F74EF", empreinte: "630DCD29"))
        #expect(EtatAccesReseau.depuis(ip: ip("56B1E064401F74EF", ouvert: true, empreinte: "B64D84FB"), empreinteDuMac: mac)
                == .cleInconnue(nom: "56B1E064401F74EF", empreinte: "B64D84FB"))
    }
}
```

(`ReseauIp`, `ReseauIp.Srp` et `ReseauIp.Udp` n'ont que des proprietes optionnelles : leurs initialiseurs par membres, internes, acceptent les seuls champs donnes.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodegen generate && xcodebuild ... -derivedDataPath build/dd test -only-testing:HaloProtocoleTests/CleReseauTests`
Expected: echec de compilation (`cannot find 'CleReseau' in scope`).

- [ ] **Step 3: Write minimal implementation**

`apps/macos/HaloProtocole/Reseau/CleReseau.swift` :

```swift
import Foundation

/// Cle partagee du transport reseau, creee par l'USB (10.4) : l'app fournit
/// un alea, la carte calcule `cle = HMAC-SHA256(alea_app, alea_carte)` et la
/// rend une seule fois dans la reponse.
public enum CleReseau {
    /// Alea de l'app : 32 octets d'un generateur cryptographique.
    public static func alea() -> Data { H1.aleatoire(32) }

    /// `json cle nouvelle <64 HEXA>` (USB seulement ; la carte la refuse a distance).
    public static func commande(alea: Data) -> String {
        "json cle nouvelle \(H1.hexa(alea))"
    }

    public struct Creee: Sendable, Equatable {
        public var cle: Data
        public var empreinte: String
    }

    public enum Erreur: Error, Sendable, Equatable, CustomStringConvertible {
        /// `ok` faux (tampon USB occupe...) : rien n'a change sur la carte.
        case refusee(String)
        case cleIllisible
        case empreinteIncoherente

        public var description: String {
            switch self {
            case .refusee(let msg): tr("La carte refuse la nouvelle clé : \(msg)")
            case .cleIllisible: tr("Réponse sans clé lisible (64 hexa majuscules attendus) : clé non rangée.")
            case .empreinteIncoherente: tr("Empreinte incohérente avec la clé reçue : clé non rangée.")
            }
        }
    }

    /// Reponse `fin` a `json cle nouvelle` : la cle et son empreinte, verifiees.
    public static func verifier(_ r: Reponse) -> Result<Creee, Erreur> {
        guard r.ok else { return .failure(.refusee(r.msg ?? r.code.rawValue)) }
        guard let texte = r.cle, let cle = H1.octets(hexa: texte), cle.count == 32 else { return .failure(.cleIllisible) }
        guard let e = r.empreinte, e == H1.kid(cle: cle) else { return .failure(.empreinteIncoherente) }
        return .success(Creee(cle: cle, empreinte: e))
    }
}

/// Acces reseau vu par l'USB : bloc `ip` de la carte, et cles de ce Mac.
public enum EtatAccesReseau: Sendable, Equatable {
    /// Pas de bloc `ip` (build sans Thread, pas encore recu), nom SRP inconnu,
    /// ou firmware sans transport reseau (pas de `udp`).
    case inconnu
    /// Le pont n'a pas de cle : transport reseau coupe.
    case sansCle(nom: String)
    /// La cle du pont est celle de ce Mac.
    case cleConnue(nom: String, empreinte: String)
    /// Le pont a une cle que ce Mac n'a pas (autre Mac, `halo_udp.py`, cle recreee).
    case cleInconnue(nom: String, empreinte: String)

    public static func depuis(ip: ReseauIp?, empreinteDuMac: (String) -> String?) -> EtatAccesReseau {
        guard let nom = ip?.srp?.nom, !nom.isEmpty, let udp = ip?.udp else { return .inconnu }
        guard udp.ouvert == true, let e = udp.empreinte else { return .sansCle(nom: nom) }
        return empreinteDuMac(nom) == e ? .cleConnue(nom: nom, empreinte: e) : .cleInconnue(nom: nom, empreinte: e)
    }
}
```

`apps/macos/Outils/traduire.py` :

```python
#!/usr/bin/env python3
"""Traductions d'un catalogue .xcstrings, au format exact de Xcode.

  python3 Outils/traduire.py <catalogue.xcstrings> <traductions.json>

traductions.json : {"<cle francaise>": "<anglais>", ...}. Chaque cle recoit
son anglais et son francais (la cle elle-meme), en specificateurs numerotes
(%1$@, %2$lld...) des qu'il y en a deux et qu'aucun ne l'est deja. Les cles
perimees laissees par `xcstringstool sync` (extractionState "stale") sont
retirees : les tests de LocalisationTests les refusent.
"""
import json
import re
import sys

SPEC = re.compile(r"%(?:\d+\$)?(lld|ld|d|@|lf|f)")


def numeroter(s):
    if re.search(r"%\d+\$", s) or len(SPEC.findall(s)) < 2:
        return s
    rang = iter(range(1, 100))
    return SPEC.sub(lambda m: f"%{next(rang)}${m.group(1)}", s)


def unite(valeur):
    return {"stringUnit": {"state": "translated", "value": valeur}}


def main(chemin, fichier):
    with open(chemin, encoding="utf-8") as f:
        d = json.load(f)
    with open(fichier, encoding="utf-8") as f:
        traductions = json.load(f)
    cles = d["strings"]
    for k in [k for k, e in cles.items() if e.get("extractionState") == "stale"]:
        del cles[k]
    for cle, anglais in traductions.items():
        locs = cles.setdefault(cle, {}).setdefault("localizations", {})
        locs["fr"] = unite(numeroter(cle))
        locs["en"] = unite(numeroter(anglais))
    texte = json.dumps(d, ensure_ascii=False, indent=2, separators=(",", " : "), sort_keys=True)
    with open(chemin, "w", encoding="utf-8") as f:
        f.write(texte + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
```

Puis les textes du framework :
1. Compiler (`xcodebuild ... -derivedDataPath build/dd build`), puis `I=build/dd/Build/Intermediates.noindex/HaloCompagnon.build/Debug && xcrun xcstringstool sync HaloProtocole/Localizable.xcstrings --stringsdata $I/HaloProtocole.build/Objects-normal/arm64/*.stringsdata`.
2. Ecrire `build/traductions-t3.json` :
```json
{
  "La carte refuse la nouvelle clé : %@": "The board refuses the new key: %@",
  "Réponse sans clé lisible (64 hexa majuscules attendus) : clé non rangée.": "Reply without a readable key (64 uppercase hex digits expected): key not stored.",
  "Empreinte incohérente avec la clé reçue : clé non rangée.": "Fingerprint does not match the received key: key not stored."
}
```
3. `python3 Outils/traduire.py HaloProtocole/Localizable.xcstrings build/traductions-t3.json`.

- [ ] **Step 4: Run test to verify it passes**

Run: `... test -only-testing:HaloProtocoleTests/CleReseauTests -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: les deux suites passent (catalogue aligne, anglais present). `git diff --stat HaloProtocole/Localizable.xcstrings` ne montre que les 3 cles ajoutees.

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloProtocole/Reseau/CleReseau.swift apps/macos/HaloProtocoleTests/CleReseauTests.swift apps/macos/Outils/traduire.py apps/macos/HaloProtocole/Localizable.xcstrings
git commit -m "Verifier la cle reseau creee par l'USB et l'etat de l'acces

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Transport UDP et erreurs du reseau

**Files:**
- Create: `apps/macos/HaloProtocole/Reseau/ErreurReseau.swift`
- Create: `apps/macos/HaloProtocole/Transport/TransportUDP.swift`
- Modify: `apps/macos/HaloProtocole/Localizable.xcstrings` (sync + outil)
- Test: `apps/macos/HaloProtocoleTests/TransportUDPTests.swift`

**Interfaces:**
- Consumes: `Transport`, `EvenementTransport`, `ErreurTransport`, `GenreTransport`, `Octets` (existants) ; `H1`, `SessionH1` (tache 1).
- Produces:
  - `public enum ErreurReseau: Error, Equatable { reseauLocalRefuse, pasDeRoute, nomIntrouvable(String), portInjoignable, aucunDefi, cheminPerdu(String), autre(String) ; var repriseAutomatique: Bool ; var description: String ; static func depuis(_: NWError, chemin: NWPath?, hote: String) -> ErreurReseau }`.
  - `public final class TransportUDP: Transport` : `struct Reglages { port = 5480, attentePret = .seconds(5), attenteDefi = .seconds(2), essais = 3 }`, `init(hote: String, cle: Data, reglages: Reglages = Reglages())`, `ecartes: Int`, `genre == .udp`, `nom == hote`.

- [ ] **Step 1: Write the failing test**

`apps/macos/HaloProtocoleTests/TransportUDPTests.swift` (le pair joue le cote pont du deroulement normal, en C sur des sockets BSD, hors sandbox) :

```swift
import CryptoKit
import Darwin
import Foundation
import Synchronization
import Testing
@testable import HaloProtocole

/// Pont local sur [::1] : repond au SALUT par un DEFI, ouvre les messages A,
/// scelle des lignes C. `muet` : ne repond jamais.
final class PontLocal: Sendable {
    let port: UInt16
    private let fd: Int32
    private let cle: Data
    private let muet: Bool
    private struct Etat {
        var saluts = 0
        var recues: [String] = []
        var session: (sid: String, ks: SymmetricKey, ctr: UInt32)?
        var pair: sockaddr_in6?
        var fini = false
    }
    private let etat = Mutex(Etat())

    init(cle: Data, muet: Bool = false) throws {
        self.cle = cle
        self.muet = muet
        fd = socket(AF_INET6, SOCK_DGRAM, 0)
        var a = sockaddr_in6()
        a.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        a.sin6_family = sa_family_t(AF_INET6)
        a.sin6_addr = in6addr_loopback
        let lie = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } }
        try #require(lie == 0)
        var l = socklen_t(MemoryLayout<sockaddr_in6>.size)
        _ = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            getsockname(fd, $0, &l) } }
        port = UInt16(bigEndian: a.sin6_port)
        var tv = timeval(tv_sec: 0, tv_usec: 100_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        let moi = self
        Thread.detachNewThread { moi.boucle() }
    }

    var saluts: Int { etat.withLock { $0.saluts } }
    var recues: [String] { etat.withLock { $0.recues } }

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
            etat.withLock { $0.saluts += 1 }
            guard !muet, champs[2] == H1.kid(cle: cle), let na = H1.octets(hexa: champs[3]) else { return }
            let sid = "5A5A0001", nc = H1.hexa(H1.aleatoire(16))
            let m = H1.mac(SymmetricKey(data: cle), Data("H1|DEFI|\(champs[2])|\(champs[3])|\(nc)|\(sid)".utf8))
            etat.withLock { $0.session = (sid, H1.cleSession(cle: cle, na: na, nc: nc, sid: sid), 0); $0.pair = de }
            var pair = de
            let defi = Data("H1 DEFI \(sid) \(nc) \(m)".utf8)
            _ = defi.withUnsafeBytes { b in withUnsafePointer(to: &pair) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(fd, b.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } } }
            return
        }
        // Message A : MAC verifie avec Ks, charge gardee.
        etat.withLock { e in
            guard let s = e.session, champs.count == 5, champs[1] == s.sid else { return }
            let attendu = H1.mac(s.ks, Data("A|\(s.sid)|\(champs[2])|".utf8) + Data(champs[4].utf8))
            if attendu == champs[3] { e.recues.append(champs[4]); e.pair = de }
        }
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
    }

    @Test func autreCle() async throws {
        let pont = try PontLocal(cle: Data(repeating: 7, count: 32))
        defer { pont.arreter() }
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(pont.port))
        await #expect(throws: ErreurReseau.aucunDefi) { _ = try await t.ouvrir() }
    }

    @Test func portFerme() async throws {
        // Un port libre puis ferme : l'ICMPv6 "port injoignable" remonte.
        let libre = try PontLocal(cle: VecteursH1.psk)
        let port = libre.port
        libre.arreter()
        try await Task.sleep(for: .milliseconds(300))  // la boucle du pair ferme sa socket
        let t = TransportUDP(hote: "::1", cle: VecteursH1.psk, reglages: Self.rapides(port))
        await #expect(throws: ErreurReseau.portInjoignable) { _ = try await t.ouvrir() }
    }

    @Test func textes() {
        #expect(!ErreurReseau.portInjoignable.repriseAutomatique)
        #expect(!ErreurReseau.reseauLocalRefuse.repriseAutomatique)
        #expect(ErreurReseau.pasDeRoute.repriseAutomatique)
        #expect(ErreurReseau.depuis(.posix(.EHOSTUNREACH), chemin: nil, hote: "x.local") == .pasDeRoute)
        #expect(ErreurReseau.depuis(.posix(.ECONNREFUSED), chemin: nil, hote: "x.local") == .portInjoignable)
        #expect(ErreurReseau.depuis(.dns(-65554), chemin: nil, hote: "x.local") == .nomIntrouvable("x.local"))
        #expect(ErreurReseau.depuis(.dns(-65570), chemin: nil, hote: "x.local") == .reseauLocalRefuse)
    }
}
```

(`-65554` : `kDNSServiceErr_NoSuchRecord` ; `-65570` : `kDNSServiceErr_PolicyDenied`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodegen generate && xcodebuild ... -derivedDataPath build/dd test -only-testing:HaloProtocoleTests/TransportUDPTests`
Expected: echec de compilation (`cannot find 'TransportUDP' in scope`).

- [ ] **Step 3: Write minimal implementation**

`apps/macos/HaloProtocole/Reseau/ErreurReseau.swift` :

```swift
import Foundation
import Network
import dnssd

/// Echec du transport reseau (10.1 a 10.4), avec son texte et sa reprise.
public enum ErreurReseau: Error, Sendable, Equatable, CustomStringConvertible {
    /// Autorisation "reseau local" refusee (Reglages Systeme).
    case reseauLocalRefuse
    /// Pas de route IPv6 vers le reseau Thread (bug du noyau de macOS, 10.1).
    case pasDeRoute
    /// `<nom>.local` introuvable.
    case nomIntrouvable(String)
    /// ICMPv6 "port injoignable" : le pont n'a plus de cle (port 5480 ferme).
    case portInjoignable
    /// Aucun DEFI juste apres les essais du SALUT (autre cle, pont muet).
    case aucunDefi
    /// Connexion perdue apres son ouverture.
    case cheminPerdu(String)
    case autre(String)

    /// Vrai : la reconnexion reessaie seule ; faux : il faut l'utilisateur
    /// (ou un changement du reseau).
    public var repriseAutomatique: Bool {
        switch self {
        case .reseauLocalRefuse, .portInjoignable: false
        default: true
        }
    }

    public var description: String {
        switch self {
        case .reseauLocalRefuse:
            tr("Accès au réseau local refusé : Réglages Système › Confidentialité et sécurité › Réseau local › Halo Compagnon.")
        case .pasDeRoute:
            tr("Pas de route IPv6 vers le réseau Thread (bug du noyau de macOS, section 10.1).")
        case .nomIntrouvable(let hote):
            tr("Pont introuvable (\(hote)) : éteint, hors du réseau Thread, ou routeurs de bordure injoignables.")
        case .portInjoignable:
            tr("Le pont n'a plus de clé : le brancher en USB, puis « Activer l'accès réseau ».")
        case .aucunDefi:
            tr("Aucune réponse du pont : clé différente de la sienne ? (comparer les empreintes par l'USB)")
        case .cheminPerdu(let raison):
            tr("Connexion réseau perdue : \(raison)")
        case .autre(let raison):
            tr("Erreur réseau : \(raison)")
        }
    }

    /// Erreur de Network.framework ; `chemin` : dernier chemin connu de la connexion.
    public static func depuis(_ e: NWError, chemin: NWPath?, hote: String) -> ErreurReseau {
        if chemin?.unsatisfiedReason == .localNetworkDenied { return .reseauLocalRefuse }
        switch e {
        case .posix(let code):
            switch code {
            case .EHOSTUNREACH, .ENETUNREACH, .ENETDOWN, .EHOSTDOWN: return .pasDeRoute
            case .ECONNREFUSED: return .portInjoignable
            default: return .autre(String(describing: code))
            }
        case .dns(let code):
            return Int(code) == kDNSServiceErr_PolicyDenied ? .reseauLocalRefuse : .nomIntrouvable(hote)
        default:
            return .autre(String(describing: e))
        }
    }
}
```

`apps/macos/HaloProtocole/Transport/TransportUDP.swift` :

```swift
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
            etat.withLock { e in
                e.session = session
                e.suite = suite
                e.recus.removeAll()
            }
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
            if ouverte { terminer(err.description) }
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

    private func echec(_ err: ErreurReseau) {
        let ouverte = etat.withLock { e -> Bool in
            e.erreur = err
            e.echoue = true
            return e.suite != nil
        }
        if ouverte { terminer(err.description) }
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
        throw ErreurReseau.aucunDefi
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
```

Si le SDK ne marque pas `NWConnection` `Sendable` et que le compilateur refuse les captures : `@preconcurrency import Network` dans ce fichier (et seulement la).

Textes : sync du catalogue du framework (commande de la tache 3), puis `build/traductions-t4.json` :

```json
{
  "Accès au réseau local refusé : Réglages Système › Confidentialité et sécurité › Réseau local › Halo Compagnon.": "Local network access denied: System Settings › Privacy & Security › Local Network › Halo Compagnon.",
  "Pas de route IPv6 vers le réseau Thread (bug du noyau de macOS, section 10.1).": "No IPv6 route to the Thread network (macOS kernel bug, section 10.1).",
  "Pont introuvable (%@) : éteint, hors du réseau Thread, ou routeurs de bordure injoignables.": "Bridge not found (%@): off, outside the Thread network, or border routers unreachable.",
  "Le pont n'a plus de clé : le brancher en USB, puis « Activer l'accès réseau ».": "The bridge has no key anymore: plug it in over USB, then \"Enable network access\".",
  "Aucune réponse du pont : clé différente de la sienne ? (comparer les empreintes par l'USB)": "No answer from the bridge: a different key? (compare fingerprints over USB)",
  "Connexion réseau perdue : %@": "Network connection lost: %@",
  "Erreur réseau : %@": "Network error: %@",
  "session réseau fermée": "network session closed",
  "session réseau fermée par l'app": "network session closed by the app"
}
```

`python3 Outils/traduire.py HaloProtocole/Localizable.xcstrings build/traductions-t4.json`.

- [ ] **Step 4: Run test to verify it passes**

Run: `... test -only-testing:HaloProtocoleTests/TransportUDPTests -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: les 5 tests du transport passent en quelques secondes ; catalogues alignes. Si Network.framework ne remonte pas l'ICMPv6 "port injoignable" sur `::1` (`portFerme` echoue par `aucunDefi`), ne pas masquer l'ecart : le signaler, marquer ce seul test `.disabled("...")` avec la raison, et le verifier au banc (pont sans cle : `json cle efface` par l'USB, puis connexion reseau).

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloProtocole/Reseau/ErreurReseau.swift apps/macos/HaloProtocole/Transport/TransportUDP.swift apps/macos/HaloProtocoleTests/TransportUDPTests.swift apps/macos/HaloProtocole/Localizable.xcstrings
git commit -m "Ajouter le transport UDP sur Thread (enveloppe H1) et ses erreurs

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Trousseau

**Files:**
- Create: `apps/macos/HaloCompagnon/Reseau/Trousseau.swift`
- Modify: `apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings` (sync + outil)
- Test: `apps/macos/HaloCompagnonTests/TrousseauTests.swift`

**Interfaces:**
- Consumes: `H1.hexa`, `H1.octets(hexa:)` (tache 1).
- Produces:
  - `struct PontConnu: Hashable, Sendable, Identifiable { nom, empreinte ; id ; hote }` (`hote` = `"<nom>.local"`).
  - `protocol TrousseauCles: Sendable { func lister() -> [PontConnu] ; func lire(nom: String) throws -> Data ; func ranger(nom: String, cle: Data, empreinte: String) throws ; func oublier(nom: String) throws }`.
  - `enum ErreurTrousseau: Error, Equatable, Sendable, CustomStringConvertible { absente(String), systeme(Int32) }`.
  - `struct TrousseauSysteme: TrousseauCles { init(service: String = "fr.djoko.halo.pont") }`, `final class TrousseauMemoire: TrousseauCles`.

- [ ] **Step 1: Write the failing test**

`apps/macos/HaloCompagnonTests/TrousseauTests.swift` :

```swift
import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

@Suite("Trousseau des cles reseau (10.4)", .langue(.francais))
struct TrousseauTests {
    static let cle = Data((0..<32).map { UInt8($0) })

    static func exercer(_ t: any TrousseauCles) throws {
        #expect(t.lister().isEmpty)
        #expect(throws: ErreurTrousseau.absente("56B1E064401F74EF")) { try t.lire(nom: "56B1E064401F74EF") }
        try t.ranger(nom: "56B1E064401F74EF", cle: cle, empreinte: "630DCD29")
        #expect(t.lister() == [PontConnu(nom: "56B1E064401F74EF", empreinte: "630DCD29")])
        #expect(try t.lire(nom: "56B1E064401F74EF") == cle)
        // Nouvelle cle pour le meme pont : remplacee, pas doublee.
        try t.ranger(nom: "56B1E064401F74EF", cle: Data(repeating: 9, count: 32), empreinte: "11111111")
        #expect(t.lister().count == 1)
        #expect(try t.lire(nom: "56B1E064401F74EF") == Data(repeating: 9, count: 32))
        try t.oublier(nom: "56B1E064401F74EF")
        #expect(t.lister().isEmpty)
        try t.oublier(nom: "56B1E064401F74EF")  // deja oublie : sans erreur
    }

    @Test func enMemoire() throws {
        try Self.exercer(TrousseauMemoire())
    }

    /// Vrai trousseau (service de test, nettoye) : seulement sur demande,
    /// `TEST_RUNNER_HALO_TEST_TROUSSEAU=1 xcodebuild ... test`.
    @Test(.enabled(if: ProcessInfo.processInfo.environment["HALO_TEST_TROUSSEAU"] == "1"))
    func trousseauDuMac() throws {
        let t = TrousseauSysteme(service: "fr.djoko.halo.pont.tests")
        try? t.oublier(nom: "56B1E064401F74EF")
        try Self.exercer(t)
    }

    @Test func pontConnu() {
        #expect(PontConnu(nom: "56B1E064401F74EF", empreinte: "630DCD29").hote == "56B1E064401F74EF.local")
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodegen generate && xcodebuild ... -derivedDataPath build/dd test -only-testing:HaloCompagnonTests/TrousseauTests`
Expected: echec de compilation (`cannot find 'TrousseauMemoire' in scope`).

- [ ] **Step 3: Write minimal implementation**

`apps/macos/HaloCompagnon/Reseau/Trousseau.swift` :

```swift
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
/// `fr.djoko.halo.pont`, compte = nom SRP, valeur = 64 hexa MAJUSCULES,
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
```

Textes de l'app (sync du catalogue de l'app, commande des Global Constraints), puis `build/traductions-t5.json` :

```json
{
  "Clé absente de ce Mac pour %@.local : brancher le pont en USB, puis « Activer l'accès réseau ».": "No key on this Mac for %@.local: plug the bridge in over USB, then \"Enable network access\".",
  "Trousseau : %@": "Keychain: %@"
}
```

`python3 Outils/traduire.py HaloCompagnon/Ressources/Localizable.xcstrings build/traductions-t5.json`.

- [ ] **Step 4: Run test to verify it passes**

Run: `... test -only-testing:HaloCompagnonTests/TrousseauTests -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: `enMemoire` et `pontConnu` passent, `trousseauDuMac` est saute (non active) ; catalogues alignes.

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloCompagnon/Reseau/Trousseau.swift apps/macos/HaloCompagnonTests/TrousseauTests.swift apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings
git commit -m "Garder la cle reseau dans le trousseau du Mac

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Projet : droit reseau, autorisation reseau local, signature

**Files:**
- Modify: `apps/macos/HaloCompagnon/HaloCompagnon.entitlements`
- Modify: `apps/macos/project.yml` (reglages de signature de `settings.base`, cible `HaloCompagnon`)
- Create: `apps/macos/Signature.xcconfig`, `apps/macos/HaloCompagnon/Ressources/InfoPlist.xcstrings`
- Modify: `apps/macos/.gitignore`, `apps/macos/HaloProtocoleTests/LocalisationTests.swift` (`catalogues()` l.110-113, `codeEtCatalogueAlignes` l.150-160, `chaqueTableASonCatalogue` l.161-169)
- Local, jamais commite : `apps/macos/Local.xcconfig`

**Interfaces:**
- Consumes: rien.
- Produces: app signee avec l'equipe de Djoko quand `Local.xcconfig` existe ; droit `com.apple.security.network.client` ; texte d'autorisation reseau local francais et anglais.

- [ ] **Step 1: Write the failing test**

Dans `LocalisationTests.swift`, `CataloguesTests.catalogues()` attend le nouveau catalogue :

```swift
    @Test func catalogues() {
        #expect(Catalogues.tous.map(\.description)
                == ["HaloProtocole/Localizable", "HaloCompagnon/InfoPlist", "HaloCompagnon/Localizable",
                    "HaloCompagnon/Titres"])
    }
```

et les deux tests d'alignement ecartent la table `InfoPlist` (ses cles viennent de l'Info.plist, pas du code) :

```swift
    /// Catalogues des textes du code (l'Info.plist a le sien, sans cle extraite du code).
    static let duCode = Catalogues.tous.filter { $0.table != "InfoPlist" }
```

(a placer dans `CataloguesTests`), puis `arguments: Self.duCode` pour `codeEtCatalogueAlignes`, et dans `chaqueTableASonCatalogue` : `let catalogues = Set(Self.duCode.filter { $0.cible == cible }.map(\.table))`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodebuild ... -derivedDataPath build/dd test -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: FAIL sur `catalogues()` (pas de `HaloCompagnon/InfoPlist`).

- [ ] **Step 3: Write minimal implementation**

`HaloCompagnon.entitlements`, dans le `dict`, apres `com.apple.security.device.serial` :

```xml
	<key>com.apple.security.network.client</key>
	<true/>
```

`apps/macos/HaloCompagnon/Ressources/InfoPlist.xcstrings` :

```json
{
  "sourceLanguage" : "fr",
  "strings" : {
    "NSLocalNetworkUsageDescription" : {
      "comment" : "Autorisation reseau local (source Reseau, section 10).",
      "extractionState" : "manual",
      "localizations" : {
        "en" : {
          "stringUnit" : {
            "state" : "translated",
            "value" : "Halo Compagnon reaches the Halo bridge on the local network (Thread, through the border routers)."
          }
        },
        "fr" : {
          "stringUnit" : {
            "state" : "translated",
            "value" : "Halo Compagnon joint le pont Halo sur le réseau local (Thread, par les routeurs de bordure)."
          }
        }
      }
    }
  },
  "version" : "1.0"
}
```

`project.yml` :
- dans `settings.base`, retirer `CODE_SIGN_IDENTITY: "-"`, `CODE_SIGN_STYLE: Manual` et `DEVELOPMENT_TEAM: ""` ;
- au niveau du projet (a cote de `options:`), ajouter :
```yaml
configFiles:
  Debug: Signature.xcconfig
  Release: Signature.xcconfig
```
- dans la cible `HaloCompagnon`, `settings.base`, ajouter :
```yaml
        INFOPLIST_KEY_NSLocalNetworkUsageDescription: "Halo Compagnon joint le pont Halo sur le réseau local (Thread, par les routeurs de bordure)."
```

`apps/macos/Signature.xcconfig` :

```
// Signature de l'app et des tests.
// Par defaut : ad hoc (le depot compile partout, sans compte Apple). Pour que
// l'autorisation "reseau local" et le trousseau tiennent d'une compilation a
// l'autre, signer avec son equipe : creer Local.xcconfig (ignore par git) avec
//   DEVELOPMENT_TEAM = <equipe, 10 caracteres>
//   CODE_SIGN_IDENTITY = Apple Development
// L'equipe : security find-certificate -c "Apple Development" -p | openssl x509 -noout -subject (champ OU).
CODE_SIGN_IDENTITY = -
CODE_SIGN_STYLE = Manual
DEVELOPMENT_TEAM =
#include? "Local.xcconfig"
```

`apps/macos/.gitignore`, a la fin :

```
# Signature propre au poste (equipe Apple Development) : voir Signature.xcconfig
Local.xcconfig
```

`Local.xcconfig` (poste de Djoko, non commite) :

```bash
cd apps/macos && printf 'DEVELOPMENT_TEAM = %s\nCODE_SIGN_IDENTITY = Apple Development\n' \
  "$(security find-certificate -c 'Apple Development' -p | openssl x509 -noout -subject | sed -E 's/.*OU ?= ?([A-Z0-9]{10}).*/\1/')" > Local.xcconfig
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd apps/macos && xcodegen generate && xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' -derivedDataPath build/dd test
codesign -dv --entitlements - "build/dd/Build/Products/Debug/Halo Compagnon.app" 2>&1 | grep -E 'TeamIdentifier|network.client|app-sandbox'
ls "build/dd/Build/Products/Debug/Halo Compagnon.app/Contents/Resources/en.lproj/"
git status --short apps/macos
```
Expected: tous les tests passent ; `TeamIdentifier=` suivi de l'equipe (pas `not set`), `com.apple.security.network.client` et `app-sandbox` presents ; `InfoPlist.strings` dans `en.lproj` ; `Local.xcconfig` n'apparait pas dans `git status`.

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloCompagnon/HaloCompagnon.entitlements apps/macos/project.yml apps/macos/Signature.xcconfig apps/macos/HaloCompagnon/Ressources/InfoPlist.xcstrings apps/macos/.gitignore apps/macos/HaloProtocoleTests/LocalisationTests.swift
git commit -m "Autoriser le reseau et signer l'app avec l'equipe du poste

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Source reseau dans le modele (`Pont`)

**Files:**
- Create: `apps/macos/HaloCompagnon/Reseau/AlerteReseau.swift`
- Modify: `apps/macos/HaloCompagnon/Modele/Pont.swift` (`Source` l.12-17, proprietes l.36-96, `init` l.101-129, `sourceParDefaut` l.166-170, `connecter` l.172-190, `nomSource` l.259-265, `ouvrir` l.281-326, `transportOuvert` l.353-359, `echecOuverture` l.370-381, `executer` l.506-539, `synchroniser` l.555-570, commandes l.586-650)
- Modify: `apps/macos/HaloCompagnonTests/DemoBoutEnBoutTests.swift` (`Pont()` l.199, 230, 249 -> `Pont(trousseau: TrousseauMemoire())`)
- Modify: `apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings`
- Test: `apps/macos/HaloCompagnonTests/PontReseauTests.swift`

**Interfaces:**
- Consumes: `TransportUDP`, `ErreurReseau` (tache 4) ; `TrousseauCles`, `TrousseauSysteme`, `PontConnu`, `ErreurTrousseau` (tache 5) ; `MoteurSession.ouvert(maintenant:genre:)`, `Correlateur.politique` (tache 2).
- Produces:
  - `enum AlerteReseau: Equatable, Sendable { transport(ErreurReseau), trousseau(ErreurTrousseau), sansHello ; var texte: String ; static func textePasDeRoute(assistant: Bool) -> String ; static var assistantInstalle: Bool }`.
  - `Pont.Source.reseau(nom: String)`, `Pont.Source.estReseau`.
  - `Pont.init(trousseau: any TrousseauCles = TrousseauSysteme())`, `Pont.pontsConnus: [PontConnu]`, `Pont.alerteReseau: AlerteReseau?`, `Pont.aDistance: Bool`, `Pont.peutEnvoyer(_: String) -> Bool`, `Pont.oublierPont(_ nom: String)`, `static Pont.choisirSource(ports: [PortUSB], connus: [PontConnu]) -> Source`.

- [ ] **Step 1: Write the failing test**

`apps/macos/HaloCompagnonTests/PontReseauTests.swift` :

```swift
import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

@Suite("Source reseau du modele", .serialized, .langue(.francais))
@MainActor
struct PontReseauTests {
    static let nom = "56B1E064401F74EF"

    @Test func cleAbsenteArreteSansReessayer() async {
        let pont = Pont(trousseau: TrousseauMemoire())
        pont.connecter(.reseau(nom: Self.nom))
        #expect(pont.alerteReseau == .trousseau(.absente(Self.nom)))
        if case .erreur = pont.etatTransport {} else { Issue.record("etat \(pont.etatTransport)") }
        try? await Task.sleep(for: .milliseconds(500))
        if case .erreur = pont.etatTransport {} else { Issue.record("aucune reprise attendue") }
        pont.deconnecter()
    }

    @Test func choixDeLaSource() {
        let connu = PontConnu(nom: Self.nom, empreinte: "630DCD29")
        let c6 = PortUSB(chemin: "/dev/cu.usbmodem1", vid: 0x303A, pid: 0x1001, serie: nil, produit: nil)
        let autre = PortUSB(chemin: "/dev/cu.usbmodem2", vid: 0x043E, pid: 0x9A39, serie: nil, produit: nil)
        #expect(Pont.choisirSource(ports: [autre, c6], connus: [connu]) == .serie(chemin: c6.chemin, serie: nil))
        #expect(Pont.choisirSource(ports: [autre], connus: [connu]) == .reseau(nom: Self.nom))
        #expect(Pont.choisirSource(ports: [], connus: []) == .demo)
    }

    @Test func pontsConnusEtOubli() throws {
        let t = TrousseauMemoire()
        try t.ranger(nom: Self.nom, cle: Data(repeating: 1, count: 32), empreinte: "630DCD29")
        let pont = Pont(trousseau: t)
        #expect(pont.pontsConnus.map(\.nom) == [Self.nom])
        pont.oublierPont(Self.nom)
        #expect(pont.pontsConnus.isEmpty)
        #expect(t.lister().isEmpty)
    }

    @Test func textePasDeRoute() {
        #expect(AlerteReseau.textePasDeRoute(assistant: false).contains("installer.sh"))
        #expect(!AlerteReseau.textePasDeRoute(assistant: true).contains("installer.sh"))
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd apps/macos && xcodegen generate && xcodebuild ... -derivedDataPath build/dd test -only-testing:HaloCompagnonTests/PontReseauTests`
Expected: echec de compilation (`type 'Pont.Source' has no member 'reseau'`).

- [ ] **Step 3: Write minimal implementation**

`apps/macos/HaloCompagnon/Reseau/AlerteReseau.swift` :

```swift
import Foundation
import HaloProtocole

/// Alerte de la source reseau, montree en bandeau ; son texte suit la langue en vigueur.
enum AlerteReseau: Equatable, Sendable {
    case transport(ErreurReseau)
    case trousseau(ErreurTrousseau)
    /// DEFI recu, puis aucun `hello` (places de session prises, 10.4).
    case sansHello

    var texte: String {
        switch self {
        case .transport(.pasDeRoute): Self.textePasDeRoute(assistant: Self.assistantInstalle)
        case .transport(let e): e.description
        case .trousseau(let e): e.description
        case .sansHello:
            tr("Aucune réponse au json 1 par le réseau : deux autres sessions déjà actives (autre Mac, iPhone, halo_udp.py) ? Nouvel essai toutes les 30 s.")
        }
    }

    static func textePasDeRoute(assistant: Bool) -> String {
        ErreurReseau.pasDeRoute.description + " " + (assistant
            ? tr("L'assistant système halo-routes est installé : la route revient d'elle-même.")
            : tr("Installer l'assistant système : sh tools/macos/halo-routes/installer.sh"))
    }

    /// Le plist de l'assistant (10.1), si la sandbox laisse le voir.
    static var assistantInstalle: Bool {
        FileManager.default.fileExists(atPath: "/Library/LaunchDaemons/fr.djoko.halo.routes.plist")
    }
}
```

Dans `Pont.swift` (`import Network` en tete) :

1. `Source` :
```swift
    enum Source: Hashable, Sendable {
        case serie(chemin: String, serie: String?)
        case demo
        /// Pont joint par le reseau (section 10) : son nom SRP, sans `.local`.
        case reseau(nom: String)

        var estDemo: Bool { self == .demo }
        var estReseau: Bool { if case .reseau = self { true } else { false } }
    }
```
2. Etat publie, apres `alerte` :
```swift
    /// Alerte de la source reseau (cle absente, reseau local refuse...), en bandeau.
    private(set) var alerteReseau: AlerteReseau?
    /// Ponts dont ce Mac a la cle (trousseau).
    private(set) var pontsConnus: [PontConnu] = []
```
   Interne :
```swift
    @ObservationIgnored private let trousseau: any TrousseauCles
    @ObservationIgnored private let cheminReseau = NWPathMonitor()
```
3. `init()` devient `init(trousseau: any TrousseauCles = TrousseauSysteme())`, et commence par :
```swift
        self.trousseau = trousseau
        pontsConnus = trousseau.lister()
```
   puis, apres `surveillant.demarrer()` :
```swift
        cheminReseau.pathUpdateHandler = { [weak self] chemin in
            guard chemin.status == .satisfied else { return }
            Task { @MainActor [weak self] in self?.reseauChange() }
        }
        cheminReseau.start(queue: .main)
```
   et dans l'observateur du reveil, apres `signalerPause()` : `self?.reseauChange()`.
4. `sourceParDefaut` :
```swift
    /// Source proposee par defaut : le premier port Espressif, sinon un pont
    /// reseau connu, sinon la demo.
    var sourceParDefaut: Source { Self.choisirSource(ports: ports, connus: pontsConnus) }

    static func choisirSource(ports: [PortUSB], connus: [PontConnu]) -> Source {
        if let p = ports.first(where: \.estEspressif) { return .serie(chemin: p.chemin, serie: p.serie) }
        if let r = connus.first { return .reseau(nom: r.nom) }
        return .demo
    }
```
5. `connecter` et `deconnecter` : a cote de `alerte = nil`, ajouter `alerteReseau = nil` (dans `deconnecter`, avant `fermerProprement()`).
6. `nomSource` : `case .reseau(let nom): "\(nom).local"`.
7. `ouvrir()`, nouveau cas du `switch source` :
```swift
        case .reseau(let nom):
            do {
                t = TransportUDP(hote: "\(nom).local", cle: try trousseau.lire(nom: nom))
            } catch {
                let a = AlerteReseau.trousseau(error as? ErreurTrousseau ?? .absente(nom))
                alerteReseau = a
                etatTransport = .erreur(a.texte)
                return
            }
```
8. `transportOuvert()` :
```swift
    private func transportOuvert() {
        etatTransport = .ouvert
        alerteReseau = nil
        debutActivite()
        recepteur.resynchroniser()
        note(genreTransport == .udp ? tr("Session réseau ouverte : \(nomTransport).")
                                    : tr("Port ouvert : \(nomTransport) (DTR = RTS = 0)."))
        executer(moteur.ouvert(maintenant: maintenant(), genre: genreTransport ?? .usb))
    }
```
9. `echecOuverture(_:)`, en tete :
```swift
        if let e = erreur as? ErreurReseau {
            let texte = AlerteReseau.transport(e).texte
            if !e.repriseAutomatique {
                alerteReseau = .transport(e)
                etatTransport = .erreur(texte)
            } else if reconnexionAuto, essaisReconnexion < 40 {
                planifierReconnexion(texte)
            } else {
                etatTransport = .erreur(tr("\(texte) — en attente d'un changement du réseau"))
            }
            return
        }
```
   (`transport = nil` reste la premiere ligne de la fonction.) Nouvelle fonction, apres `portsChanges` :
```swift
    /// Chemin reseau retrouve ou reveil du Mac : une source reseau en attente
    /// ou en echec repart (la ou l'USB attend le retour du port).
    private func reseauChange() {
        guard reconnexionAuto, source?.estReseau == true else { return }
        switch etatTransport {
        case .attente, .erreur:
            essaisReconnexion = 0
            planifierReconnexion(tr("réseau changé"))
        default:
            break
        }
    }
```
10. `executer`, cas `.note(let n)` :
```swift
            case .note(let n):
                if n == .aucuneReponse, genreTransport == .udp {
                    alerteReseau = .sansHello
                    note(AlerteReseau.sansHello.texte, grave: true)
                } else {
                    note(n.texte, grave: n.grave)
                    if n.grave { alerte = n }
                }
```
    cas `.commandeSansReponse(let id)` : remplacer le texte par
```swift
                    let p = moteur.correlateur.politique
                    let t = p.renvois > 0
                        ? tr("‹ id=\(numero) « \(s.commande) » : sans réponse sous \(Int(p.delaiReponse)) s (\(p.renvois) renvois du même id)")
                        : tr("‹ id=\(numero) « \(s.commande) » : sans réponse sous 3 s (pas de réémission)")
                    ajouterConsole(.retour(ok: false, session: s.origine == .session), t, numero: s.numero)
```
11. `synchroniser()`, dans `if phase == .connecte` : ajouter `alerteReseau = nil`.
12. Commandes :
```swift
    /// Source reseau ouverte : liste blanche (10.5).
    var aDistance: Bool { genreTransport == .udp }

    /// La commande peut partir par la source en vigueur.
    func peutEnvoyer(_ commande: String) -> Bool {
        peutCommander && (!aDistance || PolitiqueCommandes.autoriseeADistance(commande))
    }
```
    dans `envoyer(_:fusion:)`, apres la garde `peutCommander` :
```swift
        guard !aDistance || PolitiqueCommandes.autoriseeADistance(commande) else {
            note(tr("« \(commande) » : interdite à distance (liste blanche, section 10.5)."))
            return nil
        }
```
    `consoleAvecId` commence par `if aDistance { return true }` (le pont ignore une ligne sans `id` a distance).
13. Oubli :
```swift
    /// Retire la cle d'un pont du trousseau (le pont garde la sienne).
    func oublierPont(_ nom: String) {
        do { try trousseau.oublier(nom: nom) } catch { note(String(describing: error), grave: true) }
        pontsConnus = trousseau.lister()
        if source == .reseau(nom: nom) { deconnecter() }
    }
```

`DemoBoutEnBoutTests.swift` : les trois `let pont = Pont()` deviennent `let pont = Pont(trousseau: TrousseauMemoire())` (aucun test ne touche au vrai trousseau).

Textes de l'app : sync, puis `build/traductions-t7.json` :

```json
{
  "Aucune réponse au json 1 par le réseau : deux autres sessions déjà actives (autre Mac, iPhone, halo_udp.py) ? Nouvel essai toutes les 30 s.": "No answer to json 1 over the network: two other sessions already active (another Mac, iPhone, halo_udp.py)? Retrying every 30 s.",
  "L'assistant système halo-routes est installé : la route revient d'elle-même.": "The halo-routes system helper is installed: the route will come back by itself.",
  "Installer l'assistant système : sh tools/macos/halo-routes/installer.sh": "Install the system helper: sh tools/macos/halo-routes/installer.sh",
  "Session réseau ouverte : %@.": "Network session open: %@.",
  "%@ — en attente d'un changement du réseau": "%@ — waiting for a network change",
  "réseau changé": "network changed",
  "‹ id=%@ « %@ » : sans réponse sous %lld s (%lld renvois du même id)": "‹ id=%@ “%@”: no reply within %lld s (%lld resends of the same id)",
  "« %@ » : interdite à distance (liste blanche, section 10.5).": "“%@”: not allowed remotely (allow list, section 10.5)."
}
```

(Verifier dans le catalogue apres sync la forme exacte des cles a plusieurs valeurs, `%@` ou `%lld`, et reprendre les memes ; l'outil numerote.)

- [ ] **Step 4: Run test to verify it passes**

Run: `... test -only-testing:HaloCompagnonTests -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: `PontReseauTests` passe ; `PontDemoTests` et `DemoBoutEnBoutTests` passent toujours ; catalogues alignes.

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloCompagnon/Reseau/AlerteReseau.swift apps/macos/HaloCompagnon/Modele/Pont.swift apps/macos/HaloCompagnonTests/PontReseauTests.swift apps/macos/HaloCompagnonTests/DemoBoutEnBoutTests.swift apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings
git commit -m "Ouvrir une source reseau dans le modele de l'app

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Creation de la cle par l'USB (modele et carte de demo)

**Files:**
- Modify: `apps/macos/HaloProtocole/Commandes/Correlateur.swift` (nouvelle `effacerCle`), `apps/macos/HaloProtocole/Session/MoteurSession.swift` (nouvelle `effacerCle`)
- Modify: `apps/macos/HaloCompagnon/Modele/Pont.swift` (`traiterMachine` cas `.reponse` l.485-491, `executer` cas `.commandeSansReponse`)
- Modify: `apps/macos/HaloCompagnon/Demo/SimulateurDemo.swift` (`tic` l.179-182, `instantane` l.352-360, `commandeJson` cas `"cle"` l.486-487)
- Modify: `apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings`
- Test: `apps/macos/HaloProtocoleTests/CleReseauTests.swift` (un test de plus), `apps/macos/HaloCompagnonTests/PontReseauTests.swift` (un test de plus)

**Interfaces:**
- Consumes: `CleReseau`, `EtatAccesReseau` (tache 3) ; `TrousseauCles`, `TrousseauMemoire` (tache 5) ; `Pont.envoyer`, `Pont.pontsConnus` (tache 7).
- Produces: `Correlateur.effacerCle(_ id: UUID)`, `MoteurSession.effacerCle(_ id: UUID)`, `Pont.accesReseau: EtatAccesReseau`, `Pont.creerCle()`.

- [ ] **Step 1: Write the failing test**

Dans `CleReseauTests` :

```swift
    @Test func laCleNeResteNiDansLesSuivis() {
        var c = Correlateur()
        let a = c.soumettre(CleReseau.commande(alea: Data(repeating: 1, count: 32)), origine: .interface, maintenant: 0)
        _ = c.prochainEnvoi(maintenant: 0)
        _ = c.recevoir(Self.reponseCle(cle: H1.hexa(VecteursH1.psk), empreinte: "630DCD29").avecId(1), maintenant: 0.1)
        #expect(c.suivi(a)?.fin?.cle != nil)
        c.effacerCle(a)
        #expect(c.suivi(a)?.fin?.cle == nil)
        #expect(c.suivi(a)?.fin?.empreinte == "630DCD29", "l'empreinte reste")
    }
```

avec, dans le meme fichier :

```swift
extension Reponse {
    func avecId(_ n: Int) -> Reponse {
        var r = self
        r.id = n
        return r
    }
}
```

Dans `PontReseauTests` :

```swift
    @Test func creerLaCleParLUSB() async throws {
        let t = TrousseauMemoire()
        let pont = Pont(trousseau: t)
        pont.connecter(.demo)
        try #require(await attendre { pont.phase == .connecte && pont.accesReseau != .inconnu })
        #expect(pont.accesReseau == .sansCle(nom: Self.nom))
        pont.creerCle()
        try #require(await attendre { !t.lister().isEmpty })
        let connu = try #require(t.lister().first)
        #expect(connu.nom == Self.nom)
        let cle = try t.lire(nom: connu.nom)
        #expect(H1.kid(cle: cle) == connu.empreinte)
        #expect(pont.pontsConnus == [connu])
        // Le secret ne traine nulle part (10.4).
        let hexa = H1.hexa(cle)
        #expect(!pont.console.elements.contains { $0.texte.contains(hexa) })
        #expect(!pont.trames.elements.contains { $0.json.contains(hexa) })
        #expect(!pont.suivis.contains { $0.fin?.cle != nil })
        _ = await attendre { pont.accesReseau == .cleConnue(nom: connu.nom, empreinte: connu.empreinte) }
        #expect(pont.accesReseau == .cleConnue(nom: connu.nom, empreinte: connu.empreinte))
        pont.deconnecter()
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `... test -only-testing:HaloProtocoleTests/CleReseauTests -only-testing:HaloCompagnonTests/PontReseauTests`
Expected: echec de compilation (`value of type 'Correlateur' has no member 'effacerCle'`, `'Pont' has no member 'creerCle'`).

- [ ] **Step 3: Write minimal implementation**

`Correlateur` (apres `verifierDelais`) :

```swift
    /// Retire la cle d'une `reponse` a `json cle nouvelle` une fois rangee (10.4).
    public mutating func effacerCle(_ id: UUID) {
        guard let i = index(id) else { return }
        suivis[i].fin?.cle = nil
    }
```

`MoteurSession` (apres `soumettre`) :

```swift
    /// La cle rendue par `json cle nouvelle` ne reste pas dans les suivis (10.4).
    public mutating func effacerCle(_ id: UUID) {
        correlateur.effacerCle(id)
    }
```

`Pont.swift` :

```swift
    /// `json cle nouvelle` en cours : suivi et nom SRP du pont.
    @ObservationIgnored private var creationCle: (id: UUID, nom: String)?

    /// Acces reseau (carte Thread) : par l'USB ou la demo seulement.
    var accesReseau: EtatAccesReseau {
        guard genreTransport == .usb || genreTransport == .demo else { return .inconnu }
        let connus = pontsConnus
        return EtatAccesReseau.depuis(ip: etat.ip?.valeur) { nom in connus.first { $0.nom == nom }?.empreinte }
    }

    /// Nouvelle cle par l'USB (10.4) : alea de l'app, cle calculee par la
    /// carte, rangee dans le trousseau ; les sessions reseau tombent.
    func creerCle() {
        guard !aDistance, creationCle == nil, let nom = etat.ip?.valeur.srp?.nom else { return }
        guard let id = envoyer(CleReseau.commande(alea: CleReseau.alea())) else { return }
        creationCle = (id, nom)
        note(tr("Nouvelle clé réseau demandée à la carte : les sessions réseau en cours tombent."))
    }

    private func terminerCreationCle(_ r: Reponse, nom: String, suivi: UUID) {
        moteur.effacerCle(suivi)
        synchroniser()
        switch CleReseau.verifier(r) {
        case .success(let c):
            do {
                try trousseau.ranger(nom: nom, cle: c.cle, empreinte: c.empreinte)
                pontsConnus = trousseau.lister()
                note(tr("Clé réseau rangée dans le trousseau (empreinte \(c.empreinte)) : le pont est dans la section Réseau."))
            } catch {
                note(String(describing: error), grave: true)
            }
        case .failure(let e):
            note(e.description, grave: true)
        }
    }
```

dans `traiterMachine`, cas `.reponse(let r)`, a la fin du cas :

```swift
            if let c = creationCle, r.etape == .fin, moteur.correlateur.suivi(numero: r.id)?.id == c.id {
                creationCle = nil
                terminerCreationCle(r, nom: c.nom, suivi: c.id)
            }
```

dans `executer`, cas `.commandeSansReponse(let id)`, en tete :

```swift
                if creationCle?.id == id {
                    creationCle = nil
                    note(tr("Pas de réponse à la création de clé : si la carte a changé de clé, la carte Thread affiche « clé inconnue de ce Mac » ; recommencer."), grave: true)
                }
```

`SimulateurDemo.swift` (`import CryptoKit` en tete) :

```swift
    // Transport reseau simule (10.4) : nom SRP du pont de demo et sa cle.
    private static let srpDemo = "56B1E064401F74EF"
    private var cleDemo: Data?

    /// Bloc `reseau` `ip` d'apres la cle simulee.
    private func ligneIp() -> String {
        let empreinte: JSONValeur = cleDemo.map { .texte(H1.kid(cle: $0)) } ?? .nul
        return LigneJSON.machine("reseau", [
            ("bloc", "ip"), ("frais_ms", 0),
            ("srp", .objet([("nom", .texte(Self.srpDemo))])),
            ("adresses", .tableau([.objet([("adr", "fd4f:9c:ed42:0:92ce:ed98:d7ba:119f"), ("type", "omr"), ("pref", true)])])),
            ("udp", .objet([("port", 5480), ("ouvert", .booleen(cleDemo != nil)), ("empreinte", empreinte),
                            ("sessions", 0), ("provisoire", false), ("rx", 0), ("rejets", 0), ("rx_perdus", 0),
                            ("defis", 0), ("tx", 0), ("tx_perdus", 0), ("tx_erreurs", 0),
                            ("tampons_libres", 65), ("tampons_min", 42)])),
        ])
    }
```

`instantane(hello:etat:)` : dans le `if etat`, apres la boucle des blocs, `emettre(ligneIp())`. `tic()` : dans le bloc `reseauMs`, apres la boucle, `emettre(ligneIp())`. `commandeJson`, le cas `"cle"` devient :

```swift
        case "cle":
            // Transport reseau simule : la cle ne sert qu'a l'essai de l'app (10.4).
            if m.count == 2 {
                reponse(id, cmd, code: "ok", suite: [("empreinte", cleDemo.map { .texte(H1.kid(cle: $0)) } ?? .nul)])
            } else if m.count == 3, m[2] == "efface" {
                cleDemo = nil
                reponse(id, cmd, code: "ok")
                emettre(ligneIp())
            } else if m.count == 4, m[2] == "nouvelle", id != nil,
                      let alea = H1.octets(hexa: m[3].uppercased()), alea.count == 32 {
                let cle = Data(HMAC<SHA256>.authenticationCode(for: H1.aleatoire(32), using: SymmetricKey(data: alea)))
                cleDemo = cle
                reponse(id, "json cle nouvelle", code: "ok",
                        suite: [("cle", .texte(H1.hexa(cle))), ("empreinte", .texte(H1.kid(cle: cle)))])
                emettre(ligneIp())
            } else {
                usage("json cle [nouvelle <64 hexa>|efface]")
            }
```

Textes de l'app : sync, puis `build/traductions-t8.json` :

```json
{
  "Nouvelle clé réseau demandée à la carte : les sessions réseau en cours tombent.": "New network key requested from the board: open network sessions are dropped.",
  "Clé réseau rangée dans le trousseau (empreinte %@) : le pont est dans la section Réseau.": "Network key stored in the keychain (fingerprint %@): the bridge is in the Network section.",
  "Pas de réponse à la création de clé : si la carte a changé de clé, la carte Thread affiche « clé inconnue de ce Mac » ; recommencer.": "No reply to the key creation: if the board changed its key, the Thread card shows \"key unknown to this Mac\"; try again."
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `... test -only-testing:HaloProtocoleTests/CleReseauTests -only-testing:HaloCompagnonTests -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: tout passe, dont `creerLaCleParLUSB` (cle rangee, aucune trace de ses 64 hexa, etat `cleConnue`).

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloProtocole/Commandes/Correlateur.swift apps/macos/HaloProtocole/Session/MoteurSession.swift apps/macos/HaloCompagnon/Modele/Pont.swift apps/macos/HaloCompagnon/Demo/SimulateurDemo.swift apps/macos/HaloProtocoleTests/CleReseauTests.swift apps/macos/HaloCompagnonTests/PontReseauTests.swift apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings
git commit -m "Creer la cle reseau par l'USB et la ranger dans le trousseau

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Interface : section Reseau, acces par l'USB, bandeau, commandes grisees

**Files:**
- Modify: `apps/macos/HaloCompagnon/Vues/ContenuPrincipal.swift` (bandeau l.55-57, `PanneauConnexion` l.147-210)
- Modify: `apps/macos/HaloCompagnon/Vues/TableauDeBord.swift` (`CarteThread` l.256-315, nouvelle vue `AccesReseau`)
- Modify: `apps/macos/HaloCompagnon/Vues/Graphiques.swift` (l.119-120), `apps/macos/HaloCompagnon/Vues/TramesEnDirect.swift` (l.114-117)
- Modify: `apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings`

**Interfaces:**
- Consumes: `Pont.pontsConnus`, `Pont.connecter(.reseau(nom:))`, `Pont.oublierPont`, `Pont.alerteReseau`, `Pont.peutEnvoyer`, `Pont.aDistance`, `Pont.Source.estReseau` (tache 7) ; `Pont.accesReseau`, `Pont.creerCle()` (tache 8).
- Produces: interface seulement.

- [ ] **Step 1: Write the failing test**

Pas de test d'interface automatise dans ce projet : la verification est le build (avertissements = erreurs), les catalogues (`CataloguesTests`, qui echouent tant que les nouveaux textes ne sont pas traduits) et l'essai manuel du Step 4.

- [ ] **Step 2: Run test to verify it fails**

Apres les changements du Step 3 mais avant la traduction : `... test -only-testing:HaloProtocoleTests/CataloguesTests`
Expected: FAIL `codeEtCatalogueAlignes` ("absentes du catalogue") tant que le catalogue n'est pas synchronise et traduit.

- [ ] **Step 3: Write minimal implementation**

`ContenuPrincipal.swift`, bandeau :

```swift
                if let a = pont.alerteReseau {
                    Bandeau(texte: a.texte, couleur: .red, icone: "network.slash")
                } else if let alerte = pont.alerte {
                    Bandeau(texte: alerte.texte, couleur: .red, icone: "exclamationmark.octagon.fill")
                }
```

`PanneauConnexion` : `@State private var aOublier: PontConnu?` ; dans le `Menu`, entre la section "Ports série" et celle de la demo :

```swift
                Section("Réseau") {
                    if pont.pontsConnus.isEmpty {
                        Text("Aucun pont : « Activer l'accès réseau » par l'USB (carte Thread et Matter)")
                    }
                    ForEach(pont.pontsConnus) { p in
                        Menu {
                            Button("Connecter par le réseau") { pont.connecter(.reseau(nom: p.nom)) }
                            Button("Oublier ce pont…", role: .destructive) { aOublier = p }
                        } label: {
                            Label("\(p.hote) · clé \(p.empreinte)", systemImage: "point.3.connected.trianglepath.dotted")
                        }
                    }
                }
```

l'icone du libelle : `systemImage: pont.estDemo ? "play.rectangle" : (pont.source?.estReseau == true ? "point.3.connected.trianglepath.dotted" : "cable.connector")` ; `libelleSource` : `case .reseau(let nom): "\(nom).local"` ; sur le `VStack` du panneau :

```swift
        .confirmationDialog("Oublier ce pont ?", isPresented: Binding(get: { aOublier != nil }, set: { if !$0 { aOublier = nil } }),
                            presenting: aOublier) { p in
            Button("Oublier \(p.hote)", role: .destructive) { pont.oublierPont(p.nom) }
        } message: { p in
            Text("La clé de \(p.hote) est retirée du trousseau de ce Mac ; le pont garde la sienne. Pour revenir : « Activer l'accès réseau » par l'USB crée une nouvelle clé.")
        }
```

`TableauDeBord.swift`, dans `CarteThread`, juste apres le bloc `if let ip = pont.etat.ip?.valeur { ... }` :

```swift
            if pont.accesReseau != .inconnu {
                Divider()
                AccesReseau()
            }
```

et, avant `// MARK: - Appairage Matter` :

```swift
/// Cle du transport reseau, par l'USB (10.4) : etat et creation.
private struct AccesReseau: View {
    @Environment(Pont.self) private var pont
    @State private var confirmation = false

    var body: some View {
        HStack {
            switch pont.accesReseau {
            case .sansCle:
                Text("Accès réseau : aucune clé").foregroundStyle(.secondary)
                Spacer()
                Button("Activer l'accès réseau…") { confirmation = true }
            case .cleConnue(_, let e):
                Text("Clé \(e) connue de ce Mac").foregroundStyle(.secondary)
                Spacer()
                Button("Nouvelle clé…") { confirmation = true }
            case .cleInconnue(_, let e):
                Text("Clé \(e) inconnue de ce Mac").foregroundStyle(.orange)
                Spacer()
                Button("Nouvelle clé…") { confirmation = true }
            case .inconnu:
                EmptyView()
            }
        }
        .font(.callout)
        .controlSize(.small)
        .disabled(!pont.peutCommander)
        .confirmationDialog("Créer une nouvelle clé réseau ?", isPresented: $confirmation) {
            Button("Créer la clé") { pont.creerCle() }
        } message: {
            Text("La carte remplace sa clé : les sessions réseau en cours tombent. La nouvelle clé est rangée dans le trousseau de ce Mac ; halo_udp.py l'y relira.")
        }
    }
}
```

`Graphiques.swift` l.120 : `.disabled(!pont.peutCommander)` du bouton "Statistiques de la carte (lampe stats raz)…" devient `.disabled(!pont.peutEnvoyer("lampe stats raz"))`. `TramesEnDirect.swift` l.115 et l.117 : `.disabled(!pont.peutEnvoyer("lampe ecoute 1"))` et `.disabled(!pont.peutEnvoyer("lampe ecoute 0"))`. Sur ces trois boutons, ajouter `.help(pont.aDistance ? tr("Interdite à distance (liste blanche, section 10.5).") : "")`.

Textes : sync du catalogue de l'app, puis `build/traductions-t9.json` avec l'anglais de chaque nouvelle cle (reprendre les cles telles que le sync les a ecrites) :

```json
{
  "Réseau": "Network",
  "Aucun pont : « Activer l'accès réseau » par l'USB (carte Thread et Matter)": "No bridge: \"Enable network access\" over USB (Thread and Matter card)",
  "Connecter par le réseau": "Connect over the network",
  "Oublier ce pont…": "Forget this bridge…",
  "%@ · clé %@": "%@ · key %@",
  "Oublier ce pont ?": "Forget this bridge?",
  "Oublier %@": "Forget %@",
  "La clé de %@ est retirée du trousseau de ce Mac ; le pont garde la sienne. Pour revenir : « Activer l'accès réseau » par l'USB crée une nouvelle clé.": "The key of %@ is removed from this Mac's keychain; the bridge keeps its own. To come back: \"Enable network access\" over USB creates a new key.",
  "Accès réseau : aucune clé": "Network access: no key",
  "Activer l'accès réseau…": "Enable network access…",
  "Clé %@ connue de ce Mac": "Key %@ known to this Mac",
  "Nouvelle clé…": "New key…",
  "Clé %@ inconnue de ce Mac": "Key %@ unknown to this Mac",
  "Créer une nouvelle clé réseau ?": "Create a new network key?",
  "Créer la clé": "Create the key",
  "La carte remplace sa clé : les sessions réseau en cours tombent. La nouvelle clé est rangée dans le trousseau de ce Mac ; halo_udp.py l'y relira.": "The board replaces its key: open network sessions are dropped. The new key is stored in this Mac's keychain; halo_udp.py will read it there.",
  "Interdite à distance (liste blanche, section 10.5).": "Not allowed remotely (allow list, section 10.5)."
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `... test` (toutes les cibles), puis essai manuel en demo : `open "build/dd/Build/Products/Debug/Halo Compagnon.app" --args -demo`.
Expected: tous les tests passent. En demo : la carte "Thread et Matter" montre "Accès réseau : aucune clé" et "Activer l'accès réseau…" ; apres confirmation, "Clé XXXXXXXX connue de ce Mac" ; le menu de la source montre la section "Réseau" avec `56B1E064401F74EF.local · clé XXXXXXXX` ; "Oublier ce pont…" le retire apres confirmation. (La demo range sa cle dans le vrai trousseau du Mac : l'oublier a la fin de l'essai.)

- [ ] **Step 5: Commit**

```bash
git add apps/macos/HaloCompagnon/Vues/ContenuPrincipal.swift apps/macos/HaloCompagnon/Vues/TableauDeBord.swift apps/macos/HaloCompagnon/Vues/Graphiques.swift apps/macos/HaloCompagnon/Vues/TramesEnDirect.swift apps/macos/HaloCompagnon/Ressources/Localizable.xcstrings
git commit -m "Montrer la source reseau et l'acces par l'USB dans l'app

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: `halo_udp.py` lit la cle dans le trousseau

**Files:**
- Modify: `tools/halo_udp.py` (docstring l.1-33, `load_key` l.154-164, `cmd_cle` l.105-132, `cmd_session` l.254-256)
- Test: `tools/test_halo_udp.py`

**Interfaces:**
- Consumes: le service du trousseau `fr.djoko.halo.pont` (tache 5).
- Produces: `SERVICE_TROUSSEAU`, `nom_du_pont(hote) -> str | None`, `comptes_du_trousseau() -> list[str]`, `cle_du_trousseau(nom) -> bytes | None`, `lire_fichier_cle(chemin) -> bytes`, `load_key(hote=None) -> bytes`.

- [ ] **Step 1: Write the failing test**

`tools/test_halo_udp.py` :

```python
#!/usr/bin/env python3
"""Tests de tools/halo_udp.py sans reseau ni trousseau reel : python3 tools/test_halo_udp.py"""
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import halo_udp  # noqa: E402

CLE = bytes(range(32))
DUMP = '''keychain: "/Users/x/Library/Keychains/login.keychain-db"
version: 512
class: "genp"
attributes:
    "acct"<blob>="56B1E064401F74EF"
    "svce"<blob>="fr.djoko.halo.pont"
keychain: "/Users/x/Library/Keychains/login.keychain-db"
class: "genp"
attributes:
    "acct"<blob>="autre"
    "svce"<blob>="com.exemple"
'''


def resultat(sortie, code=0):
    return subprocess.CompletedProcess([], code, stdout=sortie, stderr="")


class Cle(unittest.TestCase):
    def test_nom_du_pont(self):
        self.assertEqual(halo_udp.nom_du_pont("56B1E064401F74EF.local"), "56B1E064401F74EF")
        self.assertIsNone(halo_udp.nom_du_pont("fd4f:9c:ed42::1"))

    def test_comptes_du_trousseau(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(DUMP)):
            self.assertEqual(halo_udp.comptes_du_trousseau(), ["56B1E064401F74EF"])

    def test_cle_du_trousseau(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(CLE.hex().upper() + "\n")) as run:
            self.assertEqual(halo_udp.cle_du_trousseau("56B1E064401F74EF"), CLE)
            self.assertIn("-a", run.call_args[0][0])
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat("", 44)):
            self.assertIsNone(halo_udp.cle_du_trousseau("56B1E064401F74EF"))

    def test_plusieurs_ponts_sans_nom(self):
        with mock.patch.object(halo_udp, "comptes_du_trousseau", return_value=["A", "B"]):
            with self.assertRaises(SystemExit):
                halo_udp.cle_du_trousseau(None)

    def test_ordre(self):
        with tempfile.TemporaryDirectory() as d:
            fichier = os.path.join(d, "cle")
            with open(fichier, "w") as f:
                f.write(bytes(range(1, 33)).hex().upper() + "\n")
            # HALO_CLE d'abord : le trousseau n'est pas consulte.
            with mock.patch.dict(os.environ, {"HALO_CLE": fichier}), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau") as trousseau:
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), bytes(range(1, 33)))
                trousseau.assert_not_called()
            # Sans HALO_CLE : le trousseau passe avant le fichier.
            env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=CLE):
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), CLE)
            # Rien dans le trousseau : le fichier.
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=None):
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), bytes(range(1, 33)))

    def test_cle_exige_halo_cle(self):
        env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
        with mock.patch.dict(os.environ, env, clear=True):
            with self.assertRaises(SystemExit) as e:
                halo_udp.cmd_cle("/dev/cu.inexistant")
            self.assertIn("app", str(e.exception))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 tools/test_halo_udp.py`
Expected: FAIL (`module 'halo_udp' has no attribute 'nom_du_pont'`).

- [ ] **Step 3: Write minimal implementation**

Dans `tools/halo_udp.py` : `import re` et `import subprocess` en tete ; apres `KEY_PATH` :

```python
# Cle rangee par l'app (docs/PROTOCOLE-JSON.md 10.4) : trousseau de session.
SERVICE_TROUSSEAU = "fr.djoko.halo.pont"
```

remplacer `load_key` par :

```python
def lire_fichier_cle(chemin):
    try:
        with open(chemin) as f:
            key = bytes.fromhex(f.read().strip())
    except OSError:
        raise SystemExit(f"{chemin} : illisible")
    except ValueError:
        raise SystemExit(f"{chemin} : cle illisible (64 hexa attendus)")
    if len(key) != 32:
        raise SystemExit(f"{chemin} : cle illisible (64 hexa attendus)")
    return key


def nom_du_pont(hote):
    """Nom SRP (compte du trousseau) d'un hote <nom>.local ; None pour une adresse."""
    return hote[:-len(".local")] if hote.endswith(".local") else None


def comptes_du_trousseau():
    """Comptes (noms SRP) du service, lus sans les secrets (security dump-keychain)."""
    try:
        r = subprocess.run(["security", "dump-keychain"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return []
    comptes, service, compte = set(), None, None
    for ligne in r.stdout.splitlines() + ["keychain:"]:
        ligne = ligne.strip()
        if ligne.startswith("keychain:"):
            if service == SERVICE_TROUSSEAU and compte:
                comptes.add(compte)
            service, compte = None, None
            continue
        m = re.match(r'"(svce|acct)"<blob>="(.*)"$', ligne)
        if m:
            if m.group(1) == "svce":
                service = m.group(2)
            else:
                compte = m.group(2)
    return sorted(comptes)


def cle_du_trousseau(nom):
    """Cle creee par l'app (security ; macOS demande une fois d'autoriser l'acces)."""
    if nom is None:
        comptes = comptes_du_trousseau()
        if len(comptes) > 1:
            raise SystemExit(f"plusieurs ponts dans le trousseau ({', '.join(comptes)}) : "
                             "donner l'hote <nom>.local")
        if not comptes:
            return None
        nom = comptes[0]
    try:
        r = subprocess.run(["security", "find-generic-password", "-s", SERVICE_TROUSSEAU, "-a", nom, "-w"],
                           capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if r.returncode != 0:
        return None
    try:
        key = bytes.fromhex(r.stdout.strip())
    except ValueError:
        return None
    return key if len(key) == 32 else None


def load_key(hote=None):
    """Cle, dans l'ordre : HALO_CLE (fichier), trousseau (cle de l'app), ~/.config/halo-pont/cle."""
    if os.environ.get("HALO_CLE"):
        return lire_fichier_cle(KEY_PATH)
    key = cle_du_trousseau(nom_du_pont(hote) if hote else None)
    if key:
        return key
    if os.path.exists(KEY_PATH):
        return lire_fichier_cle(KEY_PATH)
    raise SystemExit("pas de cle : la creer dans l'app (carte Thread et Matter, par l'USB : Activer l'acces "
                     "reseau), ou HALO_CLE=<fichier> pour un banc sans l'app")
```

en tete de `cmd_cle` :

```python
    if not os.environ.get("HALO_CLE"):
        raise SystemExit("la cle se cree dans l'app (carte Thread et Matter, par l'USB : Activer l'acces reseau) "
                         "et se relit dans le trousseau. Banc sans l'app : "
                         "HALO_CLE=<fichier> python3 tools/halo_udp.py cle <port>")
```

et, apres `print(f"cle rangee dans {KEY_PATH} ...")` : `print("  (la cle de l'app devient perimee : la recreer depuis l'app pour y revenir)")`. Dans `cmd_session` : `key = load_key(host)`. Docstring : section `cle` ("banc sans l'app, HALO_CLE obligatoire") et une ligne "Cle : HALO_CLE, sinon le trousseau (cle creee par l'app), sinon ~/.config/halo-pont/cle".

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 tools/test_halo_udp.py && python3 tools/halo_udp.py 2>&1 | head -3`
Expected: `OK` (6 tests) ; l'aide s'affiche sans erreur.

- [ ] **Step 5: Commit**

```bash
git add tools/halo_udp.py tools/test_halo_udp.py
git commit -m "Lire la cle reseau de l'app dans le trousseau depuis halo_udp.py

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Documentation et essais au banc

**Files:**
- Modify: `apps/macos/README.md` (nouvelle section "Source réseau (UDP sur Thread)", arbre des fichiers)
- Modify: `docs/PROTOCOLE-JSON.md` (10.2 "Pertes" : `json 1` renvoye avec le meme `id` a distance ; 10.4 : trousseau de l'app, service et compte)
- Modify: `docs/ETUDE-THREAD-COMPAGNON.md` (etat de la phase 2, `TransportUDP` dans `HaloProtocole`)

**Interfaces:**
- Consumes: tout ce qui precede.
- Produces: documentation ; resultats du banc.

- [ ] **Step 1: Ecrire la documentation**

`apps/macos/README.md`, section "Source réseau (UDP sur Thread)" : creer la cle (USB, carte "Thread et Matter", "Activer l'accès réseau…") ; se connecter (menu de la source, section "Réseau") ; ce qui est permis a distance (liste blanche 10.5, profil distant 10.6, commandes grisees) ; erreurs et remedes (tableau 4.6 de la spec, assistant `tools/macos/halo-routes`) ; signature (`Local.xcconfig`, `Signature.xcconfig`) et autorisation reseau local ; `halo_udp.py` et le trousseau. `docs/PROTOCOLE-JSON.md` et `docs/ETUDE-THREAD-COMPAGNON.md` : les deux complements ci-dessus, en ASCII, sans toucher au reste.

- [ ] **Step 2: Verifier le tout**

Run: `cd apps/macos && xcodegen generate && xcodebuild -project HaloCompagnon.xcodeproj -scheme HaloCompagnon -destination 'platform=macOS' -derivedDataPath build/dd test && python3 ../../tools/test_halo_udp.py`
Expected: tous les tests passent (les 96 + 17 d'origine et les nouveaux).

- [ ] **Step 3: Essais au banc, avec Djoko (pont reel ; route par l'assistant `halo-routes` ou a la main ; Djoko present pour toute commande lampe)**

1. Cle : app connectee par l'USB, carte "Thread et Matter" -> "Activer l'accès réseau…" -> "Clé XXXXXXXX connue de ce Mac" ; puis `python3 tools/halo_udp.py session 56B1E064401F74EF.local --duree 10` (macOS demande d'autoriser `security` : "Toujours autoriser") -> session ouverte avec la cle de l'app.
2. Source reseau : "Libérer le port", puis menu de la source -> "Réseau" -> "Connecter par le réseau" (macOS demande l'autorisation reseau local la premiere fois) -> `hello` rev 3 transport `udp`, `etat` toutes les 2 s ; `lampe niveau 200` depuis l'app, livree ; `lampe stats raz` grise.
3. R5 : redemarrer le pont (USB : `reboot`, depuis l'app ou `pio device monitor` sans `json cle`) -> l'app reprend seule en ~15 s.
4. R7 : Reglages Systeme > Confidentialite et securite > Reseau local : couper Halo Compagnon -> bandeau "Accès au réseau local refusé…" ; reautoriser -> reprise.
5. Route : `sudo route -n delete -inet6 -prefixlen 64 fd4f:9c:ed42::` (Djoko) -> au plus quelques secondes de bandeau "Pas de route…", puis reprise par l'assistant (journal `/Library/Logs/fr.djoko.halo.routes.log`).

Consigner les resultats dans `docs/PROTOCOLE-JSON.md` section 10 (bloc "Banc du ...") et dans `docs/ETUDE-THREAD-COMPAGNON.md`.

- [ ] **Step 4: Commit**

```bash
git add apps/macos/README.md docs/PROTOCOLE-JSON.md docs/ETUDE-THREAD-COMPAGNON.md
git commit -m "Documenter la source reseau de l'app et ses essais au banc

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
