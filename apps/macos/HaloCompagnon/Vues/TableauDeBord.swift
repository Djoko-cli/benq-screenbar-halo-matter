import AppKit
import CoreImage.CIFilterBuiltins
import HaloProtocole
import SwiftUI

struct TableauDeBord: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        if pont.etat.lampe == nil && pont.etat.helloBase == nil {
            ContentUnavailableView {
                Label("Aucun état reçu", systemImage: "antenna.radiowaves.left.and.right.slash")
            } description: {
                Text("Choisir un port USB (VID 303A) ou le mode démo dans la barre latérale.")
            } actions: {
                Button("Lancer la démo") { pont.connecter(.demo) }
            }
        } else {
            ScrollView {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 340), spacing: 14, alignment: .top)],
                          alignment: .leading, spacing: 14) {
                    CarteLampe()
                    CarteModule()
                    CarteLiaison()
                    CarteVoyant()
                    if avecMatter {
                        CarteThread()
                        CarteAbonnements()
                    }
                    CarteSanteLien()
                    CarteVersions()
                    CarteSysteme()
                }
                .padding(16)
            }
            .defaultScrollAnchor(.top)
        }
    }

    /// Cartes Thread et Matter : seulement si le build compile le pont Matter
    /// (`caps`, 5.1) ; avant l'identite, d'apres les blocs deja recus.
    private var avecMatter: Bool {
        let caps = pont.etat.capacites
        if !caps.isEmpty { return caps.contains("matter") }
        return pont.etat.thread != nil || pont.etat.abonnements != nil || pont.etat.sante?.valeur.matter != nil
    }
}

// MARK: - Lampe

private struct CarteLampe: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let l = pont.etat.lampe?.valeur
        let aLivrer = Set(l?.aLivrer ?? [])
        Carte(titre: "Lampe : consigne et état cru", icone: "lightbulb.2") {
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
                GridRow {
                    Text("")
                    Text("Consigne").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                    Text("Cru").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                }
                ligne("Marche", l?.consigne.marche.map { $0 ? "allumée" : "éteinte" }, l?.cru.marche.map { $0 ? "allumée" : "éteinte" },
                      aLivrer.contains(.marche))
                ligne("Lampes", l?.consigne.lampes?.libelle, l?.cru.lampes?.libelle, aLivrer.contains(.marche))
                ligne("Luminosité", lum(l?.consigne), lum(l?.cru), aLivrer.contains(.lum))
                ligne("Température", temp(l?.consigne), temp(l?.cru), aLivrer.contains(.temp))
            }
            .font(.callout)
            Divider()
            HStack(spacing: 6) {
                Text("À livrer :").foregroundStyle(.secondary)
                if aLivrer.isEmpty { Pastille(texte: "rien", couleur: .green) }
                ForEach(l?.aLivrer ?? [], id: \.self) { Pastille(texte: $0.libelle, couleur: .orange) }
                Spacer()
                if let v = l?.version { Text("version \(v)").foregroundStyle(.secondary).monospacedDigit() }
            }
            .font(.callout)
            LigneInfo("Pilote", l?.phase.map { p in
                var s = p.libelle
                if p == .reprise, let r = l?.repriseMs { s += " (dans \(Format.ms(r)))" }
                return s
            }, couleur: l?.phase == .reprise ? .orange : nil)
            LigneInfo("Tours ratés", l?.echecs.map { e in
                "\(e) / \(pont.etat.config?.valeur.reglages?.reprises.map(String.init) ?? "?")"
            })
            LigneInfo("Mémoire de sélection", l?.memoire?.libelle)
            LigneInfo("Bouton A", l.map { "dernier n° \($0.dernierA ?? 0), \($0.aEntendus ?? 0) entendus" })
            LigneInfo("Écoute de fond", Format.oui(l?.ecoute))
            if let t = pont.etat.tranches?.valeur.tranches, !t.isEmpty {
                Divider()
                Text("Tranches actives").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                ForEach(Array(t.enumerated()), id: \.offset) { _, tr in
                    LigneInfo(tr.tranche?.libelle ?? "?",
                              "\(tr.charge ?? "") · essai \(tr.essais ?? 0) · \(tr.accuses ?? 0)/\(tr.paquets ?? 0) accusés",
                              mono: true)
                }
            }
        }
    }

    @ViewBuilder
    private func ligne(_ nom: String, _ c: String?, _ r: String?, _ enAttente: Bool) -> some View {
        GridRow {
            Text(nom).foregroundStyle(.secondary)
            Text(c ?? "–").fontWeight(enAttente ? .semibold : .regular).foregroundStyle(enAttente ? .orange : .primary)
            Text(r ?? "–").foregroundStyle(c != r ? .orange : .primary)
        }
    }

    private func lum(_ e: EtatLampe?) -> String? {
        guard let e, let lum = e.lum else { return nil }
        let niveau = e.niveau ?? pont.etat.correspondance.niveau(brut: lum)
        return "\(Format.hexa(lum)) · niveau \(niveau) (\(CorrespondanceLuminosite.pourcent(niveau: niveau)) %)"
    }

    private func temp(_ e: EtatLampe?) -> String? {
        guard let e, let t = e.temp else { return nil }
        let m = e.mired ?? CorrespondanceLuminosite.mired(temp: t)
        return "\(t) · \(m) mireds (~\(CorrespondanceLuminosite.kelvin(mired: m)) K)"
    }
}

// MARK: - Liaison

private struct CarteLiaison: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let l = pont.etat.lampe?.valeur
        let lien = l?.lien ?? .inconnu
        let couleur: Color = lien == .ok ? .green : (lien == .perdu ? .red : .secondary)
        Carte(titre: "Liaison avec la lampe", icone: "wave.3.right", accent: lien == .perdu ? .red : .secondary) {
            HStack {
                Pastille(texte: lien.libelle, couleur: couleur)
                Spacer()
                if let a = l?.accuseMs {
                    Text("dernier accusé il y a \(Format.duree(secondes: a / 1000))").foregroundStyle(.secondary)
                } else {
                    Text("aucun accusé depuis le démarrage").foregroundStyle(.secondary)
                }
            }
            .font(.callout)
            LigneInfo("Consignes livrées", l?.livrees.map(String.init))
            LigneInfo("Consignes abandonnées", l?.abandons.map(String.init), couleur: (l?.abandons ?? 0) > 0 ? .orange : nil)
            if let d = pont.etat.derniereLivraison {
                Divider()
                Text("Dernière livraison · \(Format.heure(d.date))").font(.caption).foregroundStyle(.secondary)
                Text(Interpretation.livraison(d.valeur))
                    .font(.callout)
                    .foregroundStyle(d.valeur.issue == .abandon ? .red : .primary)
            }
            if let p = pont.etat.pilote?.valeur {
                Divider()
                LigneInfo("Paquets émis / accusés", "\(p.tx?.paquets ?? 0) / \(p.tx?.accuses ?? 0)", mono: true)
                LigneInfo("MAX_RT · délais · FIFO", "\(p.tx?.maxRt ?? 0) · \(p.tx?.delais ?? 0) · \(p.tx?.fifo ?? 0)", mono: true)
                LigneInfo("Trames entendues (CRC faux)", "\(p.rx?.trames ?? 0) (\(p.rx?.crcFaux ?? 0))", mono: true)
            }
        }
    }
}

// MARK: - Module

private struct CarteModule: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let s = pont.etat.sante?.valeur
        let seuils = pont.etat.config?.valeur.seuils
        let panne = s?.surveil?.panne == true
        Carte(titre: "Module radio BM5602", icone: "antenna.radiowaves.left.and.right",
              accent: panne ? .red : (s?.surveil?.defaut == true ? .orange : .secondary)) {
            if panne {
                HStack {
                    Image(systemName: "exclamationmark.triangle.fill")
                    Text("EN PANNE").font(.title3.weight(.heavy))
                    Spacer()
                    if let a = s?.surveil?.attenteMs { Text("essai dans \(Format.duree(secondes: a / 1000))") }
                }
                .foregroundStyle(.white)
                .padding(8)
                .background(.red, in: RoundedRectangle(cornerRadius: 6))
            }
            HStack(spacing: 6) {
                Pastille(texte: s?.radio?.presente == false ? "absent" : "présent",
                         couleur: s?.radio?.presente == false ? .red : .green)
                if s?.radio?.perdue == true { Pastille(texte: "perdu (L3)", couleur: .red) }
                Pastille(texte: s?.radio?.mode?.libelle ?? "–")
                Pastille(texte: s?.radio?.configuree == true ? "configuré" : "non configuré",
                         couleur: s?.radio?.configuree == true ? .green : .orange)
                if let sy = s?.surveil?.symptome { Pastille(texte: sy.libelle, couleur: .orange) }
            }
            LigneInfo("Quartz · calibration", "\(Format.oui(s?.radio?.quartz)) · \(Format.oui(s?.radio?.calib))")
            JaugeSeuil(libelle: "Délais TX de suite", valeur: s?.surveil?.delaisSuite, seuil: seuils?.delaisSuite)
            JaugeSeuil(libelle: "Réarmements hors RX (10 s)", valeur: s?.surveil?.horsRx10s, seuil: seuils?.sourdHorsRx)
            JaugeSeuil(libelle: "Relances sans guérison", valeur: s?.surveil?.sansGuerison, seuil: seuils?.sansGuerison)
            LigneInfo("Fenêtre d'écoute", "\(s?.surveil?.fenTrames ?? 0) trames, \(s?.surveil?.fenCrcFaux ?? 0) CRC faux")
            LigneInfo("Relances automatiques", s?.surveil?.relances.map(String.init))
            if let d = s?.surveil?.derniere {
                LigneInfo("Dernière relance", "\(d.cause?.libelle ?? "?"), il y a \(Format.duree(secondes: d.ilYaS))")
            }
            if let r = pont.etat.derniereRelance {
                Text(Interpretation.relance(r.valeur)).font(.caption).foregroundStyle(.secondary)
            }
            if let m = pont.etat.dernierModule {
                Text("\(Format.heure(m.date)) · \(Interpretation.module(m.valeur))")
                    .font(.caption)
                    .foregroundStyle(m.valeur.etat == .panne ? .red : .secondary)
            }
        }
    }
}

// MARK: - Voyant

private struct CarteVoyant: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let motif = pont.etat.motifLed
        Carte(titre: "Voyant", icone: "light.beacon.max") {
            HStack(spacing: 14) {
                VoyantLed(motif: motif, depuis: pont.etat.motifLedDepuis, taille: 34)
                VStack(alignment: .leading) {
                    Text((motif?.libelle ?? "inconnu").avecMajuscule).font(.title3.weight(.semibold))
                    Text((motif?.description ?? "motif pas encore reçu").avecMajuscule).foregroundStyle(.secondary)
                    if pont.etat.ledTest { Pastille(texte: "Test du voyant en cours", couleur: .purple) }
                }
            }
            if pont.etat.capacites.contains("led") {
                HStack {
                    Button("Tester le voyant") { pont.envoyer("led test") }
                    Button("Arrêter") { pont.envoyer("led stop") }
                }
                .controlSize(.small)
                .disabled(!pont.peutCommander)
            } else if !pont.etat.capacites.isEmpty {
                Text("Pas de voyant d'état dans ce build (capacité led absente).")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Thread et Matter

private struct CarteThread: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let r = pont.etat.thread?.valeur
        let m = pont.etat.sante?.valeur.matter
        Carte(titre: "Thread et Matter", icone: "point.3.connected.trianglepath.dotted") {
            HStack(spacing: 6) {
                Pastille(texte: (r?.matter?.enService ?? m?.enService) == true ? "en service" : "pas en service",
                         couleur: (r?.matter?.enService ?? m?.enService) == true ? .green : .orange)
                Pastille(texte: (r?.matter?.connecte ?? m?.connecte) == true ? "connecté" : "déconnecté",
                         couleur: (r?.matter?.connecte ?? m?.connecte) == true ? .green : .red)
                if m?.identify == true { Pastille(texte: "identify", couleur: .purple) }
                if let role = r?.thread?.role {
                    Pastille(texte: role, couleur: role == "detached" || role == "disabled" ? .red : .blue)
                }
            }
            LigneInfo("Canal", r?.thread.map { "\($0.canal.map(String.init) ?? "?") (\($0.mhz.map(String.init) ?? "?") MHz)" })
            LigneInfo("PAN · puissance", r?.thread.map { "\($0.pan ?? "?") · \($0.txDbm.map { "\($0) dBm" } ?? "?")" })
            LigneInfo("RSSI du parent", r?.thread?.parentRssi.map { "\($0) dBm" })
            LigneInfo("Mode", r?.thread.map { "\($0.mode ?? "?") (démarrage \($0.typeBoot ?? "?"), suivant \($0.typeSuivant ?? "?"))" })
            LigneInfo("Changements de rôle", r?.thread?.roles.map(String.init))
            LigneInfo("SRP", r?.thread?.srp.map { "\($0.hote ?? "?") · \($0.enregistres ?? 0)/\($0.services ?? 0) services" })
            LigneInfo("Fabriques", r?.matter?.fabriques.map(String.init))
            if let f = r?.fraisMs, f > 2000 { LigneInfo("Âge des valeurs", Format.ms(f), couleur: .orange) }
            if let code = r?.matter?.codeManuel {
                Divider()
                Text("Mise en service").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                HStack(alignment: .top) {
                    VStack(alignment: .leading) {
                        Text("Code manuel").foregroundStyle(.secondary)
                        Text(code).font(.title3.monospaced()).textSelection(.enabled)
                        if let qr = r?.matter?.qr { Text(qr).font(.caption.monospaced()).textSelection(.enabled) }
                    }
                    Spacer()
                    if let qr = r?.matter?.qr, let image = CodeQR.image(qr) {
                        Image(nsImage: image).interpolation(.none).resizable().frame(width: 110, height: 110)
                    }
                }
            }
        }
    }
}

enum CodeQR {
    static func image(_ texte: String) -> NSImage? {
        let filtre = CIFilter.qrCodeGenerator()
        filtre.message = Data(texte.utf8)
        filtre.correctionLevel = "M"
        guard let sortie = filtre.outputImage?.transformed(by: CGAffineTransform(scaleX: 8, y: 8)) else { return nil }
        let rep = NSCIImageRep(ciImage: sortie)
        let image = NSImage(size: rep.size)
        image.addRepresentation(rep)
        return image
    }
}

// MARK: - Abonnements

private struct CarteAbonnements: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let a = pont.etat.abonnements?.valeur
        Carte(titre: "Abonnements Matter", icone: "bell.badge") {
            HStack {
                Text("\(a?.abonnements?.actifs ?? 0)").font(.largeTitle.weight(.semibold)).monospacedDigit()
                Text("actif(s)").foregroundStyle(.secondary)
                Spacer()
                if a?.reprise?.enCours == .booleen(true) { Pastille(texte: "reprise en cours", couleur: .orange) }
            }
            LigneInfo("Lectures", a?.abonnements?.lectures.map(String.init))
            LigneInfo("Sauvés (NVS)", a?.abonnements?.sauves.map(String.init))
            LigneInfo("Demandés · neufs · terminés",
                      a?.abonnements.map { "\($0.demandes ?? 0) · \($0.neufs ?? 0) · \($0.termines ?? 0)" })
            LigneInfo("Repris (pont · pile)", a?.abonnements.map { "\($0.reprisPont ?? 0) · \($0.reprisPile ?? 0)" })
            LigneInfo("Plafond", a?.abonnements.map { "\($0.plafondS ?? 0) s (\($0.plafonnes ?? 0) plafonnés)" })
            LigneInfo("Reprise automatique", Format.oui(a?.abonnements?.repriseAuto))
            LigneInfo("Passages de reprise", a?.reprise.map { "\($0.passages ?? 0) (\($0.auto ?? 0) auto, \($0.echecs ?? 0) échecs)" })
        }
    }
}

// MARK: - Sante du lien

private struct CarteSanteLien: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let r = pont.reception
        let s = pont.statistiques
        let sys = pont.etat.sante?.valeur.sys
        let probleme = r.lignesAbimees + r.fragments + s.pertes > 0
        Carte(titre: "Santé du lien série", icone: "waveform.path.ecg") {
            HStack {
                Pastille(texte: pont.phase.libelle, couleur: pont.phase.couleur)
                Spacer()
                if let d = pont.derniereReception {
                    TimelineView(.periodic(from: .now, by: 1)) { ctx in
                        Text("reçu il y a \(Format.duree(secondes: Int(max(0, ctx.date.timeIntervalSince(d)))))")
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .font(.callout)
            LigneInfo("Transport", pont.nomTransport.isEmpty ? nil : pont.nomTransport)
            LigneInfo("Lignes machine · texte", "\(r.lignesMachine) · \(r.lignesTexte)")
            LigneInfo("Abîmées · fragments", "\(r.lignesAbimees) · \(r.fragments)", couleur: probleme ? .orange : nil)
            LigneInfo("Pertes (trous de n) · reculs", "\(s.pertes) · \(s.reculs)", couleur: s.pertes > 0 ? .orange : nil)
            LigneInfo("Débordements · versions inconnues", "\(r.debordements) · \(r.versionsInconnues)")
            LigneInfo("Types inconnus · invalides", "\(r.typesInconnus) · \(r.messagesInvalides)")
            LigneInfo("Connexions · redémarrages", "\(s.connexions) · \(s.redemarrages)")
            LigneInfo("Silences · réouvertures · sans réponse", "\(s.silences) · \(s.reouvertures) · \(s.sansReponse)")
            Divider()
            Text("Côté carte").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            LigneInfo("JSON perdus · trop longs", sys.map { "\($0.jsonPerdus ?? 0) · \($0.jsonTropLongs ?? 0)" })
            LigneInfo("Lignes de l'hôte refusées", sys?.rejets.map(String.init))
            LigneInfo("Plus long tour de loop()", sys?.boucleMaxMs.map { "\($0) ms" })
            if let dernier = pont.rejets.elements.last {
                Divider()
                Text("Dernière ligne rejetée · \(Format.heure(dernier.date))").font(.caption).foregroundStyle(.secondary)
                Text("\(dernier.raison)\n\(dernier.brut.prefix(160))")
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(4)
            }
        }
    }
}

// MARK: - Versions

private struct CarteVersions: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let h = pont.etat.helloBase?.valeur
        let i = pont.etat.identite?.valeur
        Carte(titre: "Versions et identité", icone: "info.circle") {
            LigneInfo("Firmware", h?.fw, mono: true)
            if let fw = h?.fw, let desc = h?.fwDesc, fw != desc {
                LigneInfo("Descripteur (Matter)", desc, couleur: .orange, mono: true)
            }
            LigneInfo("Environnement", h.map { "\($0.env ?? "?") · \($0.build ?? "?") · \($0.reseauBuild ?? "?")" })
            LigneInfo("Compilé le", h.map { "\($0.date ?? "?") \($0.heure ?? "")" })
            LigneInfo("Puce · IDF · Arduino", h.map { "\($0.puce ?? "?") · \($0.idf ?? "?") · \($0.arduino ?? "?")" })
            LigneInfo("Protocole", h.map { "v1 rev \($0.rev ?? 0)" })
            Divider()
            LigneInfo("Produit", i?.id.map { "\($0.fabricant ?? "?") · \($0.produit ?? "?")" })
            LigneInfo("Série", i?.id?.serie, mono: true)
            LigneInfo("MAC", i?.mac, mono: true)
            LigneInfo("Matériel", i?.id.map { "\($0.hwTxt ?? "?") (v\($0.hw ?? 0))" })
            if let caps = i?.caps, !caps.isEmpty {
                FlowCaps(caps: caps)
            }
        }
    }
}

private struct FlowCaps: View {
    let caps: [String]

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 4) { ForEach(caps, id: \.self) { Pastille(texte: $0, couleur: .blue) } }
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 4) { ForEach(caps.prefix(caps.count / 2), id: \.self) { Pastille(texte: $0, couleur: .blue) } }
                HStack(spacing: 4) { ForEach(caps.suffix(caps.count - caps.count / 2), id: \.self) { Pastille(texte: $0, couleur: .blue) } }
            }
        }
    }
}

// MARK: - Systeme

private struct CarteSysteme: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let h = pont.etat.helloBase?.valeur
        let sys = pont.etat.sante?.valeur.sys
        // Reglages du hello, suivis des json periode|compteurs|reseau acceptes.
        let session = pont.reglages
        Carte(titre: "Démarrage et système", icone: "cpu") {
            LigneInfo("En marche depuis", Format.duree(secondes: pont.etat.upS))
            LigneInfo("Démarrage (boot)", pont.etat.boot, mono: true)
            LigneInfo("Cause", h.map { "\($0.reset ?? "?") (\($0.resetN.map(String.init) ?? "?"))" })
            LigneInfo("Tas libre · minimum", sys.map { "\(Format.octets($0.heap)) · \(Format.octets($0.heapMin))" })
            LigneInfo("Plus grand bloc", Format.octets(sys?.heapBloc))
            LigneInfo("Pile de loop() jamais utilisée", Format.octets(sys?.pileBoucle))
            Divider()
            LigneInfo("Session", session.map { "\($0.transport ?? "?") · bail \($0.bailS.map { "\($0) s" } ?? "?")" })
            LigneInfo("Périodes état · compteurs · réseau",
                      session.map { "\(Format.ms($0.periodeMs)) · \(Format.ms($0.compteursMs)) · \(Format.ms($0.reseauMs))" })
        }
    }
}
