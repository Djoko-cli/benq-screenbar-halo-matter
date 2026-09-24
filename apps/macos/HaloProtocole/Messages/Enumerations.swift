import Foundation

/// Enumeration du protocole qui range une valeur inconnue sous `inconnu`
/// sans echouer (section 9.1 : ajouts additifs dans une meme `v`).
public protocol EnumeTolerante: RawRepresentable, Codable, Sendable, Hashable, CaseIterable
where RawValue == String {
    static var inconnu: Self { get }
}

extension EnumeTolerante {
    public init(from decoder: any Decoder) throws {
        let conteneur = try decoder.singleValueContainer()
        let brut = try conteneur.decode(String.self)
        self = Self(rawValue: brut) ?? Self.inconnu
    }

    public func encode(to encoder: any Encoder) throws {
        var conteneur = encoder.singleValueContainer()
        try conteneur.encode(rawValue)
    }
}

/// Selection de lampes (`State::lamps & F_LAMPS`).
public enum Lampes: String, EnumeTolerante {
    case avant, arriere, deux, inconnu
}

/// Codes de champs de consigne (`a_livrer`, `confirme`, `champs`).
public enum ChampConsigne: String, EnumeTolerante {
    case marche, lum, temp, inconnu
}

/// `etat.lampe.phase`.
public enum PhasePilote: String, EnumeTolerante {
    case repos, rafale, reprise, inconnu
}

/// `etat.lampe.lien` (la valeur `inconnu` existe aussi dans le protocole).
public enum EtatLien: String, EnumeTolerante {
    case inconnu, ok, perdu
}

/// `etat.sante.radio.mode`.
public enum ModeRadio: String, EnumeTolerante {
    case inconnu, reset, emission, ecoute, veille
}

/// `etat.sante.surveil.symptome`, `module.symptome`.
public enum Symptome: String, EnumeTolerante {
    case delais, bruit, sourde, inconnu
}

/// Cause d'une relance du module (`relance.cause`, `surveil.derniere.cause`).
public enum CauseRelance: String, EnumeTolerante {
    case verif, delais, bruit, sourde, l3, inconnu
}

/// Motif du voyant (section 7.9).
public enum MotifLed: String, EnumeTolerante {
    case identification
    case injoignable
    case panneRadio = "panne_radio"
    case livree
    case nonAppaire = "non_appaire"
    case horsReseau = "hors_reseau"
    case operationnel
    case inconnu
}

/// Tranche du pilote (`tx.tranche`, `tranches[].tranche`, `livraison.derniere`).
public enum TypeTranche: String, EnumeTolerante {
    case lum, temp, a, brut, inconnu
}

/// Classement d'une trame entendue (`rx.type`).
public enum TypeTrame: String, EnumeTolerante {
    case lum, temp, a
    case accuseLampe = "accuse_lampe"
    case service, favori, invalide
    case crcFaux = "crc_faux"
    case inconnu
}

/// `rx.source`.
public enum SourceTrame: String, EnumeTolerante {
    case ecoute, accuse, inconnu
}

/// Verdict d'un paquet emis (`tx.verdict`).
public enum VerdictTx: String, EnumeTolerante {
    case ack
    case ackTrame = "ack_trame"
    case maxRt = "max_rt"
    case delai, fifo, inconnu
}

/// `livraison.issue`.
public enum IssueLivraison: String, EnumeTolerante {
    case livree, abandon, annulee, inconnu
}

/// `livraison.cause`.
public enum CauseAbandon: String, EnumeTolerante {
    case injoignable, module, inconnu
}

/// `module.etat` (section 7.5).
public enum EtatModule: String, EnumeTolerante {
    case panne, retabli, perdu, retrouve
    case configRejetee = "config_rejetee"
    case configVerifiee = "config_verifiee"
    case inconnu
}

/// `reponse.etape`.
public enum EtapeReponse: String, EnumeTolerante {
    case debut, fin, inconnu
}

/// `reponse.code` (section 6.3).
public enum CodeReponse: String, EnumeTolerante {
    case ok, accepte, differe
    case enCours = "en_cours"
    case execute, usage, refuse
    case radioAbsente = "radio_absente"
    case radioPerdue = "radio_perdue"
    case commandeInconnue = "inconnue"
    case tropLong = "trop_long"
    case cadence, interdite
    case inconnu
}

/// `reponse.suite`.
public enum SuiteReponse: String, EnumeTolerante {
    case livraison, aucune, inconnu
}

/// `fin.cause`.
public enum CauseFin: String, EnumeTolerante {
    case commande, bail, inconnu
}

/// `abonnement.quoi` (section 7.7).
public enum QuoiAbonnement: String, EnumeTolerante {
    case demande, etabli, termine, reprise, session
    case repriseAbonne = "reprise_abonne"
    case inconnu
}
