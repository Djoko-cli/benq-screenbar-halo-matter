#!/usr/bin/env python3
# Revision git du firmware, pour les drapeaux dynamiques de PlatformIO :
#     build_src_flags = !python3 tools/git_rev.py
# Imprime -DFW_GIT_REV="<hash court>", suivi de "-dirty" si un fichier suivi
# differe du commit, ou si un fichier non suivi (et non ignore) se trouve la ou
# PlatformIO compile (src/, include/, lib/) : il finirait dans le firmware sans
# etre dans le commit. Les autres fichiers non suivis ne comptent pas. Sans git,
# ou hors d'un depot : "nogit". La valeur finit dans esp_app_desc.version
# (src/app_desc.c), que Matter rapporte comme SoftwareVersionString
# ("Programme interne" dans Apple Home) : 31 caracteres au plus avec
# FW_VERSION, verifie a la compilation.
#
# Lance a chaque 'pio run' (et quand l'IDE relit le projet) : --no-optional-locks
# evite que 'git status' prenne .git/index.lock pour rafraichir l'index, ce qui
# ferait echouer un 'git commit' ou 'git add' lance au meme moment.
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIRS = ("src", "include", "lib")


def git(*args):
    out = subprocess.check_output(("git", "--no-optional-locks") + args, cwd=ROOT, stderr=subprocess.DEVNULL)
    return out.decode("utf-8", "replace").strip()


try:
    # 12 au plus : git allonge le hash court s'il devient ambigu.
    rev = git("rev-parse", "--short=7", "HEAD")[:12]
except (OSError, subprocess.CalledProcessError):
    rev = "nogit"

if rev != "nogit":
    try:
        dirty = bool(git("status", "--porcelain", "--untracked-files=no")) or bool(
            git("ls-files", "--others", "--exclude-standard", "--", *BUILD_DIRS)
        )
    except (OSError, subprocess.CalledProcessError):
        dirty = True  # etat de l'arbre illisible : on ne peut pas le dire propre
    if dirty:
        rev += "-dirty"

print("-DFW_GIT_REV='\"%s\"'" % rev)
