import Foundation
import Testing
@testable import HaloProtocole

/// Octets d'une ligne machine : RS + JSON + LF.
func ligneMachine(_ json: String) -> [UInt8] {
    [Octets.rs] + Array(json.utf8) + [Octets.lf]
}

let exempleHb = #"{"v":1,"t":"hb","n":641,"ms":202000,"boot":"3FA2C901","up_s":202,"json_perdus":0}"#

@Suite("Tramage (2.4)")
struct TramageTests {
    @Test func ligneMachineSimple() throws {
        var r = RecepteurLignes()
        let e = r.alimenter(ligneMachine(exempleHb))
        #expect(e.count == 1)
        guard case .machine(let l) = e.first else { Issue.record("pas une ligne machine"); return }
        #expect(l.enveloppe.t == "hb")
        #expect(l.enveloppe.n == 641)
        #expect(l.enveloppe.ms == 202000)
        guard case .battement(let b) = l.message else { Issue.record("pas un hb"); return }
        #expect(b.boot == "3FA2C901")
        #expect(b.upS == 202)
        #expect(r.compteurs.lignesMachine == 1)
    }

    @Test func octetParOctet() {
        var r = RecepteurLignes()
        var sortie: [ElementRecu] = []
        for o in ligneMachine(exempleHb) + Array("texte humain\r\n".utf8) {
            sortie += r.alimenter([o])
        }
        #expect(sortie.count == 2)
        if case .machine = sortie[0] {} else { Issue.record("attendu machine") }
        #expect(sortie[1] == .texte(LigneTexte(texte: "texte humain", classe: .commande)))
    }

    @Test func texteAvantRSEtCRFinal() {
        var r = RecepteurLignes()
        let octets = Array("> ".utf8) + ligneMachine(exempleHb).dropLast() + [Octets.cr, Octets.lf]
        let e = r.alimenter(octets)
        #expect(e.count == 2)
        #expect(e[0] == .texte(LigneTexte(texte: "> ", classe: .invite)))
        if case .machine = e[1] {} else { Issue.record("attendu machine apres l'invite") }
    }

    @Test func dernierRSDeLaLigne() {
        // Ligne machine coupee sans LF, suivie d'une ligne complete : le dernier RS gagne.
        var r = RecepteurLignes()
        let coupee = Array("> ".utf8) + [Octets.rs] + Array(#"{"v":1,"t":"etat","n":3,"ms"#.utf8)
        let e = r.alimenter(coupee + ligneMachine(exempleHb))
        #expect(e.count == 3)
        #expect(e[0] == .texte(LigneTexte(texte: "> ", classe: .invite)))
        if case .abimee = e[1] {} else { Issue.record("le debut coupe est une ligne abimee, pas du texte") }
        if case .machine(let l) = e[2] { #expect(l.enveloppe.t == "hb") } else { Issue.record("attendu hb") }
        #expect(r.compteurs.lignesAbimees == 1)
    }

    @Test func logIDFAuMilieuPuisFragment() {
        // Un log IDF s'intercale au milieu d'une ligne machine (2.1) : la ligne
        // est abimee, sa suite est un fragment, jamais du texte de commande.
        var r = RecepteurLignes()
        let debut = [Octets.rs] + Array(#"{"v":1,"t":"hb","n":641,"ms":2020"#.utf8)
        let log = Array("E (48213) chip[DL]: rafale\n".utf8)
        let suite = Array(#"00,"boot":"3FA2C901","up_s":202,"json_perdus":0}"#.utf8) + [Octets.lf]
        let e = r.alimenter(debut + log + suite)
        #expect(e.count == 2)
        if case .abimee = e[0] {} else { Issue.record("attendu abimee, obtenu \(e[0])") }
        if case .fragment = e[1] {} else { Issue.record("attendu fragment, obtenu \(e[1])") }
        #expect(r.compteurs.lignesAbimees == 1)
        #expect(r.compteurs.fragments == 1)
        #expect(r.compteurs.lignesTexte == 0)
    }

    @Test func fragmentApresPause() {
        var r = RecepteurLignes()
        _ = r.alimenter(Array("ligne\n".utf8))
        r.signalerPause()
        let e = r.alimenter(Array(#"1,"json_perdus":0}"#.utf8) + [Octets.lf])
        if case .fragment = e.first {} else { Issue.record("attendu fragment apres une pause") }
        // Une ligne en '}' qui ne suit ni pause ni ligne abimee reste du texte.
        let t = r.alimenter(Array("bloc {x}\n".utf8))
        if case .texte = t.first {} else { Issue.record("attendu texte") }
    }

    @Test func resynchronisationALOuverture() {
        var r = RecepteurLignes()
        r.resynchroniser()
        let e = r.alimenter(Array(#"reste,"up_s":3}"#.utf8) + [Octets.lf] + ligneMachine(exempleHb))
        #expect(e.count == 1)
        if case .machine = e.first {} else { Issue.record("le reste avant le 1er LF doit etre jete") }
    }

    @Test func debordementSansLF() {
        var r = RecepteurLignes()
        let e = r.alimenter([UInt8](repeating: UInt8(ascii: "x"), count: 2049))
        #expect(e.count == 1)
        if case .debordement(let t) = e.first { #expect(t.count == 2049) } else { Issue.record("attendu debordement") }
        #expect(r.compteurs.debordements == 1)
        // La suite repart proprement.
        let s = r.alimenter(ligneMachine(exempleHb))
        if case .machine = s.first {} else { Issue.record("attendu machine apres debordement") }
    }

    @Test func lignesAbimees() {
        var r = RecepteurLignes()
        let cas = [
            #"{"t":"hb","v":1,"n":1}"#,             // ne commence pas par {"v":
            #"{"v":1,"t":"hb","n":1"#,              // ne finit pas par }
            #"{"v":1,"t":"hb","n":1,,}"#,           // JSON invalide
            #"{"v":1,"t":"hb"}"#,                   // n absent
            #"{"v":"1","t":"hb","n":1}"#,           // v mal type
            #"{"v":1,"t":"hb","n":-4}"#,            // n hors 0..4294967295
            #"{"v":1,"t":"hb","n":4294967296}"#,
            "{\"v\":1,\"t\":\"hb\",\"n\":1,\"x\":\"" + String(repeating: "a", count: 1010) + "\"}",
        ]
        for c in cas {
            let e = r.alimenter(ligneMachine(c))
            if case .abimee = e.first {} else { Issue.record("attendu abimee pour \(c.prefix(40)) : \(e)") }
        }
        #expect(r.compteurs.lignesAbimees == cas.count)
        #expect(r.compteurs.lignesTexte == 0)
    }

    @Test func versionEtTypeInconnus() {
        var r = RecepteurLignes()
        let v2 = r.alimenter(ligneMachine(#"{"v":2,"t":"hello","n":0,"ms":1,"bloc":"base"}"#))
        #expect(v2 == [.versionInconnue(v: 2, t: "hello")])
        let t = r.alimenter(ligneMachine(#"{"v":1,"t":"futur","n":1,"ms":2,"champ":[1,2]}"#))
        if case .machine(let l) = t.first { #expect(l.message == .inconnu) } else { Issue.record("attendu machine") }
        #expect(r.compteurs.versionsInconnues == 1)
        #expect(r.compteurs.typesInconnus == 1)
    }

    @Test func champsEtValeursInconnusIgnores() {
        var r = RecepteurLignes()
        let e = r.alimenter(ligneMachine(
            #"{"v":1,"t":"led","n":5,"ms":9,"motif":"violet_disco","avant":"operationnel","test":false,"nouveau":{"a":1}}"#))
        guard case .machine(let l) = e.first, case .led(let led) = l.message else {
            Issue.record("attendu led : \(e)"); return
        }
        #expect(led.motif == .inconnu)
        #expect(led.avant == .operationnel)
    }

    @Test func champObligatoireAbsent() {
        var r = RecepteurLignes()
        let e = r.alimenter(ligneMachine(#"{"v":1,"t":"reponse","n":5,"ms":9,"etape":"fin","ok":true,"code":"ok"}"#))
        if case .invalide(let t, _) = e.first { #expect(t == "reponse") } else { Issue.record("attendu invalide : \(e)") }
        #expect(r.compteurs.messagesInvalides == 1)
    }

    @Test func texteUTF8AvecRemplacement() {
        var r = RecepteurLignes()
        let e = r.alimenter([0x61, 0xFF, 0x62, Octets.lf])
        if case .texte(let t) = e.first { #expect(t.texte == "a\u{FFFD}b") } else { Issue.record("attendu texte") }
    }
}

@Suite("Classement du texte (2.5)")
struct ClasseurTexteTests {
    @Test func classes() {
        #expect(ClasseurTexte.classer("E (48213) chip[DL]: rafale").classe == .logIDF)
        #expect(ClasseurTexte.classer("E (48213) chip[DL]: rafale").niveau == "E")
        #expect(ClasseurTexte.classer("W (1) tag: x").niveau == "W")
        #expect(ClasseurTexte.classer("[    1234][E][fichier.cpp:12] fonction(): x").classe == .logArduino)
        #expect(ClasseurTexte.classer("[lampe] injoignable : consigne abandonnee").classe == .annonce)
        #expect(ClasseurTexte.classer("[matter] fenetre fermee").classe == .annonce)
        #expect(ClasseurTexte.classer("ESP-ROM:esp32c6-20220919").classe == .demarrage)
        #expect(ClasseurTexte.classer("=== BenQ ScreenBar Halo -> Matter ===").classe == .demarrage)
        #expect(ClasseurTexte.classer(">").classe == .invite)
        #expect(ClasseurTexte.classer("> ").classe == .invite)
        #expect(ClasseurTexte.classer("  consigne    : allumee deux").classe == .commande)
        #expect(ClasseurTexte.classer("E (x) tag: y").classe == .commande)
    }

    @Test func sequencesANSI() {
        let l = ClasseurTexte.classer("\u{1B}[0;31mE (5) wifi: echec\u{1B}[0m")
        #expect(l.texte == "E (5) wifi: echec")
        #expect(l.classe == .logIDF)
    }

    @Test func ancienFirmware() {
        #expect(ClasseurTexte.estRefusIdAncienFirmware(#"Commande inconnue : "id=1". Tape 'help'."#))
        #expect(!ClasseurTexte.estRefusIdAncienFirmware(#"Commande inconnue : "lampx". Tape 'help'."#))
        #expect(!ClasseurTexte.estRefusIdAncienFirmware(#"Commande inconnue : "id=". Tape 'help'."#))
    }
}
