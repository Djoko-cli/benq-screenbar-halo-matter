import Foundation
import HaloProtocole
import Testing
@testable import HaloCompagnon

@Suite("Source reseau du modele", .serialized, .langue(.francais))
@MainActor
struct PontReseauTests {
    static let nom = "56B1E064401F74EF"
    /// Nom SRP simule par le mode demo (`SimulateurDemo.srpDemo`) : jamais 16
    /// hexa, pour ne jamais se confondre avec un vrai pont (5.2).
    static let nomDemo = "DEMO-HALO"

    @Test func cleAbsenteArreteSansReessayer() {
        let pont = Pont(trousseau: TrousseauMemoire())
        pont.connecter(.reseau(nom: Self.nom))
        #expect(pont.alerteReseau == .trousseau(.absente(Self.nom)))
        if case .erreur = pont.etatTransport {} else { Issue.record("etat \(pont.etatTransport)") }
        // Simule un chemin reseau retrouve (ou un reveil) : une cle absente
        // n'est pas une cause que le reseau puisse lever seul (4.6).
        pont.reseauChange()
        #expect(pont.alerteReseau == .trousseau(.absente(Self.nom)), "cle absente : pas de reprise seule")
        if case .erreur = pont.etatTransport {} else { Issue.record("aucune reprise attendue") }
        pont.deconnecter()
    }

    @Test func echecAvecRepriseAutomatiqueEffaceLAlerteEtReprogramme() {
        let pont = Pont(trousseau: TrousseauMemoire())
        pont.connecter(.reseau(nom: Self.nom))
        // Simule l'echec d'un essai reseau ulterieur : "pas de route" reprend seul.
        pont.echecOuverture(ErreurReseau.pasDeRoute)
        #expect(pont.alerteReseau == nil, "reprise automatique : pas de bandeau d'arret")
        if case .attente = pont.etatTransport {} else { Issue.record("reprise programmee attendue : \(pont.etatTransport)") }
        pont.deconnecter()
    }

    @Test func echecSansRepriseAutomatiqueGardeLAlerteEtArrete() {
        let pont = Pont(trousseau: TrousseauMemoire())
        pont.connecter(.reseau(nom: Self.nom))
        // "Port injoignable" (pont sans cle) : arret net, pas de reessai seul.
        pont.echecOuverture(ErreurReseau.portInjoignable)
        #expect(pont.alerteReseau == .transport(.portInjoignable))
        if case .erreur = pont.etatTransport {} else { Issue.record("arret attendu : \(pont.etatTransport)") }
        pont.deconnecter()
    }

    @Test func causeReseauNoteeUneFoisTantQuElleNeChangePas() {
        let pont = Pont(trousseau: TrousseauMemoire())
        pont.connecter(.reseau(nom: Self.nom))
        func occurrences(_ sousChaine: String) -> Int {
            pont.console.elements.filter { $0.texte.contains(sousChaine) }.count
        }
        #expect(occurrences("Clé absente") == 1)
        // Reessayer sur la meme cause n'ajoute pas de ligne (4.6).
        pont.reconnecter()
        #expect(occurrences("Clé absente") == 1, "meme cause : pas de nouvelle ligne")
        // Une cause differente, elle, est notee.
        pont.echecOuverture(ErreurReseau.pasDeRoute)
        #expect(occurrences("Pas de route IPv6") == 1, "cause differente : nouvelle ligne")
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

    @Test func creerLaCleParLUSB() async throws {
        let reel = TrousseauMemoire()
        let demo = TrousseauMemoire()
        let pont = Pont(trousseau: reel, trousseauDemo: demo)
        pont.connecter(.demo)
        try #require(await attendre { pont.phase == .connecte && pont.accesReseau != .inconnu })
        #expect(pont.accesReseau == .sansCle(nom: Self.nomDemo))
        pont.creerCle()
        try #require(await attendre { !demo.lister().isEmpty })
        let connu = try #require(demo.lister().first)
        #expect(connu.nom == Self.nomDemo)
        let cle = try demo.lire(nom: connu.nom)
        #expect(H1.kid(cle: cle) == connu.empreinte)
        // Isolation de la demo (5.2) : jamais dans le vrai trousseau, jamais dans pontsConnus (barre laterale).
        #expect(reel.lister().isEmpty, "le trousseau reel n'est jamais touche par la demo")
        #expect(pont.pontsConnus.isEmpty, "pontsConnus ne suit que le trousseau reel")
        // Le secret ne traine nulle part (5.2).
        let hexa = H1.hexa(cle)
        #expect(!pont.console.elements.contains { $0.texte.contains(hexa) })
        #expect(!pont.trames.elements.contains { $0.json.contains(hexa) })
        #expect(!pont.trames.elements.contains { entree in
            guard case .reponse(let r) = entree.message else { return false }
            return r.cle != nil
        }, "le message garde par le journal ne porte jamais la cle non plus")
        #expect(!pont.suivis.contains { $0.fin?.cle != nil })
        _ = await attendre { pont.accesReseau == .cleConnue(nom: connu.nom, empreinte: connu.empreinte) }
        #expect(pont.accesReseau == .cleConnue(nom: connu.nom, empreinte: connu.empreinte))
        pont.deconnecter()
    }

    /// Une reconnexion (transport ferme, source reprise) ne doit jamais
    /// laisser une creation de cle perimee bloquer le prochain essai (5.2).
    /// `deconnecter()` envoie `json 0` sans passer par `fermerTransport` (la
    /// file doit encore vider le "json 0") : la reprise n'arrive qu'au
    /// prochain `connecter`, qui appelle toujours `fermerTransport` avant `ouvrir`.
    @Test func laCreationDeCleReprendApresUneReconnexion() async throws {
        let demo = TrousseauMemoire()
        let pont = Pont(trousseau: TrousseauMemoire(), trousseauDemo: demo)
        pont.connecter(.demo)
        try #require(await attendre { pont.phase == .connecte && pont.accesReseau != .inconnu })
        pont.creerCle()
        pont.deconnecter()
        pont.connecter(.demo)
        // La reconnexion a coupe l'ancien essai (fermerTransport) : signale, pas bloque en silence.
        #expect(pont.console.elements.contains { $0.genre == .note(grave: true) && $0.texte.contains("interrompue") })
        try #require(await attendre { pont.phase == .connecte && pont.accesReseau != .inconnu })
        pont.creerCle()
        try #require(await attendre { !demo.lister().isEmpty })
        #expect(demo.lister().first?.nom == Self.nomDemo)
        pont.deconnecter()
    }
}
