import HaloProtocole
import SwiftUI

@main
struct HaloCompagnonApp: App {
    @State private var pont = Pont()
    /// Lu dans `body` : un changement de langue reconstruit aussi les menus.
    @AppStorage(ReglageLangue.cle) private var choixLangue: ChoixLangue = .systeme

    init() {
        // Avant la premiere vue : les textes calcules partent dans la bonne langue.
        ReglageLangue.appliquerAuLancement()
    }

    /// `--args -ecran trames|graphiques|commandes` : ecran affiche au lancement.
    private static var ecranDemande: Ecran {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "-ecran"), i + 1 < a.count, let e = Ecran(rawValue: a[i + 1]) else { return .tableau }
        return e
    }

    var body: some Scene {
        let _ = choixLangue
        WindowGroup("Halo Compagnon", id: "principale") {
            ContenuPrincipal(ecranInitial: Self.ecranDemande)
                .environment(pont)
                .langueDeLInterface()
                .frame(minWidth: 980, minHeight: 640)
                .task {
                    // "Halo Compagnon.app" --args -demo : demarre directement en mode demo.
                    if CommandLine.arguments.contains("-demo"), pont.source == nil { pont.connecter(.demo) }
                }
        }
        .defaultSize(width: 1280, height: 820)
        .commands {
            // Titres calcules (tr) et non `LocalizedStringKey` : les menus ne
            // recoivent pas la locale de l'environnement des fenetres.
            CommandGroup(after: .newItem) {
                Button(tr("Mode démo")) { pont.connecter(.demo) }
                    .keyboardShortcut("d", modifiers: [.command, .shift])
                Button(tr("Rafraîchir l'état (json etat)")) { pont.rafraichir() }
                    .keyboardShortcut("r", modifiers: [.command])
                    .disabled(!pont.peutCommander)
                Divider()
                Button(tr("Libérer le port")) { pont.libererPort() }
                    .keyboardShortcut("l", modifiers: [.command, .shift])
                    .disabled(pont.phase == .ferme || pont.estDemo)
            }
        }

        Settings {
            Reglages()
                .langueDeLInterface()
        }
    }
}

extension View {
    /// Locale de l'environnement : celle de la langue choisie. `Text("...")`
    /// y cherche sa traduction et les formats (dates, nombres) la suivent.
    func langueDeLInterface() -> some View {
        modifier(LangueDeLInterface())
    }
}

private struct LangueDeLInterface: ViewModifier {
    func body(content: Content) -> some View {
        // Lire la locale ici fait dependre la vue de la langue (Observation).
        content.environment(\.locale, Localisation.partagee.locale)
    }
}
