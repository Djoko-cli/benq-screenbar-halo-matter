import HaloProtocole
import SwiftUI

/// Carte du tableau de bord.
struct Carte<Contenu: View>: View {
    let titre: String
    let icone: String
    var accent: Color = .secondary
    @ViewBuilder let contenu: Contenu

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(titre, systemImage: icone)
                .font(.headline)
                .foregroundStyle(accent == .secondary ? .primary : accent)
            contenu
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(accent == .secondary ? .clear : accent.opacity(0.6)))
    }
}

/// Ligne "libelle : valeur".
struct LigneInfo: View {
    let libelle: String
    let valeur: String
    var couleur: Color?
    var mono = false

    init(_ libelle: String, _ valeur: String?, couleur: Color? = nil, mono: Bool = false) {
        self.libelle = libelle
        self.valeur = valeur ?? "–"
        self.couleur = couleur
        self.mono = mono
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(libelle)
                .foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(valeur)
                .font(mono ? .body.monospaced() : .body)
                .foregroundStyle(couleur ?? .primary)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
        }
        .font(.callout)
    }
}

/// Petite etiquette coloree.
struct Pastille: View {
    let texte: String
    var couleur: Color = .secondary

    var body: some View {
        Text(texte)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .foregroundStyle(couleur)
            .background(couleur.opacity(0.15), in: Capsule())
    }
}

/// Jauge "valeur / seuil".
struct JaugeSeuil: View {
    let libelle: String
    let valeur: Int?
    let seuil: Int?

    var body: some View {
        let v = Double(valeur ?? 0)
        let s = Double(max(seuil ?? 1, 1))
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(libelle).foregroundStyle(.secondary)
                Spacer()
                Text("\(valeur.map(String.init) ?? "–") / \(seuil.map(String.init) ?? "–")").monospacedDigit()
            }
            .font(.callout)
            ProgressView(value: min(v, s), total: s)
                .tint(v >= s ? .red : (v >= s * 0.6 ? .orange : .accentColor))
        }
    }
}

// MARK: - Voyant

/// Le voyant de la carte, anime d'apres le motif et les constantes de
/// `src/status_led.h` (la couleur instantanee n'est pas transmise, 7.9).
struct VoyantLed: View {
    let motif: MotifLed?
    let depuis: Date?
    var taille: CGFloat = 18

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30)) { contexte in
            let t = max(0, contexte.date.timeIntervalSince(depuis ?? .distantPast))
            let (couleur, intensite) = Self.rendu(motif, t: t)
            ZStack {
                Circle().fill(Color.black.opacity(0.75))
                Circle().fill(couleur.opacity(intensite))
                Circle().strokeBorder(.white.opacity(0.25), lineWidth: 1)
            }
            .frame(width: taille, height: taille)
            .shadow(color: couleur.opacity(intensite * 0.9), radius: intensite * taille * 0.5)
        }
        .accessibilityLabel(motif?.libelle ?? "voyant inconnu")
    }

    /// Couleur et intensite (0..1), `t` secondes apres le debut du motif.
    static func rendu(_ motif: MotifLed?, t: Double) -> (Color, Double) {
        let ms = t * 1000
        switch motif {
        case .identification:
            // Roue des couleurs, un tour en 2000 ms (kRainbowMs).
            return (Color(hue: ms.truncatingRemainder(dividingBy: 2000) / 2000, saturation: 1, brightness: 1), 1)
        case .injoignable:
            // Rouge, 3 clignements de 200 ms / 200 ms (kRedHalfMs, kRedBlinks), puis noir.
            guard ms < 1200 else { return (.red, 0) }
            return (.red, Int(ms / 200) % 2 == 0 ? 1 : 0)
        case .panneRadio:
            return (.red, 1)
        case .livree:
            // Eclat vert de 150 ms (kDeliveredMs).
            return (.green, ms < 150 ? 1 : 0)
        case .nonAppaire:
            return (.blue, Int(ms / 250) % 2 == 0 ? 1 : 0)
        case .horsReseau:
            return (.orange, Int(ms / 1000) % 2 == 0 ? 1 : 0)
        case .operationnel:
            // Eteint, lueur blanche de 600 ms toutes les 10 s (kGlowPeriodMs, kGlowMs).
            let phase = ms.truncatingRemainder(dividingBy: 10_000)
            guard phase < 600 else { return (.white, 0) }
            return (.white, 0.8 * (1 - abs(phase - 300) / 300))
        case .inconnu, nil:
            return (.gray, 0.2)
        }
    }
}

// MARK: - Formats

enum Format {
    private static let styleHeure = Date.FormatStyle()
        .hour(.twoDigits(amPM: .omitted)).minute(.twoDigits).second(.twoDigits)
        .secondFraction(.fractional(3))

    /// "14:02:11.512"
    static func heure(_ d: Date) -> String { d.formatted(styleHeure) }

    static func duree(secondes s: Int?) -> String {
        guard let s else { return "–" }
        let j = s / 86_400, h = (s % 86_400) / 3600, m = (s % 3600) / 60, sec = s % 60
        if j > 0 { return "\(j) j \(h) h \(m) min" }
        if h > 0 { return "\(h) h \(m) min \(sec) s" }
        if m > 0 { return "\(m) min \(sec) s" }
        return "\(sec) s"
    }

    static func ms(_ v: Int?) -> String {
        guard let v else { return "–" }
        return v >= 10_000 ? "\(v / 1000) s" : "\(v) ms"
    }

    static func octets(_ v: Int?) -> String {
        guard let v else { return "–" }
        return ByteCountFormatter.string(fromByteCount: Int64(v), countStyle: .memory)
    }

    static func oui(_ b: Bool?) -> String {
        guard let b else { return "–" }
        return b ? "oui" : "non"
    }

    static func hexa(_ v: Int?) -> String {
        v.map { String(format: "%02X", $0) } ?? "–"
    }
}

extension MoteurSession.Phase {
    var couleur: Color {
        switch self {
        case .connecte: .green
        case .attenteHello, .resynchro: .orange
        case .ferme, .modeHumain: .secondary
        case .ancienFirmware, .sansReponse, .versionInconnue: .red
        }
    }
}
