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
                    // Pont pas encore mis en service : l'appairage passe avant tout.
                    if avecMatter && horsService { CarteAppairage() }
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

    /// Pont connu comme pas mis en service (bloc `thread`, ou `sante` a defaut).
    private var horsService: Bool {
        (pont.etat.thread?.valeur.matter?.enService ?? pont.etat.sante?.valeur.matter?.enService) == false
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
                    Text(verbatim: "")
                    Text("Consigne").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                    Text("Cru").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                }
                ligne("Marche", l?.consigne.marche.map(Self.marche), l?.cru.marche.map(Self.marche),
                      aLivrer.contains(.marche))
                ligne("Lampes", l?.consigne.lampes?.libelle, l?.cru.lampes?.libelle, aLivrer.contains(.marche))
                ligne("Luminosité", lum(l?.consigne), lum(l?.cru), aLivrer.contains(.lum))
                ligne("Température", temp(l?.consigne), temp(l?.cru), aLivrer.contains(.temp))
            }
            .font(.callout)
            Divider()
            HStack(spacing: 6) {
                Text("À livrer :").foregroundStyle(.secondary)
                if aLivrer.isEmpty { Pastille("rien", couleur: .green) }
                ForEach(l?.aLivrer ?? [], id: \.self) { Pastille(texte: $0.libelle, couleur: .orange) }
                Spacer()
                if let v = l?.version { Text("version \(String(v))").foregroundStyle(.secondary).monospacedDigit() }
            }
            .font(.callout)
            LigneInfo("Pilote", l?.phase.map { p in
                if p == .reprise, let r = l?.repriseMs { return tr("\(p.libelle) (dans \(Format.ms(r)))") }
                return p.libelle
            }, couleur: l?.phase == .reprise ? .orange : nil)
            LigneInfo("Tours ratés", l?.echecs.map { e in
                "\(e) / \(pont.etat.config?.valeur.reglages?.reprises.map(String.init) ?? "?")"
            })
            LigneInfo("Mémoire de sélection", l?.memoire?.libelle)
            LigneInfo("Bouton A", l.map { tr("dernier n° \(String($0.dernierA ?? 0)), \($0.aEntendus ?? 0) entendus") })
            LigneInfo("Écoute de fond", Format.oui(l?.ecoute))
            if let t = pont.etat.tranches?.valeur.tranches, !t.isEmpty {
                Divider()
                Text("Tranches actives").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                ForEach(Array(t.enumerated()), id: \.offset) { _, tranche in
                    LigneInfo(verbatim: tranche.tranche?.libelle.avecMajuscule ?? "?",
                              tr("\(tranche.charge ?? "") · essai \(tranche.essais ?? 0) · \(tranche.accuses ?? 0)/\(tranche.paquets ?? 0) accusés"),
                              mono: true)
                }
            }
        }
    }

    private static func marche(_ m: Bool) -> String { m ? tr("allumée") : tr("éteinte") }

    @ViewBuilder
    private func ligne(_ nom: LocalizedStringKey, _ c: String?, _ r: String?, _ enAttente: Bool) -> some View {
        GridRow {
            Text(nom).foregroundStyle(.secondary)
            Text(c ?? "–").fontWeight(enAttente ? .semibold : .regular).foregroundStyle(enAttente ? .orange : .primary)
            Text(r ?? "–").foregroundStyle(c != r ? .orange : .primary)
        }
    }

    private func lum(_ e: EtatLampe?) -> String? {
        guard let e, let lum = e.lum else { return nil }
        let niveau = e.niveau ?? pont.etat.correspondance.niveau(brut: lum)
        return tr("\(Format.hexa(lum)) · niveau \(niveau) (\(CorrespondanceLuminosite.pourcent(niveau: niveau)) %)")
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
                if s?.radio?.presente == false { Pastille("absent", couleur: .red) } else { Pastille("présent", couleur: .green) }
                if s?.radio?.perdue == true { Pastille("perdu (L3)", couleur: .red) }
                Pastille(texte: s?.radio?.mode?.libelle ?? "–")
                if s?.radio?.configuree == true { Pastille("configuré", couleur: .green) } else { Pastille("non configuré", couleur: .orange) }
                if let sy = s?.surveil?.symptome { Pastille(texte: sy.libelle, couleur: .orange) }
            }
            LigneInfo("Quartz · calibration", "\(Format.oui(s?.radio?.quartz)) · \(Format.oui(s?.radio?.calib))")
            JaugeSeuil(libelle: "Délais TX de suite", valeur: s?.surveil?.delaisSuite, seuil: seuils?.delaisSuite)
            JaugeSeuil(libelle: "Réarmements hors RX (10 s)", valeur: s?.surveil?.horsRx10s, seuil: seuils?.sourdHorsRx)
            JaugeSeuil(libelle: "Relances sans guérison", valeur: s?.surveil?.sansGuerison, seuil: seuils?.sansGuerison)
            LigneInfo("Fenêtre d'écoute",
                      tr("\(s?.surveil?.fenTrames ?? 0) trames") + ", " + tr("\(s?.surveil?.fenCrcFaux ?? 0) CRC faux"))
            LigneInfo("Relances automatiques", s?.surveil?.relances.map(String.init))
            if let d = s?.surveil?.derniere {
                LigneInfo("Dernière relance", tr("\(d.cause?.libelle ?? "?"), il y a \(Format.duree(secondes: d.ilYaS))"))
            }
            if let r = pont.etat.derniereRelance {
                Text(Interpretation.relance(r.valeur)).font(.caption).foregroundStyle(.secondary)
            }
            if let m = pont.etat.dernierModule {
                Text(verbatim: "\(Format.heure(m.date)) · \(Interpretation.module(m.valeur))")
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
                    Text((motif?.libelle ?? tr("inconnu")).avecMajuscule).font(.title3.weight(.semibold))
                    Text((motif?.description ?? tr("motif pas encore reçu")).avecMajuscule).foregroundStyle(.secondary)
                    if pont.etat.ledTest { Pastille("Test du voyant en cours", couleur: .purple) }
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
    @State private var feuilleAppairage = false

    var body: some View {
        let r = pont.etat.thread?.valeur
        let m = pont.etat.sante?.valeur.matter
        let enService = (r?.matter?.enService ?? m?.enService) == true
        Carte(titre: "Thread et Matter", icone: "point.3.connected.trianglepath.dotted") {
            HStack(spacing: 6) {
                if enService {
                    Pastille("en service", couleur: .green)
                } else {
                    Pastille("pas en service", couleur: .orange)
                }
                if (r?.matter?.connecte ?? m?.connecte) == true {
                    Pastille("connecté", couleur: .green)
                } else {
                    Pastille("déconnecté", couleur: .red)
                }
                if m?.identify == true { Pastille(texte: "identify", couleur: .purple) }
                if let role = r?.thread?.role {
                    Pastille(texte: role, couleur: role == "detached" || role == "disabled" ? .red : .blue)
                }
            }
            LigneInfo("Canal", r?.thread.map { "\($0.canal.map(String.init) ?? "?") (\($0.mhz.map(String.init) ?? "?") MHz)" })
            LigneInfo("PAN · puissance", r?.thread.map { "\($0.pan ?? "?") · \($0.txDbm.map { "\($0) dBm" } ?? "?")" })
            LigneInfo("RSSI du parent", r?.thread?.parentRssi.map { "\($0) dBm" })
            LigneInfo("Mode", r?.thread.map { tr("\($0.mode ?? "?") (démarrage \($0.typeBoot ?? "?"), suivant \($0.typeSuivant ?? "?"))") })
            LigneInfo("Changements de rôle", r?.thread?.roles.map(String.init))
            LigneInfo("SRP", r?.thread?.srp.map { tr("\($0.hote ?? "?") · \($0.enregistres ?? 0)/\($0.services ?? 0) services") })
            LigneInfo("Fabriques", r?.matter?.fabriques.map(String.init))
            if let f = r?.fraisMs, f > 2000 { LigneInfo("Âge des valeurs", Format.ms(f), couleur: .orange) }
            if let ip = pont.etat.ip?.valeur {
                // Bloc ip (build Thread) : ce que l'app visera par le reseau.
                LigneInfo("Nom réseau", ip.srp?.nom.map { "\($0).local" }, mono: true)
                LigneInfo("Adresse (OMR)", ip.adresseOmr, mono: true)
                LigneInfo("Transport réseau", ip.udp.map { u in
                    guard u.ouvert == true, let e = u.empreinte else { return tr("coupé (aucune clé)") }
                    return tr("port \(u.port ?? 0) · clé \(e) · \(u.sessions ?? 0) session(s)")
                })
            }
            // Deja en service : l'etiquette du pont, a la demande (hors service,
            // elle a sa propre carte en tete du tableau de bord).
            if enService, let code = r?.matter?.codeManuel {
                Divider()
                HStack {
                    Text("Code d'appairage").foregroundStyle(.secondary)
                    Spacer()
                    Button("Afficher…") { feuilleAppairage = true }
                        .controlSize(.small)
                }
                .font(.callout)
                .sheet(isPresented: $feuilleAppairage) {
                    FeuilleAppairage(code: code, qr: r?.matter?.qr)
                }
            }
        }
    }
}

// MARK: - Appairage Matter

/// Pont pas encore mis en service : tout ce qu'il faut pour l'ajouter a Maison.
private struct CarteAppairage: View {
    @Environment(Pont.self) private var pont

    var body: some View {
        let m = pont.etat.thread?.valeur.matter
        Carte(titre: "Ajouter à Maison", icone: "qrcode.viewfinder", accent: .blue) {
            if let code = m?.codeManuel {
                HStack(alignment: .top, spacing: 16) {
                    if let qr = m?.qr { ImageCodeQR(charge: qr, cote: 168) }
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Le pont attend d'être ajouté à un contrôleur Matter.")
                            .fixedSize(horizontal: false, vertical: true)
                        Text("Dans Maison : + › Ajouter un accessoire, puis scanner ce code. Sans appareil photo : « Plus d'options » et le code à 11 chiffres.")
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                        CodeManuel(code: code)
                        Text("L'iPhone près du pont : la mise en service passe par le Bluetooth.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .font(.callout)
            } else {
                // Codes jamais envoyes par le reseau (10.5).
                Text("Le pont n'est pas encore mis en service. Ses codes d'appairage s'affichent par la liaison USB.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .font(.callout)
            }
        }
    }
}

/// Deja en service : l'etiquette du pont, et quand elle sert.
private struct FeuilleAppairage: View {
    let code: String
    let qr: String?
    @Environment(\.dismiss) private var fermer

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label("Code d'appairage Matter", systemImage: "qrcode")
                .font(.title3.weight(.semibold))
            HStack(alignment: .top, spacing: 18) {
                if let qr { ImageCodeQR(charge: qr, cote: 200) }
                VStack(alignment: .leading, spacing: 10) {
                    CodeManuel(code: code)
                    Text("Le pont est déjà dans Maison. Ce code ne sert que pendant une fenêtre de mise en service : pont remis à zéro (bouton BOOT tenu 8 s) ou retiré de son dernier contrôleur.")
                        .fixedSize(horizontal: false, vertical: true)
                    Text("Pour l'ajouter à un autre écosystème (Google Home, Alexa…), ouvrir sa fiche dans Maison et choisir « Activer le mode d'appairage » : Maison donne alors un code temporaire.")
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Text("Retirer le pont de Maison efface aussi la clé du transport réseau : elle se règle de nouveau par l'USB.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .font(.callout)
            }
            HStack {
                Spacer()
                Button("Fermer") { fermer() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 560)
    }
}

/// Code manuel groupe comme dans Maison, selectionnable, et un bouton pour le copier.
private struct CodeManuel: View {
    let code: String

    var body: some View {
        HStack(spacing: 10) {
            Text(verbatim: CodeAppairage.lisible(code))
                .font(.title2.monospaced().weight(.semibold))
                .textSelection(.enabled)
            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(code, forType: .string)
            } label: {
                Label("Copier", systemImage: "doc.on.doc")
            }
            .controlSize(.small)
            .help("Copie les chiffres du code manuel")
        }
    }
}

/// QR code Matter en noir sur blanc, avec sa marge de silence (lisible en mode
/// sombre aussi) ; rien si la charge n'est pas un `MT:...`.
private struct ImageCodeQR: View {
    let charge: String
    let cote: CGFloat

    var body: some View {
        if CodeAppairage.chargeValide(charge), let image = CodeQR.image(charge) {
            Image(nsImage: image)
                .interpolation(.none)
                .resizable()
                .frame(width: cote, height: cote)
                .padding(10)
                .background(.white, in: RoundedRectangle(cornerRadius: 8))
                .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.black.opacity(0.1)))
                .accessibilityLabel(Text("QR code Matter"))
                .help(Text(verbatim: charge))
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
                Text(verbatim: "\(a?.abonnements?.actifs ?? 0)").font(.largeTitle.weight(.semibold)).monospacedDigit()
                Text("actif(s)").foregroundStyle(.secondary)
                Spacer()
                if a?.reprise?.enCours == .booleen(true) { Pastille("reprise en cours", couleur: .orange) }
            }
            LigneInfo("Lectures", a?.abonnements?.lectures.map(String.init))
            LigneInfo("Sauvés (NVS)", a?.abonnements?.sauves.map(String.init))
            LigneInfo("Demandés · neufs · terminés",
                      a?.abonnements.map { "\($0.demandes ?? 0) · \($0.neufs ?? 0) · \($0.termines ?? 0)" })
            LigneInfo("Repris (pont · pile)", a?.abonnements.map { "\($0.reprisPont ?? 0) · \($0.reprisPile ?? 0)" })
            LigneInfo("Plafond", a?.abonnements.map { tr("\($0.plafondS ?? 0) s (\($0.plafonnes ?? 0) plafonnés)") })
            LigneInfo("Reprise automatique", Format.oui(a?.abonnements?.repriseAuto))
            LigneInfo("Passages de reprise", a?.reprise.map { tr("\($0.passages ?? 0) (\($0.auto ?? 0) auto, \($0.echecs ?? 0) échecs)") })
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
                Text(verbatim: "\(dernier.raison)\n\(dernier.brut.prefix(160))")
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
            LigneInfo("Environnement", h.map {
                "\($0.env ?? "?") · \($0.build.map(ValeurFirmware.build) ?? "?") · \($0.reseauBuild ?? "?")"
            })
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
            LigneInfo("Cause du démarrage", h.map {
                "\($0.reset.map(ValeurFirmware.causeDemarrage) ?? "?") (\($0.resetN.map(String.init) ?? "?"))"
            })
            LigneInfo("Tas libre · minimum", sys.map { "\(Format.octets($0.heap)) · \(Format.octets($0.heapMin))" })
            LigneInfo("Plus grand bloc", Format.octets(sys?.heapBloc))
            LigneInfo("Pile de loop() jamais utilisée", Format.octets(sys?.pileBoucle))
            Divider()
            LigneInfo("Session", session.map { tr("\($0.transport ?? "?") · bail \($0.bailS.map { "\($0) s" } ?? "?")") })
            LigneInfo("Périodes état · compteurs · réseau",
                      session.map { "\(Format.ms($0.periodeMs)) · \(Format.ms($0.compteursMs)) · \(Format.ms($0.reseauMs))" })
        }
    }
}
