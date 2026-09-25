#!/usr/bin/env python3
"""Tests de tools/halo_udp.py sans reseau ni trousseau reel : python3 tools/test_halo_udp.py"""
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
    "acct"<blob>="561F9A6463953778"
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
        self.assertEqual(halo_udp.nom_du_pont("561F9A6463953778.local"), "561F9A6463953778")
        self.assertIsNone(halo_udp.nom_du_pont("fd77:9e:f4bb::1"))

    def test_comptes_du_trousseau(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(DUMP)):
            self.assertEqual(halo_udp.comptes_du_trousseau(), ["561F9A6463953778"])

    def test_cle_du_trousseau(self):
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat(CLE.hex().upper() + "\n")) as run:
            self.assertEqual(halo_udp.cle_du_trousseau("561F9A6463953778"), CLE)
            self.assertIn("-a", run.call_args[0][0])
        with mock.patch.object(halo_udp.subprocess, "run", return_value=resultat("", 44)):
            self.assertIsNone(halo_udp.cle_du_trousseau("561F9A6463953778"))

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
                self.assertEqual(halo_udp.load_key("561F9A6463953778.local"), bytes(range(1, 33)))
                trousseau.assert_not_called()
            # Sans HALO_CLE : le trousseau passe avant le fichier.
            env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=CLE):
                self.assertEqual(halo_udp.load_key("561F9A6463953778.local"), CLE)
            # Rien dans le trousseau : le fichier.
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(halo_udp, "KEY_PATH", fichier), \
                    mock.patch.object(halo_udp, "cle_du_trousseau", return_value=None):
                self.assertEqual(halo_udp.load_key("561F9A6463953778.local"), bytes(range(1, 33)))

    def test_cle_exige_halo_cle(self):
        env = {k: v for k, v in os.environ.items() if k != "HALO_CLE"}
        with mock.patch.dict(os.environ, env, clear=True):
            with self.assertRaises(SystemExit) as e:
                halo_udp.cmd_cle("/dev/cu.inexistant")
            self.assertIn("app", str(e.exception))


if __name__ == "__main__":
    unittest.main()
