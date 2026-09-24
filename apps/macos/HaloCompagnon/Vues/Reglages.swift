import HaloProtocole
import SwiftUI

/// Fenetre Reglages (⌘,) : la langue de l'interface.
struct Reglages: View {
    @AppStorage(ReglageLangue.cle) private var choix: ChoixLangue = .systeme

    var body: some View {
        Form {
            Section {
                Picker("Langue", selection: Binding(get: { choix }, set: { nouveau in
                    choix = nouveau
                    // Dans la meme transaction : textes calcules et locale changent ensemble.
                    ReglageLangue.appliquer(nouveau)
                })) {
                    Text("Langue du système").tag(ChoixLangue.systeme)
                    Divider()
                    // Chaque langue sous son propre nom, quelle que soit la langue en vigueur.
                    Text(verbatim: "English").tag(ChoixLangue.anglais)
                    Text(verbatim: "Français").tag(ChoixLangue.francais)
                }
                .pickerStyle(.menu)
            } footer: {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Le contenu des fenêtres change tout de suite. Les menus de macOS (Halo Compagnon, Édition, Fenêtre…) et les boîtes du système suivent au prochain lancement. Les lignes déjà écrites dans la console gardent leur langue.")
                    if ReglageLangue.relancePourLesMenus {
                        Label("Relancer l'app pour mettre aussi les menus dans cette langue.",
                              systemImage: "arrow.clockwise.circle")
                            .foregroundStyle(.orange)
                    }
                }
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .frame(width: 460)
        .fixedSize(horizontal: false, vertical: true)
    }
}
