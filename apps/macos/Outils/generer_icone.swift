#!/usr/bin/env swift
// Genere l'icone de Halo Compagnon, ainsi que les trois pistes explorees.
//
// Usage (depuis apps/macos) :
//   swift Outils/generer_icone.swift
//       -> HaloCompagnon/Ressources/Assets.xcassets/AppIcon.appiconset
//          (piste retenue, toutes les tailles macOS de 16 a 1024 px)
//   swift Outils/generer_icone.swift --pistes /private/tmp/halo-icons
//       -> candidate-1.png, candidate-2.png, candidate-3.png (1024 px)
//          et planche.png (chaque piste a 1024, 128, 32 et 16 px,
//          sur fond clair et sur fond sombre)
//
// Tout est trace en vectoriel (CoreGraphics) dans une grille de 1024 points
// au gabarit macOS : pastille de 824 points a coins continus (rayon 185,4)
// centree, marge de 100 points pour l'ombre portee. Jusqu'a 64 px, le trace
// est simplifie (barre epaissie, sans pied ni details) pour rester lisible.
// Aucune marque ni texte : une barre lumineuse posee sur un ecran, lumiere
// chaude vers le bureau, halo froid derriere l'ecran.

import CoreGraphics
import CoreImage
import CoreText
import Foundation
import ImageIO
import SwiftUI
import UniformTypeIdentifiers

// MARK: - Outils de trace

let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!
let moteurCI = CIContext(options: [.workingColorSpace: sRGB, .outputColorSpace: sRGB])

func teinte(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: sRGB, components: [
        CGFloat((hex >> 16) & 0xFF) / 255, CGFloat((hex >> 8) & 0xFF) / 255, CGFloat(hex & 0xFF) / 255, alpha,
    ])!
}

func degrade(_ arrets: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: sRGB, colors: arrets.map(\.1) as CFArray, locations: arrets.map(\.0))!
}

func pt(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x, y: y) }

/// Epaisseur en points de grille valant `px` pixels a la taille de rendu.
func pixels(_ px: CGFloat, _ taille: Int) -> CGFloat { px * 1024 / CGFloat(taille) }

/// Rectangle a coins continus (la courbe du gabarit Apple).
func pastille(_ r: CGRect, rayon: CGFloat) -> CGPath {
    RoundedRectangle(cornerRadius: rayon, style: .continuous).path(in: r).cgPath
}

func capsule(_ r: CGRect) -> CGPath {
    let k = min(r.width, r.height) / 2
    return CGPath(roundedRect: r, cornerWidth: k, cornerHeight: k, transform: nil)
}

let corps = pastille(CGRect(x: 100, y: 100, width: 824, height: 824), rayon: 185.4)

func contexte(_ largeur: Int, _ hauteur: Int) -> CGContext {
    let ctx = CGContext(data: nil, width: largeur, height: hauteur, bitsPerComponent: 8, bytesPerRow: 0,
                        space: sRGB, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high
    return ctx
}

/// Contexte d'icone : grille de 1024 points, origine en haut a gauche.
func contexteIcone(_ taille: Int) -> CGContext {
    let ctx = contexte(taille, taille)
    let s = CGFloat(taille) / 1024
    ctx.translateBy(x: 0, y: CGFloat(taille))
    ctx.scaleBy(x: s, y: -s)
    return ctx
}

/// Remplit `chemin` d'un degrade lineaire de `de` a `a`.
func remplir(_ c: CGContext, _ chemin: CGPath, _ g: CGGradient, de: CGPoint, a: CGPoint) {
    c.saveGState()
    c.addPath(chemin)
    c.clip()
    c.drawLinearGradient(g, start: de, end: a, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    c.restoreGState()
}

/// Trace `dessin` dans un calque a part, le floute (sigma en points de la
/// grille) et le compose sur `c` : lueurs et faisceaux aux bords doux.
func calque(_ c: CGContext, _ taille: Int, flou sigma: CGFloat, alpha: CGFloat = 1,
            mode: CGBlendMode = .normal, _ dessin: (CGContext) -> Void) {
    let cal = contexteIcone(taille)
    dessin(cal)
    var image = cal.makeImage()!
    let sig = Double(sigma * CGFloat(taille) / 1024)
    if sig > 0.05 {
        let entree = CIImage(cgImage: image)
        let sortie = entree.applyingGaussianBlur(sigma: sig).cropped(to: entree.extent)
        image = moteurCI.createCGImage(sortie, from: entree.extent)!
    }
    c.saveGState()
    c.concatenate(c.ctm.inverted())
    c.setAlpha(alpha)
    c.setBlendMode(mode)
    c.draw(image, in: CGRect(x: 0, y: 0, width: taille, height: taille))
    c.restoreGState()
}

/// Arc (polyligne) de centre `o`, angles en degres, 0 a droite, 90 en haut.
func arc(_ o: CGPoint, rayon r: CGFloat, de a0: CGFloat, a a1: CGFloat) -> CGPath {
    let p = CGMutablePath()
    let n = 64
    for i in 0...n {
        let a = (a0 + (a1 - a0) * CGFloat(i) / CGFloat(n)) * .pi / 180
        let q = pt(o.x + r * cos(a), o.y - r * sin(a))
        if i == 0 { p.move(to: q) } else { p.addLine(to: q) }
    }
    return p
}

// MARK: - Elements communs

/// La barre lumineuse : tube d'aluminium, diode chaude sur l'arete avant.
func barre(_ c: CGContext, _ taille: Int, _ r: CGRect, lueur: CGFloat = 1) {
    // Lueur chaude sous la barre.
    calque(c, taille, flou: r.height * 0.55, alpha: lueur, mode: .screen) { k in
        k.setFillColor(teinte(0xFFB45E))
        k.fillEllipse(in: CGRect(x: r.minX + r.height * 0.3, y: r.maxY - r.height * 0.55,
                                 width: r.width - r.height * 0.6, height: r.height * 1.1))
    }
    let tube = capsule(r)
    remplir(c, tube, degrade([
        (0, teinte(0xFBFCFE)), (0.30, teinte(0xE1E4EC)), (0.62, teinte(0xA3A9B8)), (1, teinte(0x4E5466)),
    ]), de: pt(r.midX, r.minY), a: pt(r.midX, r.maxY))
    // Reflet lateral (le tube est cylindrique).
    c.saveGState()
    c.addPath(tube)
    c.clip()
    c.drawLinearGradient(degrade([
        (0, teinte(0x000000, 0.28)), (0.12, teinte(0x000000, 0)), (0.88, teinte(0x000000, 0)), (1, teinte(0x000000, 0.28)),
    ]), start: pt(r.minX, r.midY), end: pt(r.maxX, r.midY), options: [])
    // Diode : fine ligne tres claire sur l'arete basse.
    let h = max(r.height * 0.2, 5)
    c.setFillColor(teinte(0xFFF6E2))
    c.addPath(capsule(CGRect(x: r.minX + r.height * 0.5, y: r.maxY - h * 1.25, width: r.width - r.height, height: h)))
    c.fillPath()
    c.restoreGState()
}

/// Ecran vu de face (coque, dalle, pied), sans la barre.
func ecran(_ c: CGContext, _ r: CGRect, petit: Bool, coque: (UInt32, UInt32), dalle: (UInt32, UInt32),
           pied: (UInt32, UInt32)) {
    if !petit {
        let cou = CGRect(x: r.midX - 34, y: r.maxY - 4, width: 68, height: 50)
        remplir(c, CGPath(rect: cou, transform: nil), degrade([(0, teinte(pied.1)), (1, teinte(pied.0))]),
                de: pt(cou.minX, cou.midY), a: pt(cou.maxX, cou.midY))
        let socle = CGRect(x: r.midX - 118, y: r.maxY + 40, width: 236, height: 20)
        remplir(c, capsule(socle), degrade([(0, teinte(pied.0)), (1, teinte(pied.1))]),
                de: pt(socle.midX, socle.minY), a: pt(socle.midX, socle.maxY))
    }
    remplir(c, pastille(r, rayon: petit ? 30 : 22), degrade([(0, teinte(coque.0)), (1, teinte(coque.1))]),
            de: pt(r.midX, r.minY), a: pt(r.midX, r.maxY))
    let d = r.insetBy(dx: petit ? 22 : 14, dy: petit ? 22 : 14)
    remplir(c, pastille(d, rayon: petit ? 12 : 8), degrade([(0, teinte(dalle.0)), (1, teinte(dalle.1))]),
            de: pt(d.midX, d.minY), a: pt(d.midX, d.maxY))
    if !petit {
        // Reflet diagonal discret sur la dalle.
        c.saveGState()
        c.addPath(pastille(d, rayon: 8))
        c.clip()
        let reflet = CGMutablePath()
        reflet.move(to: pt(d.minX + d.width * 0.52, d.minY))
        reflet.addLine(to: pt(d.minX + d.width * 0.80, d.minY))
        reflet.addLine(to: pt(d.minX + d.width * 0.48, d.maxY))
        reflet.addLine(to: pt(d.minX + d.width * 0.20, d.maxY))
        reflet.closeSubpath()
        c.addPath(reflet)
        c.setFillColor(teinte(0xFFFFFF, 0.035))
        c.fillPath()
        c.restoreGState()
    }
}

/// Liseres interieurs : lumiere en haut, ombre en bas (volume de la pastille).
func liseres(_ c: CGContext, clair: CGFloat, sombre: CGFloat) {
    c.saveGState()
    c.addPath(corps)
    c.setLineWidth(10)
    c.replacePathWithStrokedPath()
    c.clip()
    c.drawLinearGradient(degrade([
        (0, teinte(0xFFFFFF, clair)), (0.35, teinte(0xFFFFFF, 0)), (0.7, teinte(0x000000, 0)), (1, teinte(0x000000, sombre)),
    ]), start: pt(512, 100), end: pt(512, 924), options: [])
    c.restoreGState()
}

// MARK: - Piste 1 : Veille (scene de bureau la nuit)

/// Tache de lumiere elliptique (degrade radial a decroissance douce).
func tache(_ c: CGContext, _ o: CGPoint, rx: CGFloat, ry: CGFloat, _ hex: UInt32, _ alpha: CGFloat) {
    c.saveGState()
    c.translateBy(x: o.x, y: o.y)
    c.scaleBy(x: rx, y: ry)
    let arrets: [(CGFloat, CGColor)] = stride(from: 0, through: 1, by: 0.125).map { t in
        (t, teinte(hex, alpha * max(0, exp(-3.2 * t * t) - exp(-3.2) * t)))
    }
    c.drawRadialGradient(degrade(arrets), startCenter: .zero, startRadius: 0, endCenter: .zero, endRadius: 1, options: [])
    c.restoreGState()
}

/// Courbe lissee passant par `points` (milieux en guise d'ancres).
func courbeLissee(_ points: [CGPoint]) -> CGMutablePath {
    let p = CGMutablePath()
    p.move(to: points[0])
    for i in 1..<points.count - 1 {
        let m = pt((points[i].x + points[i + 1].x) / 2, (points[i].y + points[i + 1].y) / 2)
        p.addQuadCurve(to: m, control: points[i])
    }
    p.addLine(to: points[points.count - 1])
    return p
}

func veille(_ c: CGContext, _ taille: Int) {
    let petit = taille <= 64
    remplir(c, corps, degrade([(0, teinte(0x2E3870)), (0.55, teinte(0x151A3A)), (1, teinte(0x08090F))]),
            de: pt(512, 100), a: pt(512, 924))

    let barreR = petit ? CGRect(x: 262, y: 352, width: 500, height: 66) : CGRect(x: 312, y: 378, width: 400, height: 38)
    let ecranR = petit ? CGRect(x: 182, y: barreR.maxY + 4, width: 660, height: 372)
                       : CGRect(x: 212, y: barreR.maxY + 2, width: 600, height: 338)

    // Halo : lumiere froide renvoyee par le mur, derriere l'ecran.
    calque(c, taille, flou: petit ? 40 : 60, mode: .screen) { k in
        k.setFillColor(teinte(0x5A78FF, 0.95))
        k.fillEllipse(in: CGRect(x: 150, y: barreR.minY - 170, width: 724, height: 420))
        k.setFillColor(teinte(0xB9CBFF))
        k.fillEllipse(in: CGRect(x: 250, y: barreR.minY - 60, width: 524, height: 130))
    }
    // Lumiere chaude sur le bureau, de part et d'autre du pied.
    c.saveGState()
    c.setBlendMode(.screen)
    tache(c, pt(512, ecranR.maxY + (petit ? 80 : 62)), rx: 420, ry: petit ? 150 : 120, 0xFFB45C, 0.95)
    c.restoreGState()
    calque(c, taille, flou: 26, alpha: 0.7, mode: .screen) { k in
        let f = CGMutablePath()
        f.move(to: pt(barreR.minX + 20, barreR.maxY))
        f.addLine(to: pt(barreR.maxX - 20, barreR.maxY))
        f.addLine(to: pt(930, ecranR.maxY + 120))
        f.addLine(to: pt(94, ecranR.maxY + 120))
        f.closeSubpath()
        remplir(k, f, degrade([(0, teinte(0xFFCB8A, 0.5)), (1, teinte(0xFFB060, 0.3))]),
                de: pt(512, barreR.maxY), a: pt(512, ecranR.maxY + 120))
    }

    ecran(c, ecranR, petit: petit, coque: (0x232840, 0x0E101A), dalle: (0x1E2750, 0x0B0F22), pied: (0x1A1E2D, 0x323850))

    if petit {
        // En petit, un filet clair detache l'ecran du fond et la courbe tient en un trait.
        c.saveGState()
        c.addPath(pastille(ecranR.insetBy(dx: pixels(0.5, taille), dy: pixels(0.5, taille)), rayon: 30))
        c.setLineWidth(pixels(1, taille))
        c.setStrokeColor(teinte(0x6C7BC4, 0.75))
        c.strokePath()
        if taille >= 32 {
            let d = ecranR.insetBy(dx: 80, dy: 70)
            let courbe = courbeLissee([pt(d.minX, d.maxY - 40), pt(d.midX - 60, d.maxY - 60), pt(d.midX + 40, d.minY + 70),
                                       pt(d.maxX, d.minY + 30)])
            c.addPath(courbe)
            c.setLineWidth(pixels(1.4, taille))
            c.setLineCap(.round)
            c.setLineJoin(.round)
            c.setStrokeColor(teinte(0x6EF0CF))
            c.strokePath()
        }
        c.restoreGState()
    } else {
        // Courbe de veille sur la dalle : le pont surveille.
        let d = ecranR.insetBy(dx: 14, dy: 14)
        let valeurs: [CGFloat] = [0.66, 0.62, 0.68, 0.52, 0.58, 0.40, 0.47, 0.36, 0.42]
        let points = valeurs.enumerated().map { i, v in
            pt(d.minX + 48 + (d.width - 96) * CGFloat(i) / CGFloat(valeurs.count - 1), d.minY + d.height * v)
        }
        let courbe = courbeLissee(points)
        let aire = courbe.mutableCopy()!
        aire.addLine(to: pt(points.last!.x, d.maxY))
        aire.addLine(to: pt(points[0].x, d.maxY))
        aire.closeSubpath()
        remplir(c, aire, degrade([(0, teinte(0x5EE6C4, 0.22)), (1, teinte(0x5EE6C4, 0))]),
                de: pt(512, d.minY + d.height * 0.35), a: pt(512, d.maxY))
        c.saveGState()
        c.addPath(courbe)
        c.setLineWidth(8)
        c.setLineJoin(.round)
        c.setLineCap(.round)
        c.setStrokeColor(teinte(0x6EF0CF, 0.9))
        c.strokePath()
        c.restoreGState()
        let fin = points.last!
        calque(c, taille, flou: 10, mode: .screen) { k in
            k.setFillColor(teinte(0x6EF0CF))
            k.fillEllipse(in: CGRect(x: fin.x - 22, y: fin.y - 22, width: 44, height: 44))
        }
        c.setFillColor(teinte(0xE8FFF8))
        c.fillEllipse(in: CGRect(x: fin.x - 11, y: fin.y - 11, width: 22, height: 22))
    }

    barre(c, taille, barreR, lueur: petit ? 1 : 0.85)
    if !petit {
        // Pince de fixation, sous la barre.
        remplir(c, pastille(CGRect(x: 484, y: barreR.maxY - 2, width: 56, height: 22), rayon: 5),
                degrade([(0, teinte(0x4A5063)), (1, teinte(0x1C1F2B))]), de: pt(512, barreR.maxY), a: pt(512, barreR.maxY + 20))
    }
    liseres(c, clair: 0.22, sombre: 0.35)
}

// MARK: - Piste 2 : Ondes (barre, faisceau et halo en arcs)

func ondes(_ c: CGContext, _ taille: Int) {
    let petit = taille <= 64
    remplir(c, corps, degrade([(0, teinte(0x323744)), (0.55, teinte(0x1A1C23)), (1, teinte(0x0B0C10))]),
            de: pt(512, 100), a: pt(512, 924))

    let barreR = petit ? CGRect(x: 232, y: 486, width: 560, height: 80) : CGRect(x: 262, y: 492, width: 500, height: 60)
    let o = pt(512, barreR.midY)

    // Faisceau chaud vers le bureau.
    calque(c, taille, flou: petit ? 10 : 16, mode: .screen) { k in
        let f = CGMutablePath()
        f.move(to: pt(barreR.minX + 24, barreR.midY))
        f.addLine(to: pt(barreR.maxX - 24, barreR.midY))
        f.addLine(to: pt(880, 930))
        f.addLine(to: pt(144, 930))
        f.closeSubpath()
        remplir(k, f, degrade([(0, teinte(0xFFC984, 0.95)), (0.5, teinte(0xFF9E4F, 0.38)), (1, teinte(0xFF8A3D, 0))]),
                de: pt(512, barreR.midY), a: pt(512, 900))
    }

    // Halo : trois arcs froids au-dessus de la barre (lumiere et liaison radio).
    let rayons: [CGFloat] = petit ? [150, 265] : [140, 225, 310]
    let epaisseur: CGFloat = petit ? 62 : 38
    let teintes: [UInt32] = petit ? [0x8FE6FF, 0x9C8CFF] : [0x8FE6FF, 0x86B4FF, 0x9C8CFF]
    calque(c, taille, flou: 26, alpha: 0.9, mode: .screen) { k in
        for (r, t) in zip(rayons, teintes) {
            k.addPath(arc(o, rayon: r, de: 32, a: 148))
            k.setLineWidth(epaisseur * 1.6)
            k.setLineCap(.round)
            k.setStrokeColor(teinte(t, 0.8))
            k.strokePath()
        }
    }
    for (r, t) in zip(rayons, teintes) {
        c.saveGState()
        c.addPath(arc(o, rayon: r, de: 32, a: 148))
        c.setLineWidth(epaisseur)
        c.setLineCap(.round)
        c.replacePathWithStrokedPath()
        c.clip()
        c.drawLinearGradient(degrade([(0, teinte(0xFFFFFF)), (0.6, teinte(t)), (1, teinte(t, 0.85))]),
                             start: pt(512, o.y - r - epaisseur), end: pt(512, o.y - r * 0.4), options: [])
        c.restoreGState()
    }

    barre(c, taille, barreR)
    liseres(c, clair: 0.2, sombre: 0.3)
}

// MARK: - Piste 3 : Pont (fond clair, pastille de liaison)

func pont(_ c: CGContext, _ taille: Int) {
    let petit = taille <= 64
    remplir(c, corps, degrade([(0, teinte(0xFDFDFF)), (1, teinte(0xDCE1EC))]), de: pt(512, 100), a: pt(512, 924))

    let barreR = petit ? CGRect(x: 262, y: 300, width: 500, height: 64) : CGRect(x: 312, y: 318, width: 400, height: 38)
    let ecranR = petit ? CGRect(x: 172, y: barreR.maxY + 6, width: 680, height: 390)
                       : CGRect(x: 202, y: barreR.maxY + 2, width: 620, height: 350)

    // Halo violet sur le mur, autour de l'ecran.
    calque(c, taille, flou: petit ? 40 : 54, alpha: 0.85) { k in
        k.addPath(pastille(ecranR.insetBy(dx: -30, dy: -20).offsetBy(dx: 0, dy: -30), rayon: 60))
        k.setFillColor(teinte(0x7B6BFF, 0.75))
        k.fillPath()
    }
    // Lumiere chaude sur le bureau.
    c.saveGState()
    c.setBlendMode(.multiply)
    tache(c, pt(512, ecranR.maxY + 70), rx: 400, ry: 110, 0xFFB86B, 0.8)
    c.restoreGState()
    ecran(c, ecranR, petit: petit, coque: (0x30333F, 0x16181F), dalle: (0x2A3A6E, 0x0F1430), pied: (0xB9BFCC, 0xE9ECF2))
    // Reflet chaud de la barre en haut de la dalle.
    let d = ecranR.insetBy(dx: petit ? 22 : 14, dy: petit ? 22 : 14)
    c.saveGState()
    c.addPath(pastille(d, rayon: 8))
    c.clip()
    c.setBlendMode(.screen)
    tache(c, pt(512, d.minY), rx: d.width * 0.45, ry: 90, 0xFFC27A, 0.55)
    c.restoreGState()

    barre(c, taille, barreR)

    // Pastille de liaison : le pont radio qui veille.
    let ctr = petit ? pt(730, 730) : pt(748, 748)
    let rb: CGFloat = petit ? 150 : 116
    c.saveGState()
    c.setShadow(offset: CGSize(width: 0, height: -8 * CGFloat(taille) / 1024), blur: 24 * CGFloat(taille) / 1024,
                color: teinte(0x000000, 0.3))
    c.setFillColor(teinte(0xFFFFFF))
    c.fillEllipse(in: CGRect(x: ctr.x - rb, y: ctr.y - rb, width: 2 * rb, height: 2 * rb))
    c.restoreGState()
    let ri = rb - (petit ? 20 : 16)
    remplir(c, CGPath(ellipseIn: CGRect(x: ctr.x - ri, y: ctr.y - ri, width: 2 * ri, height: 2 * ri), transform: nil),
            degrade([(0, teinte(0x4BE3A0)), (1, teinte(0x14A06A))]), de: pt(ctr.x, ctr.y - ri), a: pt(ctr.x, ctr.y + ri))
    c.saveGState()
    c.setStrokeColor(teinte(0xFFFFFF))
    c.setLineCap(.round)
    let e: CGFloat = petit ? 26 : 15
    c.setLineWidth(e)
    let pas: [CGFloat] = taille <= 20 ? [] : petit ? [58] : [38, 66]
    for r in pas {
        c.addPath(arc(ctr, rayon: r, de: -42, a: 42))
        c.addPath(arc(ctr, rayon: r, de: 138, a: 222))
    }
    if !pas.isEmpty { c.strokePath() }
    let rp: CGFloat = taille <= 20 ? 44 : petit ? 22 : 16
    c.setFillColor(teinte(0xFFFFFF))
    c.fillEllipse(in: CGRect(x: ctr.x - rp, y: ctr.y - rp, width: 2 * rp, height: 2 * rp))
    c.restoreGState()

    liseres(c, clair: 0.5, sombre: 0.12)
}

// MARK: - Rendu et sorties

let pistes: [(nom: String, dessin: (CGContext, Int) -> Void)] = [
    ("Veille", veille), ("Ondes", ondes), ("Pont", pont),
]
/// Piste installee comme icone de l'app (indice dans `pistes`).
let retenue = 0

func icone(_ piste: Int, _ taille: Int) -> CGImage {
    let c = contexteIcone(taille)
    let s = CGFloat(taille) / 1024
    c.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 22 * s, color: teinte(0x000000, 0.32))
    c.beginTransparencyLayer(auxiliaryInfo: nil)
    c.saveGState()
    c.addPath(corps)
    c.clip()
    pistes[piste].dessin(c, taille)
    c.restoreGState()
    c.endTransparencyLayer()
    return c.makeImage()!
}

func ecrirePNG(_ image: CGImage, _ url: URL) {
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { fatalError("ecriture impossible : \(url.path)") }
}

func texte(_ c: CGContext, _ chaine: String, _ x: CGFloat, _ y: CGFloat, corps: CGFloat, _ couleur: CGColor) {
    let police = CTFontCreateUIFontForLanguage(.system, corps, nil)!
    let attributs = [kCTFontAttributeName: police, kCTForegroundColorAttributeName: couleur] as CFDictionary
    let ligne = CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, chaine as CFString, attributs)!)
    c.textPosition = pt(x, y)
    CTLineDraw(ligne, c)
}

/// Planche : chaque piste a 1024, 128, 32 et 16 px (taille reelle), fond clair
/// puis sombre ; sous les petites tailles, 32 et 16 px grossies sans lissage.
func planche(_ rendus: [[Int: CGImage]]) -> CGImage {
    let marge: CGFloat = 56, ecart: CGFloat = 56, entete: CGFloat = 110, legende: CGFloat = 70
    let colonne = max(128 + 32 + 16 + ecart * 2, 128 * 2 + ecart)
    let panneau = marge * 2 + 1024 + ecart + colonne
    let rangee = legende + 1024 + ecart
    let largeur = Int(panneau * 2), hauteur = Int(entete + rangee * CGFloat(rendus.count) + marge)
    let c = contexte(largeur, hauteur)
    c.interpolationQuality = .none
    let H = CGFloat(hauteur)
    // Place `image` (cote `cote`, origine en haut a gauche) et sa legende dessous.
    func poser(_ image: CGImage, _ x: CGFloat, _ haut: CGFloat, _ cote: CGFloat, _ nom: String?, _ encre: CGColor) {
        c.draw(image, in: CGRect(x: x, y: H - haut - cote, width: cote, height: cote))
        if let nom { texte(c, nom, x, H - haut - cote - 30, corps: 18, encre.copy(alpha: 0.6)!) }
    }
    for (p, (fond, encre)) in [(teinte(0xF2F2F5), teinte(0x1D1D22)), (teinte(0x1C1C21), teinte(0xEDEDF2))].enumerated() {
        let x0 = panneau * CGFloat(p)
        c.setFillColor(fond)
        c.fill(CGRect(x: x0, y: 0, width: panneau, height: H))
        texte(c, p == 0 ? "Fond clair" : "Fond sombre", x0 + marge, H - 72, corps: 34, encre)
        for (i, rendu) in rendus.enumerated() {
            let haut = entete + rangee * CGFloat(i)
            let titre = "\(i + 1). \(pistes[i].nom)" + (i == retenue ? "  (retenue)" : "")
            texte(c, titre, x0 + marge, H - haut - 40, corps: 28, encre)
            let y = haut + legende
            poser(rendu[1024]!, x0 + marge, y, 1024, nil, encre)
            let xc = x0 + marge + 1024 + ecart
            poser(rendu[128]!, xc, y + 260, 128, "128 px", encre)
            poser(rendu[32]!, xc + 128 + ecart, y + 260 + 48, 32, "32", encre)
            poser(rendu[16]!, xc + 128 + 32 + ecart * 2, y + 260 + 56, 16, "16", encre)
            poser(rendu[32]!, xc, y + 600, 128, "32 px x4", encre)
            poser(rendu[16]!, xc + 128 + ecart, y + 600, 128, "16 px x8", encre)
        }
    }
    return c.makeImage()!
}

func genererPistes(_ dossier: URL) throws {
    try FileManager.default.createDirectory(at: dossier, withIntermediateDirectories: true)
    var rendus: [[Int: CGImage]] = []
    for i in pistes.indices {
        var r: [Int: CGImage] = [:]
        for t in [1024, 128, 32, 16] { r[t] = icone(i, t) }
        ecrirePNG(r[1024]!, dossier.appendingPathComponent("candidate-\(i + 1).png"))
        rendus.append(r)
    }
    ecrirePNG(planche(rendus), dossier.appendingPathComponent("planche.png"))
    print("pistes ecrites dans \(dossier.path)")
}

func genererJeu(_ dossier: URL) throws {
    try FileManager.default.createDirectory(at: dossier, withIntermediateDirectories: true)
    var images: [[String: String]] = []
    for point in [16, 32, 128, 256, 512] {
        for echelle in [1, 2] {
            let nom = "icon_\(point)x\(point)\(echelle == 2 ? "@2x" : "").png"
            ecrirePNG(icone(retenue, point * echelle), dossier.appendingPathComponent(nom))
            images.append(["filename": nom, "idiom": "mac", "scale": "\(echelle)x", "size": "\(point)x\(point)"])
        }
    }
    let contenu: [String: Any] = ["images": images, "info": ["author": "xcode", "version": 1]]
    let json = try JSONSerialization.data(withJSONObject: contenu, options: [.prettyPrinted, .sortedKeys])
    try json.write(to: dossier.appendingPathComponent("Contents.json"))
    print("icone \(pistes[retenue].nom) ecrite dans \(dossier.path)")
}

let ici = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let arguments = CommandLine.arguments
if let i = arguments.firstIndex(of: "--pistes") {
    let dossier = i + 1 < arguments.count ? arguments[i + 1] : "/private/tmp/halo-icons"
    try genererPistes(URL(fileURLWithPath: dossier))
} else {
    let catalogue = ici.appendingPathComponent("../HaloCompagnon/Ressources/Assets.xcassets").standardizedFileURL
    try FileManager.default.createDirectory(at: catalogue, withIntermediateDirectories: true)
    let info = try JSONSerialization.data(withJSONObject: ["info": ["author": "xcode", "version": 1]],
                                          options: [.prettyPrinted, .sortedKeys])
    try info.write(to: catalogue.appendingPathComponent("Contents.json"))
    try genererJeu(catalogue.appendingPathComponent("AppIcon.appiconset"))
}
