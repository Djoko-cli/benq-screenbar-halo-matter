import SwiftUI

@main
struct HaloCompagnonApp: App {
    @State private var pont = Pont()

    /// `--args -ecran trames|graphiques|commandes` : ecran affiche au lancement.
    private static var ecranDemande: Ecran {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "-ecran"), i + 1 < a.count, let e = Ecran(rawValue: a[i + 1]) else { return .tableau }
        return e
    }

    var body: some Scene {
        WindowGroup("Halo Compagnon", id: "principale") {
            ContenuPrincipal(ecranInitial: Self.ecranDemande)
                .environment(pont)
                .frame(minWidth: 980, minHeight: 640)
                .task {
                    // "Halo Compagnon.app" --args -demo : demarre directement en mode demo.
                    if CommandLine.arguments.contains("-demo"), pont.source == nil { pont.connecter(.demo) }
                }
        }
        .defaultSize(width: 1280, height: 820)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Mode démo") { pont.connecter(.demo) }
                    .keyboardShortcut("d", modifiers: [.command, .shift])
                Button("Rafraîchir l'état (json etat)") { pont.rafraichir() }
                    .keyboardShortcut("r", modifiers: [.command])
                    .disabled(!pont.peutCommander)
                Divider()
                Button("Libérer le port") { pont.libererPort() }
                    .keyboardShortcut("l", modifiers: [.command, .shift])
                    .disabled(pont.phase == .ferme || pont.estDemo)
            }
        }
    }
}
