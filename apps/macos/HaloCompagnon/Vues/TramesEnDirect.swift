import HaloProtocole
import SwiftUI

/// Ecran "Trames en direct" : les evenements `rx`, `tx`, `livraison`,
/// `relance` (et les autres), filtres, avec leur sens decode.
struct TramesEnDirect: View {
    @Environment(Pont.self) private var pont
    @State private var categories: Set<CategorieTrame> = [.rx, .tx, .livraison, .relance, .module]
    @State private var masquerAccuses = false
    @State private var seulementEchecs = false
    @State private var recherche = ""
    @State private var selection: EntreeTrame.ID?
    @State private var figees: [EntreeTrame]?

    var body: some View {
        let source = figees ?? pont.trames.elements
        let lignes = filtrer(source)
        VStack(spacing: 0) {
            barreFiltres
            Divider()
            HSplitView {
                Table(lignes, selection: $selection) {
                    TableColumn("Heure") { e in
                        Text(Format.heure(e.date)).monospacedDigit().foregroundStyle(style(e))
                    }
                    .width(min: 90, ideal: 100, max: 110)
                    TableColumn(Text(verbatim: "n")) { e in
                        Text(String(e.n)).monospacedDigit().foregroundStyle(.secondary)
                    }
                    .width(min: 40, ideal: 55, max: 80)
                    TableColumn("Type") { e in
                        Pastille(texte: e.type, couleur: couleur(e))
                    }
                    .width(min: 70, ideal: 90, max: 110)
                    TableColumn("Sens décodé") { e in
                        // Une ligne par trame : des hauteurs variables font sauter le tableau en direct.
                        Text(e.resume)
                            .foregroundStyle(style(e))
                            .italic(e.historique)
                            .lineLimit(1)
                            .truncationMode(.tail)
                            .help(e.resume)
                    }
                }
                .frame(minWidth: 520)
                Detail(entree: source.first { $0.id == selection })
                    .frame(minWidth: 280, idealWidth: 340, maxWidth: 480)
            }
            Divider()
            HStack {
                Text("\(lignes.count) affichées sur \(source.count)")
                if figees != nil { Pastille("affichage figé", couleur: .orange) }
                if pont.tramesCoupees { Pastille("flux rx/tx coupé (json trames 0)", couleur: .orange) }
                Spacer()
                Text("Gris : CRC faux (bits douteux) · italique : lignes anciennes, avant le hello")
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
        }
        // Affichage fige : sa copie du journal suit aussi la langue.
        .onChange(of: Localisation.partagee.langue) {
            figees = figees?.map { $0.relocalisee(correspondance: $0.gamma.correspondance) }
        }
    }

    private var barreFiltres: some View {
        HStack(spacing: 10) {
            Menu {
                ForEach(CategorieTrame.allCases) { c in
                    Toggle(c.libelle, isOn: Binding(
                        get: { categories.contains(c) },
                        set: { if $0 { categories.insert(c) } else { categories.remove(c) } }))
                }
                Divider()
                Button("Tout") { categories = Set(CategorieTrame.allCases) }
                Button("Radio seulement") { categories = [.rx, .tx] }
            } label: {
                Label("Types (\(categories.count))", systemImage: "line.3.horizontal.decrease.circle")
            }
            .fixedSize()
            Toggle("Sans accusés de la lampe", isOn: $masquerAccuses)
            Toggle("Échecs seulement", isOn: $seulementEchecs)
            TextField("Rechercher (charge, type, texte)", text: $recherche)
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 260)
            Spacer()
            Button {
                figees = figees == nil ? pont.trames.elements : nil
            } label: {
                Label(figees == nil ? tr("Figer") : tr("Reprendre"), systemImage: figees == nil ? "pause" : "play")
            }
            .help("Figer l'affichage sans rien demander à la carte")
            Menu {
                // L'app se regle sur hello.caps, pas sur la version du firmware (5.1).
                let caps = pont.etat.capacites
                if caps.contains("trames") {
                    Button(pont.tramesCoupees ? tr("Reprendre rx/tx (json trames 1)") : tr("Couper rx/tx (json trames 0)")) {
                        pont.couperTrames(!pont.tramesCoupees)
                    }
                    .disabled(!pont.peutCommander)
                }
                if caps.contains("log") {
                    Button("Annonces en messages log (json log 1)") { pont.envoyer("json log 1") }
                        .disabled(!pont.peutCommander || pont.reglages?.log == true)
                    Button("Annonces en texte (json log 0)") { pont.envoyer("json log 0") }
                        .disabled(!pont.peutCommander || pont.reglages?.log == false)
                }
                if !caps.contains("trames") && !caps.contains("log") {
                    Text(caps.isEmpty ? tr("Capacités de la carte pas encore reçues") : tr("Ni trames ni log dans ce build"))
                }
                Divider()
                Button("Écoute de fond active (lampe ecoute 1)") { pont.envoyer("lampe ecoute 1") }
                    .disabled(!pont.peutCommander)
                Button("Écoute de fond coupée (lampe ecoute 0)") { pont.envoyer("lampe ecoute 0") }
                    .disabled(!pont.peutCommander)
                Divider()
                Button("Vider le journal") { pont.viderJournal(); figees = nil }
            } label: {
                Label("Flux", systemImage: "antenna.radiowaves.left.and.right")
            }
            .fixedSize()
        }
        .toggleStyle(.checkbox)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    private func filtrer(_ source: [EntreeTrame]) -> [EntreeTrame] {
        let r = recherche.trimmingCharacters(in: .whitespaces).lowercased()
        return source.reversed().filter { e in
            guard categories.contains(e.categorie) else { return false }
            if masquerAccuses, case .rx(let t) = e.message, t.type == .accuseLampe { return false }
            if seulementEchecs, !e.echec, !e.douteuse { return false }
            if !r.isEmpty, !e.cleRecherche.contains(r) { return false }
            return true
        }
    }

    private func style(_ e: EntreeTrame) -> Color {
        if e.douteuse || e.historique { return .secondary }
        if e.echec { return .red }
        return .primary
    }

    private func couleur(_ e: EntreeTrame) -> Color {
        if e.douteuse { return .gray }
        if e.echec { return .red }
        switch e.categorie {
        case .rx: return .blue
        case .tx: return .teal
        case .livraison: return .green
        case .relance, .module: return .orange
        case .matter: return .purple
        default: return .secondary
        }
    }
}

private struct Detail: View {
    let entree: EntreeTrame?

    var body: some View {
        ScrollView {
            if let e = entree {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Pastille(texte: e.type, couleur: .blue)
                        Text(verbatim: "n \(e.n) · ms \(e.ms.map(String.init) ?? "–")").foregroundStyle(.secondary).monospacedDigit()
                    }
                    Text(e.resume).font(.title3)
                    if e.historique {
                        Text("Ligne ancienne : produite avant la réponse à json 1 (tampon de la carte à l'ouverture).")
                            .font(.caption).foregroundStyle(.orange)
                    }
                    if e.douteuse {
                        Text("CRC faux : longueur, PID et charge sont décodés de bits douteux.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    champs(e)
                    Divider()
                    Text("JSON reçu").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                    Text(Self.joli(e.json))
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                Text("Choisir une trame pour voir son détail.")
                    .foregroundStyle(.secondary)
                    .padding(20)
            }
        }
    }

    @ViewBuilder
    private func champs(_ e: EntreeTrame) -> some View {
        switch e.message {
        case .rx(let r):
            LigneInfo("Source", r.source == .accuse ? tr("dans une fenêtre d'accusé") : tr("écoute passive"))
            LigneInfo("Brut (8 octets après l'adresse)", r.brut, mono: true)
            LigneInfo("PCF : longueur · PID · NO_ACK", "\(r.len.map(String.init) ?? "–") · \(r.pid.map(String.init) ?? "–") · \(r.noAck.map(String.init) ?? "–")", mono: true)
            LigneInfo("Charge", r.charge.map { $0.isEmpty ? tr("(vide)") : $0 }, mono: true)
            LigneInfo("CRC", r.crc.map { r.crcOk == true ? tr("\($0) juste") : tr("\($0) FAUX") }, couleur: r.crcOk == false ? .red : nil, mono: true)
            LigneInfo("Classement", r.type.libelle)
        case .tx(let t):
            LigneInfo("Tranche · charge", "\(t.tranche?.libelle ?? "?") · \(t.charge ?? "")", mono: true)
            LigneInfo("Essai · prévus · accusés", "\(t.essai ?? 0) · \(t.paquets ?? 0) · \(t.accuses ?? 0)")
            LigneInfo("Verdict", t.verdict.libelle, couleur: t.verdict.estEchec ? .red : .green)
            LigneInfo("Durée (CE=1 à TX_DS/MAX_RT)", t.us.map { "\($0) µs" })
            LigneInfo("RT2 · IRQ1 · STATUS", "\(t.rt2 ?? "–") · \(t.irq1 ?? "–") · \(t.status ?? "–")", mono: true)
            Text("Repères du banc : accusé ~1600-1720 µs, IRQ1 2E, RT2 00 ; MAX_RT ~11470 µs, IRQ1 1E, RT2 10.")
                .font(.caption).foregroundStyle(.secondary)
        case .livraison(let l):
            LigneInfo("Issue", l.issue.libelle, couleur: l.issue == .livree ? .green : .red)
            LigneInfo("Consigne", Interpretation.etat(l.consigne))
            LigneInfo("Cru", Interpretation.etat(l.cru))
            LigneInfo("Commandes de l'app", (l.ids ?? []).isEmpty ? tr("aucune (Matter, télécommande...)") : l.ids!.map(String.init).joined(separator: ", "))
            LigneInfo("Attente", l.attenteMs.map { "\($0) ms" })
        case .relance(let r):
            LigneInfo("Cause", r.cause.libelle)
            LigneInfo("Rang", r.rang.map(String.init))
            LigneInfo("Résultat", r.ok == true ? tr("réussie") : tr("échec"), couleur: r.ok == true ? .green : .red)
            LigneInfo("Durée (boucle bloquée)", r.dureeMs.map { "\($0) ms" })
            LigneInfo("Total · EN PANNE", "\(r.total ?? 0) · \(Format.oui(r.panne))")
        case .intent(let i):
            LigneInfo("Fenêtre", i.fenetreMs.map { "\($0) ms" })
            LigneInfo("Champs", Interpretation.champs(i.champs))
            LigneInfo("Consigne", Interpretation.etat(i.consigne))
        default:
            EmptyView()
        }
    }

    static func joli(_ json: String) -> String {
        guard let o = try? JSONSerialization.jsonObject(with: Data(json.utf8)),
              let d = try? JSONSerialization.data(withJSONObject: o, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
        else { return json }
        return String(decoding: d, as: UTF8.self)
    }
}
