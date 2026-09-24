import Charts
import HaloProtocole
import SwiftUI

/// Ecran "Commandes et console" : les commandes de la lampe (6.4) a gauche,
/// la console brute a droite.
struct CommandesEtConsole: View {
    var body: some View {
        HSplitView {
            PanneauCommandes()
                .frame(minWidth: 400, idealWidth: 460, maxWidth: 560)
            ConsoleBrute()
                .frame(minWidth: 420)
        }
    }
}

// MARK: - Commandes

private struct PanneauCommandes: View {
    @Environment(Pont.self) private var pont
    @State private var niveau: Double = 180
    @State private var mired: Double = 268
    @State private var glisseNiveau = false
    @State private var glisseMired = false
    @State private var lumHexa = "A5"
    @State private var tempBrute = 53

    var body: some View {
        let consigne = pont.etat.lampe?.valeur.consigne
        let correspondance = pont.etat.correspondance
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                if !pont.peutCommander {
                    Bandeau(texte: tr("Pas de session machine : les commandes sont désactivées."), couleur: .secondary,
                            icone: "bolt.horizontal.circle")
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                } else if !pont.etat.capacites.isEmpty, !pont.etat.capacites.contains("lampe_async") {
                    // L'app se regle sur caps (5.1) : sans lampe_async, une commande lampe avec id
                    // reste historique (reponse debut, texte, fin) et bloque la boucle de la carte.
                    Bandeau(texte: tr("Ce firmware n'a pas les commandes lampe asynchrones (capacité lampe_async) : chaque commande lampe bloque la carte jusqu'à 6 s, sans livraison."),
                            couleur: .orange, icone: "hourglass")
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                }
                Carte(titre: "Marche et lampes", icone: "power") {
                    HStack {
                        Text(Interpretation.etat(consigne)).font(.callout).foregroundStyle(.secondary)
                        Spacer()
                    }
                    HStack {
                        Button { pont.envoyer("lampe on") } label: { Label("Allumer", systemImage: "lightbulb.fill") }
                        Button { pont.envoyer("lampe off") } label: { Label("Éteindre", systemImage: "lightbulb.slash") }
                        Spacer()
                        Button { pont.envoyer("lampe sync") } label: { Label("Resynchroniser", systemImage: "arrow.triangle.2.circlepath") }
                            .help("lampe sync : tout est renvoyé")
                    }
                    Picker("Lampes", selection: Binding(
                        get: { consigne?.lampes ?? .deux },
                        set: { pont.envoyer("lampe mode \($0.rawValue)") })) {
                        Text("Avant").tag(Lampes.avant)
                        Text("Arrière").tag(Lampes.arriere)
                        Text("Les deux").tag(Lampes.deux)
                    }
                    .pickerStyle(.segmented)
                    .help("lampe mode avant|arriere|deux (allume aussi)")
                    HStack {
                        Text("Une lampe :").foregroundStyle(.secondary)
                        Button("Avant on") { pont.envoyer("lampe avant on") }
                        Button("Avant off") { pont.envoyer("lampe avant off") }
                        Button("Arrière on") { pont.envoyer("lampe arriere on") }
                        Button("Arrière off") { pont.envoyer("lampe arriere off") }
                    }
                    .controlSize(.small)
                    HStack {
                        Button { pont.envoyer("lampe auto") } label: { Label("Bouton A (mode auto)", systemImage: "a.circle") }
                            .help("lampe auto : refusé lampe éteinte. Pas de réémission automatique (non idempotent).")
                        Spacer()
                    }
                }

                Carte(titre: "Luminosité (curseur Matter)", icone: "sun.max") {
                    let n = Int(niveau)
                    let brut = correspondance.brut(niveau: n)
                    HStack {
                        Text("Niveau \(n)").font(.title3.weight(.semibold)).monospacedDigit()
                        Text("· \(CorrespondanceLuminosite.pourcent(niveau: n)) % · brut \(Format.hexa(brut))")
                            .foregroundStyle(.secondary).monospacedDigit()
                        Spacer()
                        Text(verbatim: "gamma \(Interpretation.decimal(correspondance.gamma, chiffres: 2))").font(.caption).foregroundStyle(.secondary)
                    }
                    Slider(value: $niveau, in: Double(correspondance.plancher)...254, step: 1) {
                        Text("Niveau")
                    } minimumValueLabel: {
                        Text(verbatim: "\(correspondance.plancher)")
                    } maximumValueLabel: {
                        Text(verbatim: "254")
                    } onEditingChanged: { enCours in
                        glisseNiveau = enCours
                        if !enCours { pont.curseur("lampe niveau \(Int(niveau))", cle: "niveau", fini: true) }
                    }
                    .onChange(of: niveau) { _, v in
                        if glisseNiveau { pont.curseur("lampe niveau \(Int(v))", cle: "niveau", fini: false) }
                    }
                    CourbeGamma(correspondance: correspondance, niveau: n, brutCru: pont.etat.lampe?.valeur.cru.lum)
                        .frame(height: 120)
                    Text("Brut = 0x4C + round(178 × ((niveau − 1) / 253)^gamma) ; les niveaux 1 à \(correspondance.plancher) donnent 4C (plancher rapporté à Apple Home). Une commande toutes les 150 ms au plus pendant le glissement, la valeur finale au relâchement.")
                        .font(.caption).foregroundStyle(.secondary)
                    HStack {
                        Text("Brute (hexa)").foregroundStyle(.secondary)
                        TextField(text: $lumHexa, prompt: Text(verbatim: "A5")) { Text(verbatim: "A5") }
                            .frame(width: 50).textFieldStyle(.roundedBorder)
                        Button { pont.envoyer("lampe lum \(lumHexa.uppercased())") } label: { Text(verbatim: "lampe lum") }
                            .disabled(Int(lumHexa, radix: 16).map { !(0x4C...0xFE).contains($0) } ?? true)
                        Text(verbatim: "4C..FE").font(.caption).foregroundStyle(.secondary)
                    }
                    .controlSize(.small)
                }

                Carte(titre: "Température", icone: "thermometer.sun") {
                    let m = Int(mired)
                    let t = CorrespondanceLuminosite.temp(mired: m)
                    HStack {
                        Text(verbatim: "\(m) mireds").font(.title3.weight(.semibold)).monospacedDigit()
                        Text("· ~\(String(CorrespondanceLuminosite.kelvin(mired: m))) K · brute \(t)")
                            .foregroundStyle(.secondary).monospacedDigit()
                        Spacer()
                    }
                    Slider(value: $mired, in: 153...370, step: 1) {
                        Text(verbatim: "Mireds")
                    } minimumValueLabel: {
                        Label("froid", systemImage: "snowflake").labelStyle(.titleAndIcon).font(.caption)
                    } maximumValueLabel: {
                        Label("chaud", systemImage: "flame").labelStyle(.titleAndIcon).font(.caption)
                    } onEditingChanged: { enCours in
                        glisseMired = enCours
                        if !enCours { pont.curseur("lampe mired \(Int(mired))", cle: "mired", fini: true) }
                    }
                    .tint(.orange)
                    .onChange(of: mired) { _, v in
                        if glisseMired { pont.curseur("lampe mired \(Int(v))", cle: "mired", fini: false) }
                    }
                    HStack {
                        Text("Brute (0..100)").foregroundStyle(.secondary)
                        Stepper(value: $tempBrute, in: 0...100) { Text(verbatim: "\(tempBrute)").monospacedDigit() }
                        Button { pont.envoyer("lampe temp \(tempBrute)") } label: { Text(verbatim: "lampe temp") }
                        Text("Kelvin nominaux, non mesurés").font(.caption).foregroundStyle(.secondary)
                    }
                    .controlSize(.small)
                }

                Carte(titre: "Commandes récentes", icone: "list.bullet.rectangle") {
                    let recents = pont.suivis.filter { $0.origine != .session }.suffix(12).reversed()
                    if recents.isEmpty {
                        Text("Aucune commande envoyée.").foregroundStyle(.secondary)
                    }
                    ForEach(Array(recents)) { s in
                        LigneSuivi(suivi: s)
                    }
                }
            }
            .padding(14)
            .disabled(!pont.peutCommander)
        }
        .onAppear { recopier(consigne) }
        .onChange(of: pont.etat.lampe?.valeur.consigne) { _, c in recopier(c) }
    }

    /// Les curseurs suivent la consigne de la carte, sauf pendant le glissement
    /// et tant que leur valeur finale n'est pas partie (sinon un `etat` qui porte
    /// encore une valeur intermediaire ferait sauter le curseur en arriere).
    private func recopier(_ c: EtatLampe?) {
        guard let c else { return }
        if !glisseNiveau, !enAttente("niveau"), let n = c.niveau { niveau = Double(n) }
        if !glisseMired, !enAttente("mired"), let m = c.mired { mired = Double(m) }
        if let l = c.lum { lumHexa = Format.hexa(l) }
        if let t = c.temp { tempBrute = t }
    }

    private func enAttente(_ cle: String) -> Bool {
        pont.suivis.contains { $0.fusion == cle && ($0.etat == .enFile || $0.etat == .envoyee) }
    }
}

/// Courbe niveau Matter -> luminosite brute, au gamma de la carte.
private struct CourbeGamma: View {
    let correspondance: CorrespondanceLuminosite
    let niveau: Int
    let brutCru: Int?

    private struct P: Identifiable {
        let id: Int
        let brut: Int
    }

    var body: some View {
        let points = stride(from: 0, through: 254, by: 2).map { P(id: $0, brut: correspondance.brut(niveau: $0)) }
        Chart {
            ForEach(points) { p in
                LineMark(x: .value("Niveau", p.id), y: .value("Brut", p.brut))
                    .foregroundStyle(.yellow.gradient)
            }
            RuleMark(x: .value("Niveau", niveau)).foregroundStyle(.secondary.opacity(0.5))
            PointMark(x: .value("Niveau", niveau), y: .value("Brut", correspondance.brut(niveau: niveau)))
                .foregroundStyle(.orange)
                .annotation(position: .topLeading) {
                    Text(Format.hexa(correspondance.brut(niveau: niveau))).font(.caption2.monospaced())
                }
            if let b = brutCru {
                RuleMark(y: .value("Cru", b))
                    .foregroundStyle(.green.opacity(0.6))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 2]))
                    .annotation(position: .bottom, alignment: .trailing) { Text("cru").font(.caption2).foregroundStyle(.green) }
            }
        }
        .chartXScale(domain: 0...254)
        .chartYScale(domain: 0x4C...0xFE)
        .chartYAxis {
            AxisMarks(values: [0x4C, 0x80, 0xA5, 0xD0, 0xFE]) { v in
                AxisGridLine()
                AxisValueLabel { if let i = v.as(Int.self) { Text(Format.hexa(i)) } }
            }
        }
        .chartXAxisLabel("niveau Matter")
    }
}

private struct LigneSuivi: View {
    let suivi: SuiviCommande

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(verbatim: suivi.numero.map { "id=\($0)" } ?? "–").font(.caption.monospaced()).foregroundStyle(.secondary)
                .frame(width: 52, alignment: .leading)
            Text(PolitiqueCommandes.masquerCle(suivi.commande)).font(.callout.monospaced()).lineLimit(1)
            Spacer()
            Pastille(texte: libelle, couleur: couleur)
        }
        .help(aide)
    }

    private var libelle: String {
        if suivi.etat == .terminee, let f = suivi.fin, !f.ok { return f.code.libelle }
        if suivi.etat == .terminee, let f = suivi.fin, f.code == .differe { return tr("différée") }
        return suivi.etat.libelle
    }

    private var couleur: Color {
        switch suivi.etat {
        case .livree: .green
        case .terminee: suivi.fin?.ok == false ? .red : .green
        case .abandonnee, .sansReponse, .perdue: .red
        case .attenteLivraison, .envoyee, .enCours: .orange
        case .annulee, .remplacee, .finPerdue: .secondary
        case .enFile: .blue
        }
    }

    private var aide: String {
        var s = PolitiqueCommandes.masquerCle(suivi.commande)
        if let f = suivi.fin { s += "\n" + Interpretation.reponse(f) }
        if let l = suivi.livraison { s += "\n" + Interpretation.livraison(l) }
        return s
    }
}

// MARK: - Console

private struct ConsoleBrute: View {
    @Environment(Pont.self) private var pont
    @State private var saisie = ""
    @State private var erreur: String?
    @State private var aConfirmer: (ligne: String, raison: String)?
    @State private var historique: [String] = []
    @State private var positionHistorique: Int?
    @State private var logsSysteme = true
    @State private var session = false
    @State private var defilement = true
    @FocusState private var focus: Bool

    var body: some View {
        let lignes = pont.console.elements.filter(visible).suffix(2000)
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text("Console").font(.headline)
                Spacer()
                Toggle("Logs système", isOn: $logsSysteme)
                Toggle("Session (ping, json 1)", isOn: $session)
                Toggle("Défilement", isOn: $defilement)
                Button("Vider") { pont.viderConsole() }
            }
            .toggleStyle(.checkbox)
            .controlSize(.small)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            Divider()
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(lignes) { l in
                            LigneConsoleVue(ligne: l).id(l.id)
                        }
                    }
                    .padding(8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .background(Color(nsColor: .textBackgroundColor))
                .onChange(of: pont.console.elements.last?.id) { _, id in
                    if defilement, let id { proxy.scrollTo(id, anchor: .bottom) }
                }
            }
            Divider()
            saisieVue
        }
        .alert("Confirmer la commande", isPresented: Binding(get: { aConfirmer != nil }, set: { if !$0 { aConfirmer = nil } }),
               presenting: aConfirmer) { c in
            Button(tr("Envoyer « \(c.ligne) »"), role: .destructive) {
                traiter(pont.console(c.ligne, confirme: true), ligne: c.ligne)
            }
            Button("Annuler", role: .cancel) {}
        } message: { c in
            Text(c.raison)
        }
    }

    private var saisieVue: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(pont.consoleAvecId ? "id=…" : tr("brut")).font(.caption.monospaced()).foregroundStyle(.secondary)
                TextField("commande de la CLI (ex. lampe stats, help)", text: $saisie)
                    .textFieldStyle(.roundedBorder)
                    .font(.body.monospaced())
                    .focused($focus)
                    .onSubmit(soumettre)
                    .onKeyPress(.upArrow) { naviguer(-1) }
                    .onKeyPress(.downArrow) { naviguer(1) }
                Button("Envoyer", action: soumettre)
                    .keyboardShortcut(.defaultAction)
                    .disabled(saisie.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            HStack {
                if let erreur {
                    Text(erreur).foregroundStyle(.red)
                } else if !pont.consoleAvecId, pont.phase != .ferme {
                    Text("Console seule (\(pont.phase.libelle)) : lignes envoyées sans id, sans corrélation.")
                        .foregroundStyle(.orange)
                } else {
                    Text("Chaque ligne part avec un id : le texte reçu entre reponse début et fin lui est rattaché.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                // Prefixe "id=<n> " : 13 octets au plus (n <= 999999999).
                let octets = saisie.utf8.count + (pont.consoleAvecId ? 13 : 0)
                Text("\(octets) / \(LigneCommande.octetsMax) octets")
                    .foregroundStyle(octets > LigneCommande.octetsMax ? .red : .secondary)
                    .monospacedDigit()
            }
            .font(.caption)
        }
        .padding(10)
    }

    private func visible(_ l: LigneConsole) -> Bool {
        switch l.genre {
        case .texte(let c): return logsSysteme || !c.estLogSysteme
        case .fragment: return logsSysteme
        case .envoi(let o): return session || o != .session
        case .retour(_, let s): return session || !s
        default: return true
        }
    }

    private func soumettre() {
        let ligne = saisie.trimmingCharacters(in: .whitespaces)
        guard !ligne.isEmpty else { return }
        traiter(pont.console(ligne), ligne: ligne)
    }

    private func traiter(_ r: Pont.ResultatConsole, ligne: String) {
        switch r {
        case .envoyee:
            erreur = nil
            // Jamais de cle (json cle nouvelle <64 hexa>) dans l'historique de saisie (10.4).
            let cle = LigneCommande.mots(ligne).prefix(2) == ["json", "cle"]
            if !cle, historique.last != ligne { historique.append(ligne) }
            positionHistorique = nil
            saisie = ""
        case .confirmation(let raison):
            aConfirmer = (ligne, raison)
        case .refusee(let raison):
            erreur = raison
        }
    }

    private func naviguer(_ sens: Int) -> KeyPress.Result {
        guard !historique.isEmpty else { return .ignored }
        let p = (positionHistorique ?? historique.count) + sens
        if p >= historique.count {
            positionHistorique = nil
            saisie = ""
        } else {
            positionHistorique = max(0, p)
            saisie = historique[max(0, p)]
        }
        return .handled
    }
}

private struct LigneConsoleVue: View {
    let ligne: LigneConsole

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(Format.heure(ligne.date))
                .foregroundStyle(.tertiary)
            Text(ligne.texte)
                .foregroundStyle(couleur)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .font(.system(size: 11.5, design: .monospaced))
        .padding(.leading, ligne.numero != nil && estTexte ? 14 : 0)
    }

    private var estTexte: Bool {
        if case .texte = ligne.genre { return true }
        return false
    }

    private var couleur: Color {
        switch ligne.genre {
        case .envoi(let o): o == .session ? .secondary : .accentColor
        case .texte(let c):
            switch c {
            case .logIDF, .logArduino: .secondary
            case .annonce: .purple
            case .demarrage: .orange
            default: .primary
            }
        case .fragment: .gray
        case .retour(let ok, let s): s ? .secondary : (ok ? .green : .red)
        case .log: .purple
        case .note(let grave): grave ? .red : .teal
        }
    }
}
