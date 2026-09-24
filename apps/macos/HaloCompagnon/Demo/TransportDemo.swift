import Foundation
import HaloProtocole
import Synchronization

/// Transport du mode demo : une carte simulee rejoue `demo-halo.jsonl`.
///
/// Chaque ouverture est un nouveau demarrage de la carte (nouveau `boot`) ; a
/// la fin de la chronologie, la carte "redemarre" et le flux se ferme, comme
/// une re-enumeration USB : l'app se reconnecte seule.
final class TransportDemo: Transport {
    let genre: GenreTransport = .demo
    /// Nom montre a l'utilisateur, dans la langue en vigueur.
    static var nomLisible: String { tr("Démo (rejeu de demo-halo.jsonl)") }
    var nom: String { Self.nomLisible }

    private struct Etat {
        var tache: Task<Void, Never>?
        var entrees: AsyncStream<Data>.Continuation?
        var sortie: AsyncStream<EvenementTransport>.Continuation?
        var ouvertures = 0
    }

    private let script: ScriptDemo
    private let vitesse: Double
    private let etat = Mutex(Etat())

    /// `vitesse` : facteur du temps de la carte (les tests rejouent toute la chronologie en quelques secondes).
    init(vitesse: Double = 1) throws {
        script = try ScriptDemo.charger()
        self.vitesse = vitesse
    }

    func ouvrir() async throws -> AsyncStream<EvenementTransport> {
        let (flux, sortie) = AsyncStream.makeStream(of: EvenementTransport.self, bufferingPolicy: .unbounded)
        let (entrees, suiteEntrees) = AsyncStream.makeStream(of: Data.self, bufferingPolicy: .unbounded)
        let premiere = etat.withLock { e in
            e.ouvertures += 1
            return e.ouvertures == 1
        }
        let boot = premiere ? script.boot : String(format: "%08X", UInt32.random(in: .min ... .max))
        let simulateur = SimulateurDemo(script: script, sortie: sortie, boot: boot, vitesse: vitesse)
        let tache = Task.detached(priority: .userInitiated) {
            await simulateur.executer(entrees: entrees)
            sortie.finish()
        }
        etat.withLock { e in
            e.tache = tache
            e.entrees = suiteEntrees
            e.sortie = sortie
        }
        return flux
    }

    func envoyer(_ donnees: Data) throws {
        guard let entrees = etat.withLock({ $0.entrees }) else { throw ErreurTransport(tr("démo arrêtée")) }
        entrees.yield(donnees)
    }

    func fermer() {
        let (tache, entrees, sortie) = etat.withLock { e in
            let r = (e.tache, e.entrees, e.sortie)
            e.tache = nil
            e.entrees = nil
            e.sortie = nil
            return r
        }
        entrees?.finish()
        tache?.cancel()
        sortie?.yield(.ferme(raison: tr("démo arrêtée")))
        sortie?.finish()
    }
}
