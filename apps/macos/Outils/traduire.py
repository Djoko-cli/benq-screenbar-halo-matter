#!/usr/bin/env python3
"""Traductions d'un catalogue .xcstrings, au format exact de Xcode.

  python3 Outils/traduire.py <catalogue.xcstrings> <traductions.json>

traductions.json : {"<cle francaise>": "<anglais>", ...}. Chaque cle recoit
son anglais et son francais (la cle elle-meme), en specificateurs numerotes
(%1$@, %2$lld...) des qu'il y en a deux et qu'aucun ne l'est deja. Les cles
perimees laissees par `xcstringstool sync` (extractionState "stale") sont
retirees : les tests de LocalisationTests les refusent.
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


def main(chemin, fichier):
    with open(chemin, encoding="utf-8") as f:
        d = json.load(f)
    with open(fichier, encoding="utf-8") as f:
        traductions = json.load(f)
    cles = d["strings"]
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
