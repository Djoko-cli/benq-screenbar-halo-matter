#!/usr/bin/env python3
"""Traductions d'un catalogue .xcstrings, au format exact de Xcode.

  python3 Outils/traduire.py <catalogue.xcstrings> <traductions.json>

traductions.json : {"<cle francaise>": "<anglais>", ...}. Chaque cle recoit
son anglais et son francais (la cle elle-meme), en specificateurs numerotes
(%1$@, %2$lld...) des qu'il y en a deux et qu'aucun ne l'est deja. Les cles
perimees laissees par `xcstringstool sync` (extractionState "stale") sont
retirees : les tests de LocalisationTests les refusent.

Numerotation : chaque langue est numerotee selon l'ordre d'apparition de ses
propres specificateurs, ce qui suppose que l'anglais garde l'ordre des
valeurs du francais. Si l'anglais permute les valeurs, numeroter soi-meme
les specificateurs dans la traduction de traductions.json (ex : "%2$@ ...
%1$@" pour les inverser) : l'outil ne renumerote jamais une chaine qui l'est
deja.

Pluriel : si une cle ciblee existe deja dans le catalogue avec des
`variations` (pluriel) sur au moins une de ses localisations, l'outil
s'arrete (code de sortie non nul) sans rien ecrire : ca se traduit a la main
dans Xcode, pas avec cet outil.
"""
import json
import re
import sys

SPEC = re.compile(r"%(?:\d+\$)?(lld|ld|d|@|lf|f)")


def numeroter(s):
    if re.search(r"%\d+\$", s) or len(SPEC.findall(s)) < 2:
        return s
    rang = iter(range(1, 100))
    return SPEC.sub(lambda m: f"%{next(rang)}${m.group(1)}", s)


def unite(valeur):
    return {"stringUnit": {"state": "translated", "value": valeur}}


def est_plurielle(entree):
    """Une localisation existante de la cle porte des `variations` (pluriel)."""
    return any("variations" in loc for loc in entree.get("localizations", {}).values())


def main(chemin, fichier):
    with open(chemin, encoding="utf-8") as f:
        d = json.load(f)
    with open(fichier, encoding="utf-8") as f:
        traductions = json.load(f)
    cles = d["strings"]
    # Valide tout traductions.json avant la moindre ecriture (rien de partiel).
    pluriels = sorted(c for c in traductions if c in cles and est_plurielle(cles[c]))
    if pluriels:
        sys.exit("pluriel : a traduire a la main dans Xcode : " + ", ".join(pluriels))
    for k in [k for k, e in cles.items() if e.get("extractionState") == "stale"]:
        del cles[k]
    for cle, anglais in traductions.items():
        locs = cles.setdefault(cle, {}).setdefault("localizations", {})
        locs["fr"] = unite(numeroter(cle))
        locs["en"] = unite(numeroter(anglais))
    texte = json.dumps(d, ensure_ascii=False, indent=2, separators=(",", " : "), sort_keys=True)
    with open(chemin, "w", encoding="utf-8") as f:
        f.write(texte + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
