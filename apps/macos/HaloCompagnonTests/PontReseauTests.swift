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

    /// Le suivi de la creation peut se terminer sans jamais recevoir de
    /// reponse (ligne abimee, redemarrage...) : `creerCle()` ne doit pas
    /// rester bloque en silence pour autant (5.1). `avancerPourUnTest` simule
    /// l'echeance du delai de reponse (3 s en USB/demo) sans attendre les
    /// secondes reelles : le suivi du premier essai passe "sans reponse"
    /// (issue finale) alors que `creationCle` reste arme (round 1). Deux
    /// avances sont necessaires : la premiere (courte) fait sortir "json cle
    /// nouvelle" de la file d'attente (cadence limitee a 20 lignes/s, 6.5),
    /// sans quoi la commande n'est pas encore "envoyee" quand la seconde
    /// avance verifie le delai ; la seconde declenche ce delai sur la
    /// commande desormais effectivement envoyee. Avancer l'horloge de la
    /// session en avance de l'horloge reelle du pont retarde d'autant les
    /// envois suivants (la cadence les compare a nouveau a l'horloge reelle) :
    /// la confirmation finale que la cle est bien rangee prend donc, elle,
    /// plusieurs secondes reelles (`attendre` les couvre, sans sommeil fixe).
    ///
    /// La demo, elle, ne perd jamais rien : la reponse au premier essai finit
    /// toujours par arriver (elle n'est ignoree que parce que le second essai
    /// a remplace `creationCle` entre-temps, round 2). Verifier seulement
    /// qu'une cle finit par etre rangee ne distinguerait donc pas le correctif
    /// du bogue (la reponse tardive au premier essai la rangerait aussi,
    /// round 1) : le test compte d'abord les demandes envoyees (note
    /// "Nouvelle clé…") pour verifier que le second appel a bien reussi a en
    /// envoyer une deuxieme, avant de confirmer le resultat final.
    @Test func laCreationDeCleReprendApresUnSuiviTermineSansReponse() async throws {
        let demo = TrousseauMemoire()
        let pont = Pont(trousseau: TrousseauMemoire(), trousseauDemo: demo)
        pont.connecter(.demo)
        try #require(await attendre { pont.phase == .connecte && pont.accesReseau != .inconnu })
        func demandes() -> Int {
            pont.console.elements.filter { $0.texte.contains("Nouvelle clé réseau demandée") }.count
        }
        pont.creerCle()
        #expect(demandes() == 1)
        // Meme thread (@MainActor), aucun `await` entre les lignes qui suivent : la
        // reponse simulee ne peut pas arriver avant que le second essai ne remplace le premier.
        pont.avancerPourUnTest(de: 0.1)
        pont.avancerPourUnTest(de: 3.5)
        pont.creerCle()
        #expect(demandes() == 2, "le suivi du premier essai est termine (sans reponse simulee) : un second essai est accepte")
        try #require(await attendre { !demo.lister().isEmpty })
        #expect(demo.lister().first?.nom == Self.nomDemo)
        pont.deconnecter()
    }
}
