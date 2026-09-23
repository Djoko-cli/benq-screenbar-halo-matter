#!/usr/bin/env python3
# Revision git du firmware, pour les drapeaux dynamiques de PlatformIO :
#     build_src_flags = !python3 tools/git_rev.py
# Imprime -DFW_GIT_REV="<hash court>", suivi de "-dirty" si un fichier suivi
# differe du commit (les fichiers non suivis ne comptent pas). Sans git, ou hors
# d'un depot : "nogit". La valeur finit dans esp_app_desc.version (src/app_desc.c),
# que Matter rapporte comme SoftwareVersionString ("Programme interne" dans Apple
# Home) : 31 caracteres au plus avec FW_VERSION, verifie a la compilation.
import os
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args):
    out = subprocess.check_output(("git",) + args, cwd=ROOT, stderr=subprocess.DEVNULL)
    return out.decode("utf-8", "replace").strip()


try:
    # 12 au plus : git allonge le hash court s'il devient ambigu.
    rev = git("rev-parse", "--short=7", "HEAD")[:12]
    if git("status", "--porcelain", "--untracked-files=no"):
        rev += "-dirty"
except (OSError, subprocess.CalledProcessError):
    rev = "nogit"

print("-DFW_GIT_REV='\"%s\"'" % rev)
