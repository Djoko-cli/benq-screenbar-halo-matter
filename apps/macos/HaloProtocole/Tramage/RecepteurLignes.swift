import Foundation

/// Octets du tramage (section 2).
public enum Octets {
    /// Record Separator : debut d'une ligne machine.
    public static let rs: UInt8 = 0x1E
    public static let lf: UInt8 = 0x0A
    public static let cr: UInt8 = 0x0D
    /// Ctrl-U : vide la ligne en cours de saisie de la CLI.
    public static let ctrlU: UInt8 = 0x15
}

/// Ce que le recepteur tire du flux.
public enum ElementRecu: Sendable, Equatable {
    case machine(LigneMachine)
    case texte(LigneTexte)
    /// Suite d'une ligne machine coupee (filtre "logs systeme" seulement).
    case fragment(String)
    /// Ligne machine rejetee (non affichee comme texte).
    case abimee(raison: String, brut: String)
    case versionInconnue(v: Int, t: String)
    /// Objet JSON valide mais champ obligatoire absent ou mal type.
    case invalide(t: String, raison: String)
    /// Plus de 2048 octets sans LF : vides comme texte.
    case debordement(String)
}

/// Compteurs de reception de l'app (section 2.4).
public struct CompteursReception: Sendable, Equatable {
    public var lignesMachine = 0
    public var lignesTexte = 0
    public var lignesAbimees = 0
    public var fragments = 0
    public var debordements = 0
    public var versionsInconnues = 0
    public var typesInconnus = 0
    public var messagesInvalides = 0
    public var octets = 0

    public init() {}
}

/// Separation texte / JSON sur des octets bruts (algorithme 2.4).
///
/// Aucun decodage de caracteres avant d'avoir separe texte et JSON ; le texte
/// est ensuite decode en UTF-8 avec remplacement (jamais d'echec).
public struct RecepteurLignes: Sendable {
    public static let tamponMax = 2048
    /// JSON entre RS et LF : 1024 octets au plus, RS et LF compris.
    public static let jsonMax = 1022
    private static let debutJSON: [UInt8] = Array(#"{"v":"#.utf8)

    public private(set) var compteurs = CompteursReception()
    private var tampon: [UInt8] = []
    private var jeterJusquAuLF = false
    private var premiereApresPause = false
    private var precedenteAbimee = false

    public init() {}

    /// Ouverture du port : jeter tout ce qui precede le premier LF (3.1, etape 4).
    public mutating func resynchroniser() {
        tampon.removeAll(keepingCapacity: true)
        jeterJusquAuLF = true
        precedenteAbimee = false
        premiereApresPause = false
    }

    /// Pause de lecture de l'app (veille du Mac, app suspendue) : la premiere
    /// ligne lue ensuite peut etre un fragment.
    public mutating func signalerPause() {
        premiereApresPause = true
    }

    public mutating func remettreCompteursAZero() {
        compteurs = CompteursReception()
    }

    public mutating func alimenter<C: Collection>(_ octets: C) -> [ElementRecu] where C.Element == UInt8 {
        compteurs.octets += octets.count
        tampon.append(contentsOf: octets)
        var sortie: [ElementRecu] = []
        var debut = 0
        while let lf = tampon[debut...].firstIndex(of: Octets.lf) {
            let ligne = tampon[debut..<lf]
            debut = lf + 1
            traiter(ligne, dans: &sortie)
        }
        if debut > 0 { tampon.removeFirst(debut) }
        if tampon.count > Self.tamponMax {
            compteurs.debordements += 1
            let texte = String(decoding: tampon, as: UTF8.self)
            tampon.removeAll(keepingCapacity: true)
            precedenteAbimee = false
            if jeterJusquAuLF {
                // Toujours pas de LF : rien a jeter de plus, la suite est lisible.
                jeterJusquAuLF = false
            } else {
                sortie.append(.debordement(texte))
            }
        }
        return sortie
    }

    private mutating func traiter(_ brute: ArraySlice<UInt8>, dans sortie: inout [ElementRecu]) {
        var ligne = brute
        if ligne.last == Octets.cr { ligne = ligne.dropLast() }
        if jeterJusquAuLF {
            jeterJusquAuLF = false
            return
        }
        let apresPause = premiereApresPause
        premiereApresPause = false

        guard let i = ligne.lastIndex(of: Octets.rs) else {
            if ligne.last == UInt8(ascii: "}") && (precedenteAbimee || apresPause) {
                compteurs.fragments += 1
                sortie.append(.fragment(String(decoding: ligne, as: UTF8.self)))
            } else {
                compteurs.lignesTexte += 1
                sortie.append(.texte(ClasseurTexte.classer(String(decoding: ligne, as: UTF8.self))))
            }
            precedenteAbimee = false
            return
        }

        let avant = ligne[ligne.startIndex..<i]
        // Un RS plus tot dans la ligne : debut d'une ligne machine coupee avant
        // son LF (2.4, "le dernier RS"). Ce reste n'est jamais montre comme texte.
        let premierRS = avant.firstIndex(of: Octets.rs)
        let texteAvant = avant[avant.startIndex..<(premierRS ?? i)]
        if texteAvant.contains(where: { $0 != 0x20 && $0 != 0x09 }) {
            compteurs.lignesTexte += 1
            sortie.append(.texte(ClasseurTexte.classer(String(decoding: texteAvant, as: UTF8.self))))
        }
        if let premierRS {
            compteurs.lignesAbimees += 1
            sortie.append(.abimee(raison: tr("ligne machine coupée avant son LF"),
                                  brut: String(decoding: avant[(premierRS + 1)...], as: UTF8.self)))
        }
        let json = ligne[(i + 1)...]
        precedenteAbimee = false

        func abimee(_ raison: String) {
            compteurs.lignesAbimees += 1
            precedenteAbimee = true
            sortie.append(.abimee(raison: raison, brut: String(decoding: json, as: UTF8.self)))
        }

        guard json.count <= Self.jsonMax else { return abimee(tr("plus de \(String(Self.jsonMax)) octets")) }
        guard json.starts(with: Self.debutJSON) else { return abimee(tr("ne commence pas par {\"v\":")) }
        guard json.last == UInt8(ascii: "}") else { return abimee(tr("ne finit pas par }")) }

        switch DecodeurMessages.decoder(json: Data(json)) {
        case .valide(let l):
            compteurs.lignesMachine += 1
            if l.message == .inconnu { compteurs.typesInconnus += 1 }
            sortie.append(.machine(l))
        case .abimee(let raison):
            abimee(raison)
        case .versionInconnue(let v, let t):
            compteurs.versionsInconnues += 1
            sortie.append(.versionInconnue(v: v, t: t))
        case .invalide(let t, let raison):
            compteurs.messagesInvalides += 1
            sortie.append(.invalide(t: t, raison: raison))
        }
    }
}
