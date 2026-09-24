import Foundation

/// Genre de transport : les regles de commande en dependent (10.5).
public enum GenreTransport: String, Sendable, Equatable {
    case usb
    case udp
    /// Rejeu local (mode demo) : se comporte comme l'USB.
    case demo
}

/// Erreur de construction d'une ligne vers la carte (section 2.6).
public enum ErreurLigne: Error, Sendable, Equatable, CustomStringConvertible {
    case vide
    case tropLongue(octets: Int, max: Int)
    case caractereInterdit
    case prefixeId
    case json

    public var description: String {
        switch self {
        case .vide: tr("Ligne vide.")
        case .tropLongue(let o, let m): tr("Ligne trop longue : \(o) octets, \(m) au plus avec le préfixe id=.")
        case .caractereInterdit: tr("Seuls les caractères ASCII imprimables sont permis (pas d'accents).")
        case .prefixeId: tr("L'app ajoute elle-même le préfixe id=<n>.")
        case .json: tr("Jamais de JSON ni d'octet RS vers la carte.")
        }
    }
}

/// Lignes app -> carte : texte de la CLI prefixe par `id=<n> ` (6.1).
public enum LigneCommande {
    /// Tampon de la CLI : 128 octets, donc 127 au plus, prefixe compris.
    public static let octetsMax = 127
    public static let idMax = 999_999_999

    /// Octets envoyes a l'ouverture : Ctrl-U puis LF (3.3).
    public static let effacement = Data([Octets.ctrlU, Octets.lf])

    /// Normalise une commande (espaces de bord) et verifie les regles de 2.6.
    public static func valider(_ commande: String, id: Int?) -> Result<String, ErreurLigne> {
        let c = commande.trimmingCharacters(in: .whitespaces)
        guard !c.isEmpty else { return .failure(.vide) }
        guard c.unicodeScalars.allSatisfy({ $0.value >= 0x20 && $0.value <= 0x7E }) else {
            return .failure(c.unicodeScalars.contains { $0.value == 0x1E } ? .json : .caractereInterdit)
        }
        if c.hasPrefix("{") { return .failure(.json) }
        if c.lowercased().hasPrefix("id=") { return .failure(.prefixeId) }
        let ligne = id.map { "id=\($0) \(c)" } ?? c
        guard ligne.utf8.count <= octetsMax else {
            return .failure(.tropLongue(octets: ligne.utf8.count, max: octetsMax))
        }
        return .success(ligne)
    }

    /// Ligne complete, terminee par LF.
    public static func octets(_ commande: String, id: Int?) -> Result<Data, ErreurLigne> {
        valider(commande, id: id).map { Data(($0 + "\n").utf8) }
    }

    /// Numero suivant : 1..999999999, repart a 1.
    public static func suivant(_ id: Int) -> Int {
        id >= idMax ? 1 : id + 1
    }

    /// Mots d'une commande (separes par des espaces), en minuscules.
    public static func mots(_ commande: String) -> [String] {
        commande.lowercased().split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
    }
}

/// Ce que la console fait d'une ligne tapee (6.4, 10.5).
public enum VerdictConsole: Sendable, Equatable {
    case autorisee
    /// Demander confirmation avant d'envoyer.
    case confirmation(String)
    case interdite(String)
}

public enum PolitiqueCommandes {
    /// Commandes qui demandent confirmation (6.4, "Console brute").
    public static func verdictConsole(_ commande: String, transport: GenreTransport) -> VerdictConsole {
        let m = LigneCommande.mots(commande)
        guard let premier = m.first else { return .interdite(ErreurLigne.vide.description) }
        // Longueur jugee avec le plus long id possible : la ligne partira quel que soit son numero.
        if case .failure(let e) = LigneCommande.valider(commande, id: LigneCommande.idMax) {
            return .interdite(e.description)
        }

        if transport == .udp, !autoriseeADistance(commande) {
            return .interdite(tr("Interdite à distance (liste blanche, section 10.5)."))
        }
        if m == ["json", "0"] {
            return .interdite(tr("Utiliser « Libérer le port » : l'app enverra json 0 et fermera le port."))
        }

        if let raison = raisonDangereuse(premier) { return .confirmation(raison) }

        if premier == "lampe", m.count >= 2 {
            switch m[1] {
            case "oublie": return .confirmation(tr("Oublie l'état de la lampe."))
            case "adresse" where m.count >= 3: return .confirmation(tr("Change l'adresse de la lampe."))
            case "stats" where m.count >= 3 && m[2] == "raz":
                return .confirmation(tr("Remet à zéro les compteurs du pilote et de la radio."))
            default: break
            }
        }
        if premier == "matter", m.count >= 2 {
            if m[1] == "med" || m[1] == "maxint" { return .confirmation(tr("Change un réglage Matter persistant.")) }
            if m[1] == "reprise", m.count >= 3, m[2] == "auto" {
                return .confirmation(tr("Change la reprise automatique des abonnements."))
            }
        }
        if premier == "json", m.count >= 3, m[1] == "cle", m[2] == "nouvelle" || m[2] == "efface" {
            return .confirmation(tr("Change la clé du transport réseau : toutes les sessions réseau tombent."))
        }
        return .autorisee
    }

    /// Commandes dangereuses (premier mot) et la raison de la confirmation.
    static func raisonDangereuse(_ premier: String) -> String? {
        switch premier {
        case "reboot": tr("Redémarre la carte (le port USB va se ré-énumérer).")
        case "decommission": tr("Retire la carte de tous les écosystèmes Matter.")
        case "erase": tr("Efface la configuration de la carte.")
        case "wifi": tr("Change les identifiants Wi-Fi.")
        case "addr": tr("Change l'adresse radio (outil de banc).")
        case "chan": tr("Change le canal radio (outil de banc).")
        case "xo": tr("Change le réglage du quartz (outil de banc).")
        case "debit": tr("Change le débit radio (outil de banc).")
        case "amble": tr("Change le préambule radio (outil de banc).")
        case "aw": tr("Change la largeur d'adresse (outil de banc).")
        case "holtek": tr("Reconfigure le module radio (outil de banc).")
        case "regcfg": tr("Écrit des registres du module radio (outil de banc).")
        default: nil
        }
    }

    /// Apres ces commandes, l'app attend la re-enumeration de l'USB (3.1).
    public static func attendReenumeration(_ commande: String) -> Bool {
        let m = LigneCommande.mots(commande)
        return m.first == "reboot" || m.first == "decommission"
    }

    /// Liste blanche des commandes a distance (10.5).
    public static func autoriseeADistance(_ commande: String) -> Bool {
        let m = LigneCommande.mots(commande)
        guard let premier = m.first else { return false }
        func entier(_ i: Int) -> Int? { i < m.count ? Int(m[i]) : nil }
        switch premier {
        case "json":
            guard m.count >= 2 else { return false }
            switch m[1] {
            case "1":
                if m.count == 2 { return true }
                guard m.count == 4, m[2] == "bail", let b = entier(3) else { return false }
                return (10...120).contains(b)
            case "0", "etat", "hello", "ping":
                return m.count == 2
            case "periode":
                guard m.count == 3, let p = entier(2) else { return false }
                return p >= 2000 && p <= 60000
            case "compteurs":
                guard m.count == 3, let p = entier(2) else { return false }
                return p == 0 || (p >= 5000 && p <= 60000)
            case "reseau":
                guard m.count == 3, let p = entier(2) else { return false }
                return p == 0 || (p >= 10000 && p <= 60000)
            case "trames", "log":
                return m.count == 3 && (m[2] == "0" || m[2] == "1")
            default:
                return false
            }
        case "lampe":
            guard m.count >= 2 else { return false }
            switch m[1] {
            case "on", "off", "auto", "sync": return m.count == 2
            case "avant", "arriere": return m.count == 3 && (m[2] == "on" || m[2] == "off")
            case "mode": return m.count == 3 && ["avant", "arriere", "deux"].contains(m[2])
            case "lum", "niveau", "temp", "mired": return m.count == 3
            default: return false
            }
        case "led":
            return m.count == 2 && (m[1] == "test" || m[1] == "stop")
        default:
            return false
        }
    }

    /// Masque une cle de 64 hexa dans une ligne affichee (10.4) : la commande
    /// `json cle nouvelle <hexa>` (casse et espaces quelconques, comme la CLI
    /// les lit), le champ `"cle":"..."` d'une reponse, et toute suite de 64
    /// chiffres hexa (cle imprimee en texte par une commande sans `id`).
    public static func masquerCle(_ texte: String) -> String {
        guard texte.utf8.count >= 64 || texte.range(of: "cle", options: .caseInsensitive) != nil else { return texte }
        let masque = String(repeating: "•", count: 8)
        var s = texte
        s.replace(/(?i)(json[ \t]+cle[ \t]+nouvelle[ \t]+)[0-9a-f]+/) { m in m.output.1 + masque }
        s.replace(/("cle"[ ]*:[ ]*")[^"]*"/) { m in m.output.1 + masque + "\"" }
        s.replace(/\b[0-9A-Fa-f]{64}\b/) { _ in masque }
        return s
    }
}
