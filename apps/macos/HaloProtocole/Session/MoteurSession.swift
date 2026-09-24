import Foundation

/// Compteurs de sante du lien, cote app.
public struct StatistiquesLien: Sendable, Equatable {
    /// Lignes perdues d'apres les trous de `n` (2.4).
    public var pertes = 0
    /// `n` qui recule sans changement de `boot` (lignes anciennes).
    public var reculs = 0
    public var redemarrages = 0
    public var sansReponse = 0
    public var silences = 0
    public var reouvertures = 0
    public var connexions = 0

    public init() {}
}

/// Machine d'etat de la session (section 3), independante du transport.
///
/// Code pur : chaque entree renvoie des effets que l'appelant execute
/// (envoyer des octets, rouvrir le transport...). Le temps est passe en
/// argument (secondes d'une horloge monotone), ce qui la rend testable.
public struct MoteurSession: Sendable {
    public struct Parametres: Sendable, Equatable {
        /// Sans `hello` 2 s apres `json 1` : renvoyer...
        public var delaiHello: TimeInterval = 2
        /// ... 3 fois.
        public var renvoisHello = 3
        /// Puis `\x15\n` et `json 1` toutes les 30 s, pas plus souvent.
        public var relanceLente: TimeInterval = 30
        /// `json ping` apres 10 s sans autre commande (bail de 30 s).
        public var pingApres: TimeInterval = 10
        /// Silence : aucune ligne depuis 3 x max(periode, 2 s).
        public var facteurSilence: Double = 3
        public var silenceMin: TimeInterval = 2
        /// Sans `hello` sous 5 s apres le `json 1` du silence : rouvrir le port.
        public var delaiResynchro: TimeInterval = 5
        /// Commande de banc de plus de 20 min : proposer de fermer le port.
        public var bancLong: TimeInterval = 20 * 60

        public init() {}
    }

    public enum Phase: Sendable, Equatable {
        case ferme
        /// `json 1` envoye, `hello` attendu (essai 1 a 4).
        case attenteHello(essai: Int)
        /// Mode machine etabli.
        case connecte
        /// Silence de la carte : `json 1` renvoye, `hello` attendu sous 5 s.
        case resynchro
        /// `Commande inconnue : "id=1"` : firmware sans protocole JSON.
        case ancienFirmware
        /// Aucune reponse : ecoute, `json 1` toutes les 30 s.
        case sansReponse
        /// `hello` d'une version majeure non geree : console seule.
        case versionInconnue(Int)
        /// `json 0` : la carte est revenue au mode humain.
        case modeHumain

        /// Les commandes portent un `id` et suivent la correlation.
        public var modeMachine: Bool {
            switch self {
            case .connecte, .resynchro: true
            default: false
            }
        }
    }

    public enum Effet: Sendable, Equatable {
        case envoyer(Data)
        /// Fermer puis rouvrir le transport.
        case rouvrir(String)
        /// Redemarrage de la carte : vider les etats derives, nouveau segment de courbes.
        case redemarrage(ancien: String?, nouveau: String?)
        case note(String, grave: Bool)
        case commandeSansReponse(UUID)
        /// Commande de banc de plus de 20 min.
        case proposerFermeture
    }

    public var parametres = Parametres()
    public private(set) var phase: Phase = .ferme
    public private(set) var correlateur = Correlateur()
    public private(set) var statistiques = StatistiquesLien()
    /// Reglages de session annonces par `hello` (et `json periode`).
    public private(set) var periodeMs = 1000
    public private(set) var bailS: Int?
    public private(set) var boot: String?
    public private(set) var dernierUpS: Int?
    /// Vrai tant que le `hello` de cette connexion n'est pas arrive : les
    /// lignes recues sont des lignes anciennes, restees dans le tampon (3.1).
    public private(set) var historique = true

    private var json1: (numero: Int, envoyeA: TimeInterval)?
    private var dernierEssaiA: TimeInterval = 0
    private var dernierRecuA: TimeInterval = 0
    private var ouvertA: TimeInterval = 0
    private var resynchroDepuis: TimeInterval?
    private var dernierN: UInt32?
    private var bancSignale = false

    public init() {}

    // MARK: - Entrees

    /// Transport ouvert : `\x15\n` puis `id=1 json 1` (3.3).
    public mutating func ouvert(maintenant: TimeInterval) -> [Effet] {
        correlateur.reinitialiser(maintenant: maintenant)
        statistiques.connexions += 1
        phase = .attenteHello(essai: 1)
        historique = true
        dernierN = nil
        ouvertA = maintenant
        dernierRecuA = maintenant
        resynchroDepuis = nil
        bancSignale = false
        return [.envoyer(LigneCommande.effacement)] + envoyerJson1(maintenant: maintenant)
    }

    /// Transport ferme (cable, re-enumeration, liberation du port).
    public mutating func ferme(maintenant: TimeInterval) {
        correlateur.reinitialiser(maintenant: maintenant)
        phase = .ferme
        json1 = nil
        resynchroDepuis = nil
    }

    public mutating func soumettre(_ commande: String, origine: OrigineCommande, fusion: String? = nil,
                                   maintenant: TimeInterval) -> (UUID, [Effet]) {
        let id = correlateur.soumettre(commande, origine: origine, fusion: fusion, maintenant: maintenant)
        return (id, pomper(maintenant: maintenant))
    }

    /// Hors mode machine (ancien firmware, mode humain...) : ligne brute, sans `id`.
    public func ligneBrute(_ commande: String) -> Result<Data, ErreurLigne> {
        LigneCommande.octets(commande, id: nil)
    }

    /// "Liberer le port" : `json 0`, puis l'appelant ferme sans rouvrir (3.1, etape 7).
    public mutating func liberer(maintenant: TimeInterval) -> [Effet] {
        var effets: [Effet] = []
        if phase != .ferme {
            let n = correlateur.reserverNumero()
            effets.append(.envoyer(Data("id=\(n) json 0\n".utf8)))
        }
        ferme(maintenant: maintenant)
        return effets
    }

    /// Relance manuelle apres un echec (ancien firmware flashe, bouton "Reessayer").
    public mutating func reessayer(maintenant: TimeInterval) -> [Effet] {
        guard phase != .ferme else { return [] }
        phase = .attenteHello(essai: 1)
        return [.envoyer(LigneCommande.effacement)] + envoyerJson1(maintenant: maintenant)
    }

    public mutating func recu(_ element: ElementRecu, maintenant: TimeInterval) -> [Effet] {
        dernierRecuA = maintenant
        var effets: [Effet] = []
        switch element {
        case .texte(let t):
            if ClasseurTexte.estRefusIdAncienFirmware(t.texte), !phase.modeMachine, phase != .ancienFirmware {
                phase = .ancienFirmware
                json1 = nil
                correlateur.reinitialiser(maintenant: maintenant)
                effets.append(.note("Firmware sans protocole JSON : flasher une version 0.4.0 ou plus. "
                                    + "L'app reste en console seule.", grave: true))
            } else if t.classe == .commande || t.classe == .annonce {
                correlateur.texte(t.texte)
            }
            if t.classe == .demarrage {
                effets.append(.note("Texte de démarrage reçu : la carte a peut-être redémarré.", grave: false))
            }
        case .machine(let l):
            continuite(l.enveloppe.n)
            effets += traiter(l, maintenant: maintenant)
        case .versionInconnue(let v, let t) where t == "hello":
            phase = .versionInconnue(v)
            json1 = nil
            effets.append(.note("Protocole v\(v) non géré par cette app (v1) : console seule.", grave: true))
        default:
            break
        }
        return effets + pomper(maintenant: maintenant)
    }

    public mutating func tic(maintenant: TimeInterval) -> [Effet] {
        var effets: [Effet] = []
        switch phase {
        case .attenteHello(let essai):
            if let j = json1, maintenant - j.envoyeA >= parametres.delaiHello {
                if essai <= parametres.renvoisHello {
                    phase = .attenteHello(essai: essai + 1)
                    effets += envoyerJson1(maintenant: maintenant)
                } else {
                    phase = .sansReponse
                    json1 = nil
                    statistiques.sansReponse += 1
                    effets.append(.note("Aucune réponse : mauvais port, carte en mode téléchargement, "
                                        + "ou commande de banc en cours ? Nouvel essai toutes les 30 s.",
                                        grave: true))
                }
            }
        case .sansReponse:
            if maintenant - dernierEssaiA >= parametres.relanceLente {
                effets.append(.envoyer(LigneCommande.effacement))
                effets += envoyerJson1(maintenant: maintenant)
            }
        case .connecte:
            let limite = parametres.facteurSilence * max(Double(periodeMs) / 1000, parametres.silenceMin)
            if correlateur.commandeDeBanc == nil, maintenant - dernierRecuA >= limite {
                phase = .resynchro
                resynchroDepuis = maintenant
                statistiques.silences += 1
                effets.append(.note("Silence de la carte depuis \(Int(limite)) s : json 1 renvoyé.", grave: false))
                effets += envoyerJson1(maintenant: maintenant)
            }
        case .resynchro:
            if let d = resynchroDepuis, maintenant - d >= parametres.delaiResynchro {
                resynchroDepuis = nil
                statistiques.reouvertures += 1
                effets.append(.rouvrir("Pas de réponse à json 1 sous 5 s : fermeture et réouverture du port."))
            }
        default:
            break
        }

        if phase.modeMachine {
            for s in correlateur.verifierDelais(maintenant: maintenant) {
                statistiques.sansReponse += 1
                effets.append(.commandeSansReponse(s.id))
                if s.origine != .session {
                    correlateur.soumettre("json etat", origine: .session, maintenant: maintenant)
                }
            }
            if let banc = correlateur.commandeDeBanc, let d = banc.debutA {
                if maintenant - d >= parametres.bancLong, !bancSignale {
                    bancSignale = true
                    effets.append(.proposerFermeture)
                }
            } else {
                bancSignale = false
            }
            if phase == .connecte, !correlateur.occupe,
               maintenant - (correlateur.dernierEnvoiA ?? ouvertA) >= parametres.pingApres {
                correlateur.soumettre("json ping", origine: .session, maintenant: maintenant)
            }
        }
        return effets + pomper(maintenant: maintenant)
    }

    // MARK: - Interne

    private mutating func envoyerJson1(maintenant: TimeInterval) -> [Effet] {
        let n = correlateur.reserverNumero()
        json1 = (n, maintenant)
        dernierEssaiA = maintenant
        correlateur.noterEnvoiHorsFile(maintenant: maintenant)
        return [.envoyer(Data("id=\(n) json 1\n".utf8))]
    }

    private mutating func pomper(maintenant: TimeInterval) -> [Effet] {
        guard phase == .connecte, let e = correlateur.prochainEnvoi(maintenant: maintenant) else { return [] }
        return [.envoyer(e.octets)]
    }

    /// Controle de continuite de `n` (2.4).
    private mutating func continuite(_ n: UInt32) {
        defer { dernierN = n }
        guard let d = dernierN else { return }
        let attendu = d &+ 1
        let ecart = n &- attendu
        if ecart == 0 { return }
        if ecart < 0x8000_0000 {
            statistiques.pertes += Int(ecart)
        } else {
            statistiques.reculs += 1
        }
    }

    /// Redemarrage : `boot` change, ou `up_s` recule (3.6).
    private mutating func verifierDemarrage(boot b: String?, upS: Int?) -> Effet? {
        var redemarre = false
        let ancien = boot
        if let b {
            if let ancien, ancien != b { redemarre = true }
            boot = b
        }
        if let u = upS {
            if !redemarre, let d = dernierUpS, u < d { redemarre = true }
            dernierUpS = u
        }
        guard redemarre else { return nil }
        statistiques.redemarrages += 1
        return .redemarrage(ancien: ancien, nouveau: boot)
    }

    private mutating func traiter(_ l: LigneMachine, maintenant: TimeInterval) -> [Effet] {
        var effets: [Effet] = []
        switch l.message {
        case .helloBase(let h):
            if let e = verifierDemarrage(boot: h.boot, upS: h.upS) { effets.append(e) }
            if let p = h.session?.periodeMs { periodeMs = p }
            bailS = h.session?.bailS
            recevoirHello(maintenant: maintenant)
        case .helloIdentite(let h):
            if let e = verifierDemarrage(boot: h.boot, upS: nil) { effets.append(e) }
            recevoirHello(maintenant: maintenant)
        case .etatLampe(let b):
            effets += demarrageHorsHello(boot: b.boot, upS: b.upS, maintenant: maintenant)
        case .etatTranches(let b):
            effets += demarrageHorsHello(boot: b.boot, upS: b.upS, maintenant: maintenant)
        case .etatSante(let b):
            effets += demarrageHorsHello(boot: b.boot, upS: b.upS, maintenant: maintenant)
        case .battement(let b):
            effets += demarrageHorsHello(boot: b.boot, upS: b.upS, maintenant: maintenant)
        case .reponse(let r):
            if let j = json1, r.id == j.numero {
                if r.etape != .debut { json1 = nil }
                if let b = r.bailS { bailS = b }
            } else if case .fin(let id, _) = correlateur.recevoir(r, maintenant: maintenant),
                      r.ok, let s = correlateur.suivi(id) {
                appliquerReglage(s.commande)
            }
        case .livraison(let liv):
            correlateur.recevoir(liv, maintenant: maintenant)
        case .fin(let f):
            if f.cause == .bail, phase.modeMachine {
                effets.append(.note("La carte a quitté le mode machine (bail échu) : json 1 renvoyé.", grave: false))
                historique = true
                phase = .attenteHello(essai: 1)
                effets += envoyerJson1(maintenant: maintenant)
            } else if f.cause == .commande {
                phase = .modeHumain
                json1 = nil
            }
        default:
            break
        }
        return effets
    }

    private mutating func recevoirHello(maintenant: TimeInterval) {
        historique = false
        switch phase {
        case .attenteHello, .resynchro, .sansReponse, .ancienFirmware, .modeHumain:
            phase = .connecte
            resynchroDepuis = nil
        default:
            break
        }
    }

    /// Redemarrage vu hors `hello` (etat, hb) : le mode machine est retombe,
    /// renvoyer `json 1` (3.6).
    private mutating func demarrageHorsHello(boot b: String?, upS: Int?, maintenant: TimeInterval) -> [Effet] {
        guard let e = verifierDemarrage(boot: b, upS: upS) else { return [] }
        guard phase.modeMachine else { return [e] }
        historique = true
        phase = .attenteHello(essai: 1)
        return [e] + envoyerJson1(maintenant: maintenant)
    }

    /// `json periode <ms>` accepte : le seuil de silence suit.
    private mutating func appliquerReglage(_ commande: String) {
        let m = LigneCommande.mots(commande)
        if m.count == 3, m[0] == "json", m[1] == "periode", let p = Int(m[2]) { periodeMs = p }
    }
}
