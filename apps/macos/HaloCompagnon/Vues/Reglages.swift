import HaloProtocole
import ServiceManagement
import SwiftUI

/// Fenetre Reglages (⌘,) : la langue de l'interface et Thread Route.
/// Onglets de la fenetre Reglages ; le choix est garde, et la carte Thread
/// du tableau de bord ouvre directement "Acces reseau Thread".
enum OngletReglages: String {
    case general, accesReseau
    static let cle = "reglages.onglet"
}

/// Fenetre Reglages : Général (langue) et Accès réseau Thread (cles des ponts).
struct FenetreReglages: View {
    @AppStorage(OngletReglages.cle) private var onglet: OngletReglages = .general

    var body: some View {
        TabView(selection: $onglet) {
            Tab("Général", systemImage: "gearshape", value: OngletReglages.general) {
                Reglages()
            }
            Tab("Accès réseau Thread", systemImage: "point.3.connected.trianglepath.dotted",
                value: OngletReglages.accesReseau) {
                ReglagesAccesReseau()
            }
        }
    }
}

/// Onglet Général : langue de l'app, mises a jour, Thread Route.
struct Reglages: View {
    @AppStorage(ReglageLangue.cle) private var choix: ChoixLangue = .systeme
    @Environment(MisesAJour.self) private var misesAJour

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
                    Text("Le contenu des fenêtres change tout de suite. Les menus de macOS (Halo Compagnon, Édition, Fenêtre…) et les boîtes du système suivent au prochain lancement. Les textes déjà consignés gardent leur langue jusqu'au suivant : lignes de la console, erreur de connexion, dernière ligne rejetée, erreur de saisie de la console.")
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
            // Les mises a jour (Sparkle) : recherche et installation automatiques, cochees par defaut.
            Section("Mises à jour") {
                Toggle("Rechercher automatiquement",
                       isOn: Binding(get: { misesAJour.rechercheAuto }, set: { misesAJour.rechercheAuto = $0 }))
                Toggle("Installer automatiquement",
                       isOn: Binding(get: { misesAJour.installationAuto }, set: { misesAJour.installationAuto = $0 }))
                    .disabled(!misesAJour.rechercheAuto)
                Button("Rechercher les mises à jour…") { misesAJour.rechercher() }
                    .disabled(!misesAJour.peutRechercher)
            }
            SectionThreadRoute()
        }
        .formStyle(.grouped)
        .frame(width: 460)
        .fixedSize(horizontal: false, vertical: true)
    }
}

/// Onglet General : Thread Route, le demon qui garde la route du Mac vers le reseau Thread (10.1). Son etat
/// est relu a chaque apparition de l'onglet et a chaque retour de l'app au premier plan : une installation ou
/// une approbation (Reglages Systeme) faites entre-temps s'y voient.
struct SectionThreadRoute: View {
    @State private var etat = EtatThreadRoute.lire()

    var body: some View {
        Section("Thread Route") {
            LabeledContent("État", value: etat.libelle)
            if let c = etat.consigne {
                Text(c)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if etat == .aApprouver {
                Button("Ouvrir Réglages Système…") { SMAppService.openSystemSettingsLoginItems() }
            }
        }
        .onAppear { etat = EtatThreadRoute.lire() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            etat = EtatThreadRoute.lire()
        }
    }
}
