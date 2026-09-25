#!/usr/bin/env python3
"""Tests de tools/halo_udp.py sans reseau ni trousseau reel : python3 tools/test_halo_udp.py"""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import halo_udp  # noqa: E402

CLE = bytes(range(32))
DUMP = '''keychain: "/Users/x/Library/Keychains/login.keychain-db"
version: 512
class: "genp"
attributes:
    "acct"<blob>="56B1E064401F74EF"
    "svce"<blob>="fr.djoko.halo.pont"
keychain: "/Users/x/Library/Keychains/login.keychain-db"
class: "genp"
attributes:
    "acct"<blob>="autre"
    "svce"<blob>="com.exemple"
'''

# Formats including hex with quoted text and bare hex
DUMP_HEX = '''keychain: "/Users/x/Library/Keychains/login.keychain-db"
class: "genp"
attributes:
    "acct"<blob>=0x56B1E064401F74EF  "56B1E064401F74EF"
    "svce"<blob>="fr.djoko.halo.pont"
keychain: "/Users/x/Library/Keychains/login.keychain-db"
class: "genp"
attributes:
    "acct"<blob>=0xAABBCCDD
    "svce"<blob>="fr.djoko.halo.pont"
keychain: "/Users/x/Library/Keychains/login.keychain-db"
class: "genp"
attributes:
    "acct"<blob>="autre"
    "svce"<blob>="com.exemple"
'''


def resultat(sortie, code=0):
    return subprocess.CompletedProcess([], code, stdout=sortie, stderr="")


class Cle(unittest.TestCase):
    def test_nom_du_pont(self):
        self.assertEqual(halo_udp.nom_du_pont("56B1E064401F74EF.local"), "56B1E064401F74EF")
        self.assertIsNone(halo_udp.nom_du_pont("fd4f:9c:ed42::1"))

    def test_comptes_du_trousseau(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(DUMP)):
            self.assertEqual(halo_udp.comptes_du_trousseau(), ["56B1E064401F74EF"])

    def test_comptes_du_trousseau_hex(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(DUMP_HEX)):
            # 0xAABBCCDD is not valid UTF-8, so it gets replaced with U+FFFD
            self.assertEqual(halo_udp.comptes_du_trousseau(), ["56B1E064401F74EF", "����"])

    def test_cle_du_trousseau(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(CLE.hex().upper() + "\n")) as run:
            self.assertEqual(halo_udp.cle_du_trousseau("56B1E064401F74EF"), CLE)
            self.assertIn("-a", run.call_args[0][0])
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat("", 44)):
            self.assertIsNone(halo_udp.cle_du_trousseau("56B1E064401F74EF"))

    def test_plusieurs_ponts_sans_nom(self):
        with mock.patch.object(halo_udp, "comptes_du_trousseau", return_value=["A", "B"]):
            with self.assertRaises(SystemExit):
                halo_udp.cle_du_trousseau(None)

    def test_ordre(self):
        with tempfile.TemporaryDirectory() as d:
            fichier = os.path.join(d, "cle")
            with open(fichier, "w") as f:
                f.write(bytes(range(1, 33)).hex().upper() + "\n")
            # HALO_CLE d'abord : le trousseau n'est pas consulte.
            with mock.patch.dict(os.environ, {"HALO_CLE": fichier}), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau") as trousseau:
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), bytes(range(1, 33)))
                trousseau.assert_not_called()
            # Sans HALO_CLE : le trousseau passe avant le fichier.
            env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=CLE):
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), CLE)
            # Rien dans le trousseau : le fichier (avertissement, voir test_repli_previent).
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=None), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), bytes(range(1, 33)))

    def test_repli_previent(self):
        cle = bytes(range(1, 33))
        env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
        with tempfile.TemporaryDirectory() as d:
            fichier = os.path.join(d, "cle")
            with open(fichier, "w") as f:
                f.write(cle.hex().upper() + "\n")
            # Trousseau sans cle (ou refus) : repli sur le fichier, avertissement sur stderr, jamais la cle.
            erreur, sortie = io.StringIO(), io.StringIO()
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=None), \
                    contextlib.redirect_stderr(erreur), contextlib.redirect_stdout(sortie):
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), cle)
            self.assertIn(fichier, erreur.getvalue())
            self.assertIn("perimee", erreur.getvalue())
            self.assertEqual(sortie.getvalue(), "")
            for texte in (erreur.getvalue(), sortie.getvalue()):
                self.assertNotIn(cle.hex().upper(), texte)
                self.assertNotIn(cle.hex(), texte)
            # Cle du trousseau : aucun avertissement.
            erreur = io.StringIO()
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=CLE), \
                    contextlib.redirect_stderr(erreur):
                self.assertEqual(halo_udp.load_key("56B1E064401F74EF.local"), CLE)
            self.assertEqual(erreur.getvalue(), "")

    def test_cle_exige_halo_cle(self):
        env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
        with mock.patch.dict(os.environ, env, clear=True):
            with self.assertRaises(SystemExit) as e:
                halo_udp.cmd_cle("/dev/cu.inexistant")
            self.assertIn("app", str(e.exception))


if __name__ == "__main__":
    unittest.main()
