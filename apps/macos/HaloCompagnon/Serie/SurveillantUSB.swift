import Foundation
import IOKit
import IOKit.serial

/// Un port serie vu par IOKit.
struct PortUSB: Identifiable, Hashable, Sendable {
    var id: String { chemin }
    /// `/dev/cu.*`
    let chemin: String
    let vid: Int?
    let pid: Int?
    /// Numero de serie USB : l'adresse MAC de la puce (3.1, a confirmer au banc).
    let serie: String?
    let produit: String?

    /// VID Espressif : l'USB Serial/JTAG du C6 est 303A:1001.
    var estEspressif: Bool { vid == 0x303A }

    var libelle: String {
        var s = chemin.replacingOccurrences(of: "/dev/cu.", with: "")
        if let vid, let pid { s += String(format: " (%04X:%04X)", vid, pid) }
        if let serie, !serie.isEmpty { s += " · \(serie)" }
        return s
    }
}

/// Liste des ports et notifications d'arrivee et de depart (IOKit,
/// `IOServiceAddMatchingNotification` sur `IOSerialBSDClient`).
@MainActor
final class SurveillantUSB {
    static let vidEspressif = 0x303A

    /// Appele sur la file principale a chaque arrivee ou depart d'un port.
    var changement: (([PortUSB]) -> Void)?

    // Liberes aussi par deinit (non isole) : sans cela, un rappel IOKit viserait un objet detruit.
    nonisolated(unsafe) private var portNotification: IONotificationPortRef?
    nonisolated(unsafe) private var iterateurs: [io_iterator_t] = []

    deinit {
        for i in iterateurs { IOObjectRelease(i) }
        if let portNotification { IONotificationPortDestroy(portNotification) }
    }

    func demarrer() {
        guard portNotification == nil, let port = IONotificationPortCreate(kIOMainPortDefault) else { return }
        portNotification = port
        IONotificationPortSetDispatchQueue(port, DispatchQueue.main)
        let contexte = Unmanaged.passUnretained(self).toOpaque()
        for type in [kIOFirstMatchNotification, kIOTerminatedNotification] {
            var iterateur: io_iterator_t = 0
            let resultat = IOServiceAddMatchingNotification(port, type, Self.critere(), { contexte, iterateur in
                // Rappel C, sur la file principale (IONotificationPortSetDispatchQueue).
                SurveillantUSB.vider(iterateur)
                guard let contexte else { return }
                MainActor.assumeIsolated {
                    let moi = Unmanaged<SurveillantUSB>.fromOpaque(contexte).takeUnretainedValue()
                    moi.changement?(SurveillantUSB.lister())
                }
            }, contexte, &iterateur)
            if resultat == KERN_SUCCESS {
                // Vider l'iterateur arme la notification.
                Self.vider(iterateur)
                iterateurs.append(iterateur)
            }
        }
    }

    func arreter() {
        for i in iterateurs { IOObjectRelease(i) }
        iterateurs.removeAll()
        if let portNotification { IONotificationPortDestroy(portNotification) }
        portNotification = nil
    }

    private nonisolated static func vider(_ iterateur: io_iterator_t) {
        while case let objet = IOIteratorNext(iterateur), objet != 0 {
            IOObjectRelease(objet)
        }
    }

    private nonisolated static func critere() -> CFDictionary {
        let d = IOServiceMatching(kIOSerialBSDServiceValue) as NSMutableDictionary
        d[kIOSerialBSDTypeKey] = kIOSerialBSDAllTypes
        return d as CFDictionary
    }

    /// Ports `/dev/cu.*`, ceux d'Espressif en tete.
    nonisolated static func lister() -> [PortUSB] {
        var iterateur: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, critere(), &iterateur) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterateur) }
        var ports: [PortUSB] = []
        while case let service = IOIteratorNext(iterateur), service != 0 {
            defer { IOObjectRelease(service) }
            guard let chemin = propriete(service, kIOCalloutDeviceKey, parents: false) as? String,
                  chemin.hasPrefix("/dev/cu.") else { continue }
            ports.append(PortUSB(
                chemin: chemin,
                vid: propriete(service, "idVendor") as? Int,
                pid: propriete(service, "idProduct") as? Int,
                serie: propriete(service, "USB Serial Number") as? String,
                produit: propriete(service, "USB Product Name") as? String))
        }
        return ports.sorted { a, b in
            if a.estEspressif != b.estEspressif { return a.estEspressif }
            return a.chemin < b.chemin
        }
    }

    private nonisolated static func propriete(_ service: io_object_t, _ cle: String, parents: Bool = true) -> Any? {
        if parents {
            return IORegistryEntrySearchCFProperty(service, kIOServicePlane, cle as CFString, kCFAllocatorDefault,
                                                   IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents))
        }
        return IORegistryEntryCreateCFProperty(service, cle as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }
}
