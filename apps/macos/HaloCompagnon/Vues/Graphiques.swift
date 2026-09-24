import Charts
import HaloProtocole
import SwiftUI

/// Ecran "Graphiques" : les courbes de la section 8, calculees par differences
/// de blocs `compteurs` successifs (HaloProtocole.Courbes).
struct Graphiques: View {
    @Environment(Pont.self) private var pont
    @State private var fenetre: Double = 10
    @State private var duree: Double = 15 * 60
    @State private var confirmerRaz = false

    private struct Point: Identifiable {
        let id: String
        let debut: Date
        let fin: Date
        let serie: String
        let segment: Int
        let valeur: Double
    }

    var body: some View {
        TimelineView(.periodic(from: .now, by: 2)) { contexte in
            // Les lignes sont datees par `ms` (ancre du hello), pas par l'heure d'arrivee :
            // l'axe finit au plus recent des deux.
            let fin = [contexte.date, pont.pilote.elements.last?.date, pont.radio.elements.last?.date]
                .compactMap { $0 }.max() ?? contexte.date
            let debut = duree > 0 ? fin.addingTimeInterval(-duree) : .distantPast
            contenu(debut: debut, fin: fin)
        }
        .confirmationDialog("Remettre les statistiques à zéro ?", isPresented: $confirmerRaz) {
            Button("lampe stats raz", role: .destructive) { pont.envoyer("lampe stats raz") }
        } message: {
            Text("Remet à zéro les blocs pilote et radio de la carte (relances comprises). Les courbes ouvrent un nouveau segment.")
        }
    }

    @ViewBuilder
    private func contenu(debut: Date, fin: Date) -> some View {
        let dp = Courbes.differences(pont.pilote.elements, fenetre: fenetre).filter { $0.fin >= debut }
        let dr = Courbes.differences(pont.radio.elements, fenetre: fenetre).filter { $0.fin >= debut }
        let seuils = pont.etat.config?.valeur.seuils
        let marques = pont.marqueurs.elements.filter { $0.date >= debut }
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                reglages
                if pont.pilote.elements.isEmpty {
                    ContentUnavailableView("Pas encore de compteurs", systemImage: "chart.xyaxis.line",
                                           description: Text("Les courbes se calculent sur les blocs compteurs reçus chaque seconde."))
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 460), spacing: 16, alignment: .top)], spacing: 16) {
                    Carte(titre: "Taux de perte TX", icone: "arrow.up.forward.circle") {
                        graphePerte(dp, marques: marques.filter { $0.genre == .abandon }, debut: debut, fin: fin)
                        legende("(Δ max_rt + Δ délais + Δ fifo) / Δ paquets ; pointillé : 1 − Δ accusés / Δ paquets. "
                                + "Traits rouges : livraisons abandonnées.")
                    }
                    Carte(titre: "Consignes abandonnées", icone: "xmark.octagon") {
                        grapheBarres(points(dp, "abandons") { $0[.abandons].map(Double.init) }, debut: debut, fin: fin,
                                     couleur: .red, unite: "par fenêtre")
                        legende("Δ tranches.abandons par fenêtre de \(Int(fenetre)) s.")
                    }
                    Carte(titre: "CRC faux", icone: "exclamationmark.bubble") {
                        grapheCrc(dp, seuils: seuils, debut: debut, fin: fin)
                        legende("Δ rx.crc_faux par minute ; trait : seuil du déluge "
                                + "(\(seuils?.delugeTrames ?? 100) trames sur \(Format.ms(seuils?.fenetreMs ?? 10000)), "
                                + "ramené à la minute).")
                        grapheLignes(points(dp, "part CRC faux") { Courbes.partCrcFaux($0).map { $0 * 100 } },
                                     debut: debut, fin: fin, unite: "%",
                                     seuil: seuils?.delugePct.map(Double.init), hauteur: 110)
                        legende("Δ crc_faux / Δ trames (%) ; trait : deluge_pct (\(seuils?.delugePct ?? 90) %).")
                    }
                    Carte(titre: "Refus en réception", icone: "ear.trianglebadge.exclamationmark") {
                        grapheRefus(dp: dp, dr: dr, seuils: seuils, debut: debut, fin: fin)
                        legende("Δ radio.rearm_hors_rx (surdité), Δ tx.fifo, Δ garde.refus par fenêtre ; "
                                + "trait : sourd_hors_rx (\(seuils?.sourdHorsRx ?? 1000) sur 10 s) ramené à la fenêtre.")
                    }
                    Carte(titre: "Relances du module", icone: "arrow.triangle.2.circlepath") {
                        grapheRelances(debut: debut, fin: fin, marques: marques)
                        legende("relances.* cumulés, empilés par cause ; traits : événements relance et module.")
                    }
                    Carte(titre: "Santé Matter", icone: "point.3.connected.trianglepath.dotted") {
                        grapheMatter(debut: debut, fin: fin, marques: marques.filter { $0.genre == .role })
                        legende("Abonnements actifs et RSSI du parent Thread (reseau, toutes les 5 s) ; "
                                + "traits violets : changements de rôle.")
                    }
                }
            }
            .padding(16)
        }
    }

    private var reglages: some View {
        HStack(spacing: 16) {
            Picker("Fenêtre", selection: $fenetre) {
                Text("10 s").tag(10.0)
                Text("1 min").tag(60.0)
            }
            .pickerStyle(.segmented)
            .fixedSize()
            Picker("Durée", selection: $duree) {
                Text("5 min").tag(300.0)
                Text("15 min").tag(900.0)
                Text("1 h").tag(3600.0)
                Text("Tout").tag(0.0)
            }
            .pickerStyle(.segmented)
            .fixedSize()
            Spacer()
            Text("Une différence négative, un raz ou un redémarrage ouvre un nouveau segment.")
                .font(.caption)
                .foregroundStyle(.secondary)
            Menu("Remise à zéro") {
                Button("Statistiques de la carte (lampe stats raz)…") { confirmerRaz = true }
                    .disabled(!pont.peutCommander)
                Button("Courbes de l'app seulement") { pont.viderCourbes() }
            }
            .fixedSize()
        }
    }

    private func legende(_ texte: String) -> some View {
        Text(texte).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
    }

    private func points(_ d: [Difference], _ serie: String, _ f: (Difference) -> Double?) -> [Point] {
        d.enumerated().compactMap { i, x in
            f(x).map { Point(id: "\(serie)-\(i)", debut: x.debut, fin: x.fin, serie: serie, segment: x.segment, valeur: $0) }
        }
    }

    // MARK: - Graphes

    private func graphePerte(_ d: [Difference], marques: [Marqueur], debut: Date, fin: Date) -> some View {
        let perte = points(d, "perte") { Courbes.tauxPerte($0).map { $0 * 100 } }
        let sansAccuse = points(d, "sans accusé") { Courbes.tauxSansAccuse($0).map { $0 * 100 } }
        return Chart {
            ForEach(perte) { p in
                LineMark(x: .value("Heure", p.fin), y: .value("%", p.valeur), series: .value("s", "perte-\(p.segment)"))
                    .foregroundStyle(.red)
                PointMark(x: .value("Heure", p.fin), y: .value("%", p.valeur))
                    .foregroundStyle(.red)
                    .symbolSize(12)
            }
            ForEach(sansAccuse) { p in
                LineMark(x: .value("Heure", p.fin), y: .value("%", p.valeur), series: .value("s", "sa-\(p.segment)"))
                    .foregroundStyle(.orange)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
            }
            ForEach(marques) { m in
                RuleMark(x: .value("Heure", m.date))
                    .foregroundStyle(.red.opacity(0.4))
            }
        }
        .chartXScale(domain: echelle(debut, fin))
        .chartPlotStyle { $0.clipped() }
        .chartYScale(domain: 0...100)
        .chartYAxisLabel("%")
        .frame(height: 170)
    }

    private func grapheBarres(_ p: [Point], debut: Date, fin: Date, couleur: Color, unite: String) -> some View {
        Chart(p.filter { $0.valeur > 0 }) { x in
            BarMark(xStart: .value("Début", x.debut), xEnd: .value("Fin", x.fin), y: .value(unite, x.valeur))
                .foregroundStyle(couleur)
        }
        .chartXScale(domain: echelle(debut, fin))
        .chartPlotStyle { $0.clipped() }
        .chartYAxisLabel(unite)
        .frame(height: 130)
    }

    private func grapheCrc(_ d: [Difference], seuils: ConfigCarte.Seuils?, debut: Date, fin: Date) -> some View {
        let p = points(d, "crc") { Courbes.parMinute($0, .crcFaux) }
        let seuil = Courbes.seuilDelugeParMinute(trames: seuils?.delugeTrames ?? 100, fenetreMs: seuils?.fenetreMs ?? 10000)
        return Chart {
            ForEach(p.filter { $0.valeur > 0 }) { x in
                BarMark(xStart: .value("Début", x.debut), xEnd: .value("Fin", x.fin), y: .value("par min", x.valeur))
                    .foregroundStyle(.gray)
            }
            if let seuil, (p.map(\.valeur).max() ?? 0) > seuil / 4 {
                RuleMark(y: .value("Seuil", seuil))
                    .foregroundStyle(.red)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [5, 3]))
            }
        }
        .chartXScale(domain: echelle(debut, fin))
        .chartPlotStyle { $0.clipped() }
        .chartYAxisLabel("par min")
        .frame(height: 130)
    }

    private func grapheLignes(_ p: [Point], debut: Date, fin: Date, unite: String, seuil: Double?, hauteur: CGFloat) -> some View {
        Chart {
            ForEach(p) { x in
                LineMark(x: .value("Heure", x.fin), y: .value(unite, x.valeur), series: .value("s", "\(x.serie)-\(x.segment)"))
                    .foregroundStyle(.gray)
            }
            if let seuil {
                RuleMark(y: .value("Seuil", seuil))
                    .foregroundStyle(.red)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [5, 3]))
            }
        }
        .chartXScale(domain: echelle(debut, fin))
        .chartPlotStyle { $0.clipped() }
        .chartYScale(domain: 0...100)
        .chartYAxisLabel(unite)
        .frame(height: hauteur)
    }

    private func grapheRefus(dp: [Difference], dr: [Difference], seuils: ConfigCarte.Seuils?, debut: Date, fin: Date) -> some View {
        let horsRx = points(dr, "hors RX") { $0[.rearmHorsRx].map(Double.init) }
        let garde = points(dr, "garde") { $0[.gardeRefus].map(Double.init) }
        let fifo = points(dp, "FIFO") { $0[.fifo].map(Double.init) }
        let seuil = Double(seuils?.sourdHorsRx ?? 1000) * fenetre / 10
        let maxi = (horsRx + garde + fifo).map(\.valeur).max() ?? 0
        return Chart {
            ForEach(horsRx + garde + fifo) { x in
                LineMark(x: .value("Heure", x.fin), y: .value("Δ", x.valeur), series: .value("s", "\(x.serie)-\(x.segment)"))
                    .foregroundStyle(by: .value("Compteur", x.serie))
            }
            if maxi > seuil / 4 {
                RuleMark(y: .value("Seuil", seuil))
                    .foregroundStyle(.red)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [5, 3]))
                    .annotation(position: .top, alignment: .trailing) { Text("seuil de surdité").font(.caption2).foregroundStyle(.red) }
            }
        }
        .chartForegroundStyleScale(["hors RX": Color.orange, "garde": Color.purple, "FIFO": Color.blue])
        .chartXScale(domain: echelle(debut, fin))
        .chartPlotStyle { $0.clipped() }
        .chartYScale(domain: 0...max(maxi * 1.15, maxi > seuil / 4 ? seuil * 1.15 : 10))
        .chartYAxisLabel("par fenêtre")
        .frame(height: 170)
    }

    private func grapheRelances(debut: Date, fin: Date, marques: [Marqueur]) -> some View {
        let causes: [(Grandeur, String)] = [(.relancesVerif, "vérif"), (.relancesDelais, "délais"),
                                           (.relancesBruit, "bruit"), (.relancesSourde, "sourde")]
        let echantillons = pont.radio.elements
        var pts: [Point] = []
        for (g, nom) in causes {
            for (i, c) in Courbes.cumul(echantillons, g).enumerated() where c.date >= debut {
                pts.append(Point(id: "\(nom)-\(i)", debut: c.date, fin: c.date, serie: nom, segment: c.segment,
                                 valeur: Double(c.valeur)))
            }
        }
        let evenements = marques.filter { $0.genre == .relance || $0.genre == .module }
        return Chart {
            ForEach(pts) { p in
                AreaMark(x: .value("Heure", p.fin), y: .value("Relances", p.valeur), stacking: .standard)
                    .foregroundStyle(by: .value("Cause", p.serie))
                    .interpolationMethod(.stepEnd)
            }
            ForEach(evenements) { m in
                RuleMark(x: .value("Heure", m.date))
                    .foregroundStyle(m.genre == .module ? .red : .orange)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 2]))
                    .annotation(position: .top, alignment: .leading) {
                        Text(m.texte).font(.caption2).foregroundStyle(m.genre == .module ? .red : .orange)
                    }
            }
        }
        .chartForegroundStyleScale(["vérif": Color.gray, "délais": Color.red, "bruit": Color.yellow, "sourde": Color.orange])
        .chartXScale(domain: echelle(debut, fin))
        .chartPlotStyle { $0.clipped() }
        .chartYAxisLabel("cumul")
        .frame(height: 170)
    }

    private func grapheMatter(debut: Date, fin: Date, marques: [Marqueur]) -> some View {
        let abonnes = pont.abonnes.elements.filter { $0.date >= debut }
        let rssi = pont.rssi.elements.filter { $0.date >= debut }
        return VStack(alignment: .leading, spacing: 6) {
            Chart {
                ForEach(abonnes) { p in
                    if let v = p.valeur {
                        LineMark(x: .value("Heure", p.date), y: .value("Actifs", v))
                            .interpolationMethod(.stepEnd)
                            .foregroundStyle(.blue)
                    }
                }
                ForEach(marques) { m in
                    RuleMark(x: .value("Heure", m.date)).foregroundStyle(.purple.opacity(0.5))
                }
            }
            .chartXScale(domain: echelle(debut, fin))
            .chartPlotStyle { $0.clipped() }
            .chartYAxisLabel("abonnés")
            .frame(height: 90)
            Chart {
                ForEach(rssi) { p in
                    if let v = p.valeur {
                        LineMark(x: .value("Heure", p.date), y: .value("dBm", v))
                            .foregroundStyle(.teal)
                        PointMark(x: .value("Heure", p.date), y: .value("dBm", v))
                            .foregroundStyle(.teal)
                            .symbolSize(10)
                    }
                }
                ForEach(marques) { m in
                    RuleMark(x: .value("Heure", m.date))
                        .foregroundStyle(.purple.opacity(0.5))
                        .annotation(position: .top, alignment: .leading) { Text(m.texte).font(.caption2).foregroundStyle(.purple) }
                }
            }
            .chartXScale(domain: echelle(debut, fin))
            .chartPlotStyle { $0.clipped() }
            .chartYScale(domain: -100 ... -20)
            .chartYAxisLabel("RSSI parent (dBm)")
            .frame(height: 110)
        }
    }

    /// Axe du temps : la periode choisie, ramenee au premier echantillon s'il est plus recent.
    private func echelle(_ debut: Date, _ fin: Date) -> ClosedRange<Date> {
        let premier = pont.pilote.elements.first?.date ?? fin.addingTimeInterval(-60)
        return min(max(debut, premier), fin.addingTimeInterval(-30))...fin
    }
}
