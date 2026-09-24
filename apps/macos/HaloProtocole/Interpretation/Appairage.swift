import Foundation

/// Codes d'appairage Matter du pont (`reseau`, bloc `thread`, objet `matter`,
/// section 5.5) : l'etiquette du pont, sur l'USB seulement (jamais a distance).
/// Ils ne servent que pendant une fenetre de mise en service : pont neuf, remis
/// a zero, ou retire de son dernier controleur.
public enum CodeAppairage {
    /// Code manuel groupe comme dans Maison : 11 chiffres en 4-3-4
    /// (`3497-011-2332`), 21 chiffres en 4-3-4-5-5 ; toute autre forme telle quelle.
    public static func lisible(_ code: String) -> String {
        guard code.allSatisfy({ ("0"..."9").contains($0) }) else { return code }
        let groupes: [Int]
        switch code.count {
        case 11: groupes = [4, 3, 4]
        case 21: groupes = [4, 3, 4, 5, 5]
        default: return code
        }
        var morceaux: [Substring] = []
        var debut = code.startIndex
        for n in groupes {
            let fin = code.index(debut, offsetBy: n)
            morceaux.append(code[debut..<fin])
            debut = fin
        }
        return morceaux.joined(separator: "-")
    }

    /// Charge d'un QR code Matter (`MT:` puis du base38) : seule forme dessinee.
    public static func chargeValide(_ charge: String) -> Bool {
        guard charge.hasPrefix("MT:"), charge.count > 3, charge.count <= 64 else { return false }
        let base38 = Set("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.")
        return charge.dropFirst(3).allSatisfy { base38.contains($0) }
    }
}
