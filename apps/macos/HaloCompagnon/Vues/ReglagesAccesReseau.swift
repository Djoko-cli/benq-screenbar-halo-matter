import HaloProtocole
import SwiftUI

/// Reglages › Acces reseau Thread : ponts dont ce Mac a la cle, et cle du
/// pont branche en USB (10.4 : la cle ne passe que par l'USB).
struct ReglagesAccesReseau: View {
    @Environment(Pont.self) private var pont
    @State private var aOublier: PontConnu?

    var body: some View {
        Form {
            Section {
                if pont.pontsConnus.isEmpty {
                    Text("Aucun pont : brancher un pont en USB, le connecter (menu Source), puis « Activer l'accès réseau… » ci-dessous.")
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ForEach(pont.pontsConnus) { p in
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(pont.titre(pont: p))
                            Text("\(p.hote) · clé \(p.empreinte)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Oublier…", role: .destructive) { aOublier = p }
                            .controlSize(.small)
                    }
                }
            } header: {
                Text("Ponts connus de ce Mac")
            }
            Section {
                if pont.accesReseau == .inconnu {
                    Text("Brancher le pont en USB et le connecter (menu Source) pour créer ou renouveler sa clé.")
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    AccesReseau()
                }
            } header: {
                Text("Pont branché en USB")
            } footer: {
                Text("La clé ne passe que par l'USB. Elle est rangée dans le trousseau de ce Mac, où halo_udp.py la relit : à chaque nouvelle clé, macOS redemande l'accès pour security.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .frame(width: 460)
        .fixedSize(horizontal: false, vertical: true)
        .confirmationDialog("Oublier ce pont ?", isPresented: Binding(get: { aOublier != nil }, set: { if !$0 { aOublier = nil } }),
                            presenting: aOublier) { p in
            Button("Oublier \(p.hote)", role: .destructive) { pont.oublierPont(p.nom) }
        } message: { p in
            Text("La clé de \(p.hote) est retirée du trousseau de ce Mac ; le pont garde la sienne. Pour revenir : « Activer l'accès réseau » par l'USB crée une nouvelle clé.")
        }
    }
}

/// Cle du transport reseau du pont branche en USB (10.4) : etat et creation.
struct AccesReseau: View {
    @Environment(Pont.self) private var pont
    @State private var confirmation = false

    var body: some View {
        HStack {
            switch pont.accesReseau {
            case .sansCle:
                Text("Accès réseau : aucune clé").foregroundStyle(.secondary)
                Spacer()
                Button("Activer l'accès réseau…") { confirmation = true }
            case .cleConnue(_, let e):
                Text("Clé \(e) connue de ce Mac").foregroundStyle(.secondary)
                Spacer()
                Button("Nouvelle clé…") { confirmation = true }
            case .cleInconnue(_, let e):
                Text("Clé \(e) inconnue de ce Mac").foregroundStyle(.orange)
                Spacer()
                Button("Nouvelle clé…") { confirmation = true }
            case .inconnu:
                EmptyView()
            }
        }
        .font(.callout)
        .controlSize(.small)
        .disabled(!pont.peutCommander)
        .confirmationDialog("Créer une nouvelle clé réseau ?", isPresented: $confirmation) {
            Button("Créer la clé") { pont.creerCle() }
        } message: {
            Text("La carte remplace sa clé : les sessions réseau en cours tombent. La nouvelle clé est rangée dans le trousseau de ce Mac ; halo_udp.py l'y relira.")
        }
    }
}
