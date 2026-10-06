"""Tests de publication.py : les numeros, les notes, le flux appcast.xml a partir de valeurs inventees (nouveau, ou
une version de plus en tete d'un flux qui les garde toutes, dans l'ordre des versions), les etiquettes propres a l'app,
les controles avant publication et juste avant les gestes publics, leur ordre, le commit du flux et son auteur, la
compilation sans chemin personnel, la signature du code et son certificat, le contenu du .dmg (licence de Sparkle
comprise), la notarisation (desactivee par defaut), sur un faux depot et de fausses commandes (xcodebuild, codesign,
security, openssl, ditto, hdiutil, sign_update, xcrun, gh...). Aucun reseau, aucun trousseau, aucun outil reel hors
git ; HOME est un dossier temporaire, et le nom du compte, invente (USER, LOGNAME).

  /usr/bin/python3 -m unittest discover -s <dossier de ces tests>
"""
import contextlib
import datetime
import io
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
import xml.etree.ElementTree as ET
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import publication as P  # noqa: E402

CLE = 'p7jaASHZk/U9YboWWSgw+Z4xEGYXOfUUhTKtZG7uQlE='
IDENTITE = 'Essai Inventee Signing'
EMPREINTE = 'A1B2C3D4E5F60718293A4B5C6D7E8F9012345678'
AUTRE_EMPREINTE = '0F1E2D3C4B5A69788796A5B4C3D2E1F00F1E2D3C'
COMPTE = 'compte-invente'
AUTRE_CLE = 'lkPxEHj5erw+omLlr1AVsIoyhfz4YnoLa/N9147SNgc='
GIT_ENV = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null',
               GIT_AUTHOR_NAME='Essai', GIT_AUTHOR_EMAIL='essai@example.invalid',
               GIT_COMMITTER_NAME='Essai', GIT_COMMITTER_EMAIL='essai@example.invalid')


def projet(chemin_flux='appcast.xml', systeme='26.0'):
    return textwrap.dedent('''\
        name: Essai
        options:
          bundleIdPrefix: fr.exemple
          deploymentTarget:
            macOS: "%s"
        targets:
          Compagnon:
            type: application
            settings:
              base:
                MARKETING_VERSION: "1.0"
          Essai:
            type: application
            settings:
              base:
                PRODUCT_NAME: Essai Inventee
                MARKETING_VERSION: "1.2.3"
                CURRENT_PROJECT_VERSION: "1"
                FLUX_MISES_A_JOUR: https://raw.githubusercontent.com/Exemple/essai/main/%s
                CLE_MISES_A_JOUR: %s
        ''' % (systeme, chemin_flux, CLE))


PROJET = projet()

NOTES = textwrap.dedent('''\
    # Notes de version · Release notes

    ## 1.2.3

    **Français**

    - Une `commande` <nouvelle>,
      sur deux lignes.

    **English**

    - A new `command`.

    ## 1.2.2

    - Ancienne.
    ''')

# Les fausses commandes : elles notent leurs arguments dans FAUX_JOURNAL.
FAUX = {
    'xcodegen': 'import sys\n',
    'codesign': textwrap.dedent('''\
        a = sys.argv[1:]
        if a[:2] == ["-d", "-r-"]:
            print('designated => identifier "fr.exemple.essai" and certificate leaf = H"0123abcd"')
        extraire = a[:1] == ["-d"] and a[1].startswith("--extract-certificates=")
        if extraire and not os.environ.get("FAUX_SANS_CERTIFICAT"):
            open(a[1].split("=", 1)[1] + "0", "wb").write(b"FEUILLE")
        '''),
    'security': textwrap.dedent('''\
        a = sys.argv[1:]
        nom = os.environ.get("FAUSSE_IDENTITE", "%(identite)s")
        if a[0] == "find-identity":
            ligne = '  1) %(empreinte)s "%%s" (CSSMERR_TP_NOT_TRUSTED)' %% nom
            print("Policy: Code Signing\\n  Matching identities\\n" + ligne)
            if os.environ.get("FAUSSE_IDENTITE_DOUBLE"):
                print('  2) %(autre)s "%%s"' %% nom)
            print("     identities found\\n\\nValid identities only\\n" + ligne + "\\n     1 valid identities found")
        elif a[0] == "find-certificate":
            sha1 = "%(autre)s" if os.environ.get("FAUX_CERTIFICAT_ABSENT") else "%(empreinte)s"
            print("SHA-256 hash: " + "AB" * 32 + "\\nSHA-1 hash: " + sha1)
            print("-----BEGIN CERTIFICATE-----\\nRkFVWA==\\n-----END CERTIFICATE-----")
        ''' % {'identite': IDENTITE, 'empreinte': EMPREINTE, 'autre': AUTRE_EMPREINTE}),
    'openssl': textwrap.dedent('''\
        a = sys.argv[1:]
        feuille = "-in" in a
        sujet = os.environ.get("FAUX_SUJET_FEUILLE" if feuille else "FAUX_SUJET", "CN=%(identite)s")
        empreinte = os.environ.get("FAUSSE_EMPREINTE_FEUILLE", "%(empreinte)s") if feuille else "%(empreinte)s"
        if "-text" in a:
            print("Certificate:\\n    Data:\\n        Subject: " + sujet.replace(",", ", "))
            if os.environ.get("FAUX_SAN"):
                print("        X509v3 Subject Alternative Name:\\n            DNS:exemple.invalid")
            if os.environ.get("FAUX_EMETTEUR"):
                print("        Issuer: CN=Autre, emailAddress=x@exemple.invalid")
        else:
            print("subject= " + sujet)
            print("SHA1 Fingerprint=" + ":".join(empreinte[i:i + 2] for i in range(0, 40, 2)))
        ''' % {'identite': IDENTITE, 'empreinte': EMPREINTE}),
    'xcrun': 'import sys\n',
    'spctl': 'import sys\n',
    'ditto': 'import shutil\nshutil.copytree(sys.argv[1], sys.argv[2], symlinks=True)\n',
    'xcodebuild': textwrap.dedent('''\
        import plistlib, re
        a = sys.argv[1:]
        dd = a[a.index('-derivedDataPath') + 1]
        reglages = dict(x.split('=', 1) for x in a if '=' in x and not x.startswith('-'))
        projet = open('project.yml').read()
        def lu(nom):
            return reglages.get(nom) or re.search(r'  Essai:\\n(?:.*\\n)*?\\s+%s: "?([^"\\n]+)"?' % nom, projet).group(1)
        def fichier(chemin, contenu, mode=0o644):
            os.makedirs(os.path.dirname(chemin), exist_ok=True)
            open(chemin, 'wb').write(contenu)
            os.chmod(chemin, mode)
        app = os.path.join(dd, 'Build', 'Products', 'Release', 'Essai Inventee.app', 'Contents')
        os.makedirs(app, exist_ok=True)
        # Le code imbrique, comme celui de Sparkle : deux services XPC, une app, un executable, puis un autre cadre.
        b = os.path.join(app, 'Frameworks', 'Sparkle.framework', 'Versions', 'B')
        for d in ('XPCServices/Installer.xpc/Contents', 'XPCServices/Downloader.xpc/Contents', 'Updater.app/Contents'):
            os.makedirs(os.path.join(b, d), exist_ok=True)
        # Un chemin personnel injecte (FAUX_CHEMIN = quoi:ou), comme la table OSO ou un #filePath.
        perso = {'home': os.environ['HOME'] + '/Library/Essai/x.o', 'users': '/Users/autre/x.o',
                 'compte': 'build-' + os.environ.get('USER', '') + '-x'}
        quoi, _, ou = os.environ.get('FAUX_CHEMIN', ':').partition(':')
        def contenu(ici, base):
            return base + (b'\\0' + perso[quoi].encode() + b'\\0' if ou == ici else b'')
        fichier(os.path.join(b, 'Autoupdate'), contenu('autoupdate', b'autoupdate invente'), 0o755)
        fichier(os.path.join(b, 'Sparkle'), contenu('sparkle', b'sparkle invente'), 0o755)
        if not os.path.lexists(os.path.join(b, '..', 'Current')):
            os.symlink('B', os.path.join(b, '..', 'Current'))
        os.makedirs(os.path.join(app, 'Frameworks', 'Coeur.framework', 'Versions', 'A'), exist_ok=True)
        fichier(os.path.join(app, 'MacOS', 'Essai Inventee'), contenu('app', b'binaire invente'), 0o755)
        fichier(os.path.join(app, 'Resources', 'fr.lproj', 'Localizable.strings'), contenu('ressource', b'"a" = "b";'))
        lien = os.path.join(app, 'Resources', 'lien')
        if os.path.lexists(lien):
            os.remove(lien)
        if os.environ.get('FAUX_LIEN'):
            os.symlink(perso['home'], lien)
        cle = AUTRE if os.environ.get('FAUX_INFO') == 'cle' else lu('CLE_MISES_A_JOUR')
        plistlib.dump({'CFBundleShortVersionString': lu('MARKETING_VERSION'),
                       'CFBundleVersion': reglages['CURRENT_PROJECT_VERSION'],
                       'SUFeedURL': lu('FLUX_MISES_A_JOUR'), 'SUPublicEDKey': cle},
                      open(os.path.join(app, 'Info.plist'), 'wb'))
        ''').replace('AUTRE', repr(AUTRE_CLE)),
    # Le .dmg simule : ses arguments, puis la liste du dossier source (avec la cible des liens).
    'hdiutil': textwrap.dedent('''\
        a = sys.argv[1:]
        src = a[a.index('-srcfolder') + 1]
        lignes = ['dmg invente ' + ' '.join(a)]
        for r, ds, fs in os.walk(src):
            for n in sorted(ds + fs):
                p = os.path.join(r, n)
                lignes.append(os.path.relpath(p, src) + (' -> ' + os.readlink(p) if os.path.islink(p) else ''))
        open(a[-1], 'w').write('\\n'.join(lignes) + '\\n')
        '''),
    'sign_update': 'print("U0lHTkFUVVJFLUlOVkVOVEVF")\n',
    'generate_keys': 'print(os.environ["FAUSSE_CLE"])\n',
    # GitHub simule : release view (« release not found », ou FAUX_VUE, ou publiee), auth status, et release create,
    # qui note l'etat de l'origine a ce moment, puis cree l'etiquette sur la cible, comme GitHub.
    'gh': textwrap.dedent('''\
        import subprocess
        a = sys.argv[1:]
        if a[:2] == ['release', 'view']:
            if os.environ.get('FAUX_PUBLIEE'):
                sys.exit(0)
            sys.stderr.write(os.environ.get('FAUX_VUE', 'release not found') + '\\n')
            sys.exit(1)
        if a[:2] == ['auth', 'status']:
            sys.exit(1 if os.environ.get('FAUX_AUTH_ECHEC') else 0)
        if a[:2] == ['release', 'create']:
            origine = os.environ['FAUSSE_ORIGINE']
            main = subprocess.run(['git', '-C', origine, 'rev-parse', 'main'], capture_output=True, text=True)
            open(os.environ['FAUX_JOURNAL'], 'a').write('origine au moment de la publication ' + main.stdout)
            if os.environ.get('FAUX_ECHEC_CREATE'):
                sys.exit(1)
            subprocess.run(['git', '-C', origine, 'tag', a[2], a[a.index('--target') + 1]], check=True)
        '''),
    # Le controle d'anonymisation simule : il trouve si un de ses arguments contient FAUX_TROUVE.
    'controles.py': textwrap.dedent('''\
        t = os.environ.get('FAUX_TROUVE')
        trouve = bool(t) and t in ' '.join(sys.argv[1:])
        print('trouve : ' + ('1' if trouve else 'aucun'))
        sys.exit(1 if trouve else 0)
        '''),
}


def lire(chemin):
    with open(chemin, encoding='utf-8') as f:
        return f.read()


def ecrire(chemin, texte):
    os.makedirs(os.path.dirname(chemin) or '.', exist_ok=True)
    with open(chemin, 'w', encoding='utf-8') as f:
        f.write(texte)


def git(depot, *args):
    return subprocess.run(['git', '-C', depot] + list(args), check=True, capture_output=True, text=True,
                          env=GIT_ENV).stdout.strip()


class Monde:
    """Un faux depot (et son origine), de fausses commandes, un dossier de produits, un dossier personnel. L'app est
    a la racine du depot (sous='.'), ou dans un sous-dossier, comme celle du pont (sous='apps/macos')."""

    def __init__(self, racine, sous='.', systeme='26.0'):
        self.racine = racine
        self.bin = os.path.join(racine, 'bin')
        self.journal = os.path.join(racine, 'journal.txt')
        os.makedirs(self.bin)
        for nom, corps in FAUX.items():
            chemin = os.path.join(self.bin, nom)
            ecrire(chemin, '#!/usr/bin/python3\nimport os, sys\n'
                           'open(os.environ["FAUX_JOURNAL"], "a").write(%r + " " + " ".join(sys.argv[1:]) + "\\n")\n'
                           % nom + corps)
            os.chmod(chemin, 0o755)
        ecrire(os.path.join(self.bin, 'table.json'), '{}\n')
        self.maison = os.path.join(racine, 'maison')
        os.makedirs(os.path.join(self.maison, 'Desktop'))
        self.licence = os.path.join(racine, 'licence', 'Sparkle-LICENSE.txt')
        ecrire(self.licence, 'Licence inventee de Sparkle, pour les tests.\n')
        self.origine = os.path.join(racine, 'origine.git')
        self.depot = os.path.join(racine, 'depot')
        self.sous = sous
        self.app = os.path.normpath(os.path.join(self.depot, sous))
        self.chemin_flux = os.path.normpath(os.path.join(sous, 'appcast.xml'))
        subprocess.run(['git', 'init', '-q', '--bare', '-b', 'main', self.origine], check=True, env=GIT_ENV)
        subprocess.run(['git', 'clone', '-q', self.origine, self.depot], check=True, env=GIT_ENV,
                       capture_output=True)
        git(self.depot, 'config', 'user.name', 'Essai')
        git(self.depot, 'config', 'user.email', '0+essai@users.noreply.github.com')
        git(self.depot, 'checkout', '-q', '-b', 'main')
        ecrire(os.path.join(self.app, 'project.yml'), projet(self.chemin_flux, systeme))
        ecrire(os.path.join(self.app, 'NOTES-VERSIONS.md'), NOTES)
        ecrire(os.path.join(self.depot, '.gitignore'), 'build/\n')
        for i in range(3):
            ecrire(os.path.join(self.depot, 'f%d.txt' % i), '%d\n' % i)
            git(self.depot, 'add', '-A')
            git(self.depot, 'commit', '-q', '-m', 'commit %d' % i)
        git(self.depot, 'push', '-q', 'origin', 'main')
        self.dd = os.path.join(racine, 'dd')
        self.dit = ''
        self.env = {'GIT': 'git', 'XCODEGEN': self.bin + '/xcodegen', 'XCODEBUILD': self.bin + '/xcodebuild',
                    'HDIUTIL': self.bin + '/hdiutil', 'DITTO': self.bin + '/ditto', 'CODESIGN': self.bin + '/codesign',
                    'GH': self.bin + '/gh', 'SECURITY': self.bin + '/security', 'OPENSSL': self.bin + '/openssl',
                    'XCRUN': self.bin + '/xcrun', 'SPCTL': self.bin + '/spctl', 'SPARKLE_BIN': self.bin,
                    'CONTROLE_ANONYMISATION': self.bin + '/controles.py',
                    'TABLE_ANONYMISATION': self.bin + '/table.json'}

    def appels(self):
        return lire(self.journal).splitlines() if os.path.exists(self.journal) else []

    def oublier(self):
        """Un journal neuf, pour un cas de plus dans le meme monde."""
        if os.path.exists(self.journal):
            os.remove(self.journal)

    def arguments(self, *extra, bureau=False):
        return ['publier', '1.2.3', '--nom-app', 'Essai Inventee', '--fichier', 'Essai-Inventee',
                '--depot-github', 'Exemple/essai', '--projet', 'Essai.xcodeproj', '--schema', 'Essai',
                '--cible', 'Essai', '--textes', 'project.yml', '--identite', IDENTITE, '--auteur', 'Essai',
                '--etiquette', 'essai-v', '--flux', 'appcast.xml', '--licence', self.licence] \
            + ([] if bureau else ['--sans-bureau']) + list(extra)

    def environnement(self, env):
        # Sans GIT_AUTHOR_* ni GIT_COMMITTER_* : le commit du flux prend l'auteur de la configuration du depot.
        e = {k: v for k, v in GIT_ENV.items() if not k.startswith(('GIT_AUTHOR', 'GIT_COMMITTER'))}
        e.update(FAUX_JOURNAL=self.journal, FAUSSE_CLE=CLE, DD=self.dd, FAUSSE_ORIGINE=self.origine,
                 HOME=self.maison, USER=COMPTE, LOGNAME=COMPTE)
        e.update(env or {})
        return e

    def publier(self, *extra, env=None, bureau=False):
        dedans = os.getcwd()
        os.chdir(self.app)
        sortie = io.StringIO()
        try:
            with mock.patch.dict(os.environ, self.environnement(env), clear=True), contextlib.redirect_stdout(sortie):
                return P.publier(P.arguments(self.arguments(*extra, bureau=bureau)), P.Outils(self.env),
                                 maintenant=datetime.datetime(2026, 10, 6, 12, 0, 0))
        finally:
            self.dit = sortie.getvalue()
            os.chdir(dedans)

    def main(self, *extra, env=None):
        """publication.main, les outils lus dans l'environnement : le code de sortie et ce qui va sur stderr."""
        dedans = os.getcwd()
        os.chdir(self.app)
        erreurs = io.StringIO()
        try:
            e = self.environnement(env)
            e.update(self.env)
            e.update(env or {})
            with mock.patch.dict(os.environ, e, clear=True), contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(erreurs):
                code = P.main(self.arguments(*extra))
        finally:
            os.chdir(dedans)
        return code, erreurs.getvalue()


class NumerosTests(unittest.TestCase):
    def test_version_valide(self):
        self.assertTrue(P.version_valide('1.0.0'))
        self.assertTrue(P.version_valide('12.30.4'))
        for v in ('1.0', 'v1.0.0', '1.0.0-beta', '1.0.0 ', ''):
            self.assertFalse(P.version_valide(v), v)

    def test_versions_comparees_en_nombres(self):
        self.assertGreater(P.nombres('1.10.0'), P.nombres('1.2.3'))
        self.assertGreater(P.nombres('2.0.0'), P.nombres('1.99.99'))
        self.assertEqual(P.nombres('1.0.0'), (1, 0, 0))

    def test_reglages_de_la_cible(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'project.yml')
            ecrire(p, PROJET)
            self.assertEqual(P.reglage(p, 'Essai', 'MARKETING_VERSION'), '1.2.3')
            self.assertEqual(P.reglage(p, 'Compagnon', 'MARKETING_VERSION'), '1.0', 'chaque cible la sienne')
            self.assertEqual(P.reglage(p, 'Essai', 'CLE_MISES_A_JOUR'), CLE)
            self.assertEqual(P.systeme_minimum(p), '26.0')
            with self.assertRaises(P.Refus):
                P.reglage(p, 'Absente', 'MARKETING_VERSION')
            with self.assertRaises(P.Refus):
                P.reglage(p, 'Compagnon', 'CLE_MISES_A_JOUR')

    def test_numero_de_compilation(self):
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(['git', 'init', '-q', d], check=True, env=GIT_ENV)
            for i in range(4):
                git(d, 'commit', '-q', '--allow-empty', '-m', str(i))
            self.assertEqual(P.numero_compilation(d), 4)


class NotesEtFluxTests(unittest.TestCase):
    def test_notes_de_la_version(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'NOTES-VERSIONS.md')
            ecrire(p, NOTES)
            n = P.notes(p, '1.2.3')
            self.assertTrue(n.startswith('**Français**'))
            self.assertNotIn('Ancienne', n)
            self.assertEqual(P.notes(p, '1.2.2'), '- Ancienne.\n')
            with self.assertRaises(P.Refus):
                P.notes(p, '9.9.9')

    def test_notes_html(self):
        h = P.notes_html('**Français**\n\n- Une `commande` <nouvelle>,\n  sur deux lignes.\n\n**English**\n\n- B\n')
        self.assertEqual(h, '<p><strong>Français</strong></p>\n'
                            '<ul><li>Une <code>commande</code> &lt;nouvelle&gt;, sur deux lignes.</li></ul>\n'
                            '<p><strong>English</strong></p>\n<ul><li>B</li></ul>')

    def item(self, version='1.2.3', numero=57):
        return P.item_flux(version, numero, 'https://exemple.invalid/essai-v%s/Essai-Inventee-%s.dmg' % (version, version),
                           123456, 'U0lHTkFUVVJF', '26.0', '<p>Notes</p>', datetime.datetime(2026, 10, 6, 12, 0, 0))

    def test_flux_neuf_valeurs_inventees(self):
        xml = P.ajouter_au_flux(None, 'Essai Inventee', '1.2.3', self.item())
        self.assertEqual(xml, textwrap.dedent('''\
            <?xml version="1.0" encoding="utf-8"?>
            <rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
              <channel>
                <title>Essai Inventee</title>
                <item>
                  <title>1.2.3</title>
                  <pubDate>Tue, 06 Oct 2026 12:00:00 +0000</pubDate>
                  <sparkle:version>57</sparkle:version>
                  <sparkle:shortVersionString>1.2.3</sparkle:shortVersionString>
                  <sparkle:minimumSystemVersion>26.0</sparkle:minimumSystemVersion>
                  <description><![CDATA[
            <p>Notes</p>
            ]]></description>
                  <enclosure url="https://exemple.invalid/essai-v1.2.3/Essai-Inventee-1.2.3.dmg" length="123456" type="application/octet-stream" sparkle:edSignature="U0lHTkFUVVJF"/>
                </item>
              </channel>
            </rss>
            '''))
        s = '{%s}' % P.ESPACE_SPARKLE
        item = ET.fromstring(xml).find('channel/item')
        self.assertEqual(item.find(s + 'version').text, '57')
        self.assertEqual(item.find(s + 'shortVersionString').text, '1.2.3')
        self.assertEqual(item.find('enclosure').get(s + 'edSignature'), 'U0lHTkFUVVJF')
        self.assertEqual(item.find('description').text.strip(), '<p>Notes</p>')

    def test_adresse_et_signature_echappees(self):
        """Une adresse avec & et une signature avec " restent du XML valide, lues telles quelles."""
        item = P.item_flux('1.2.3', 57, 'https://exemple.invalid/a?b=1&c=2', 1, 'S"G', '26.0', '',
                           datetime.datetime(2026, 10, 6))
        enc = ET.fromstring(P.ajouter_au_flux(None, 'Essai & Co', '1.2.3', item)).find('channel/item/enclosure')
        self.assertEqual(enc.get('url'), 'https://exemple.invalid/a?b=1&c=2')
        self.assertEqual(enc.get('{%s}edSignature' % P.ESPACE_SPARKLE), 'S"G')

    def test_une_version_de_plus_en_tete(self):
        """Le flux garde toutes les versions publiees, la nouvelle en tete."""
        un = P.ajouter_au_flux(None, 'Essai Inventee', '1.2.2', self.item('1.2.2', 56))
        deux = P.ajouter_au_flux(un, 'Essai Inventee', '1.2.3', self.item('1.2.3', 57), 57)
        s = '{%s}' % P.ESPACE_SPARKLE
        items = ET.fromstring(deux).findall('channel/item')
        self.assertEqual([i.find(s + 'shortVersionString').text for i in items], ['1.2.3', '1.2.2'])
        self.assertEqual([i.find(s + 'version').text for i in items], ['57', '56'])
        self.assertEqual(deux.replace(self.item('1.2.3', 57), ''), un, 'le reste du flux ne change pas')
        with self.assertRaises(P.Refus):
            P.ajouter_au_flux(deux, 'Essai Inventee', '1.2.2', self.item('1.2.2', 58))

    def test_ordre_des_versions_dans_le_flux(self):
        """Une version n'entre que superieure a la tete, comparee en nombres, et avec un numero superieur."""
        tete = P.ajouter_au_flux(None, 'Essai Inventee', '1.10.0', self.item('1.10.0', 40))
        with self.assertRaises(P.Refus) as r:
            P.verifier_flux(tete, '1.2.3', 57)
        self.assertIn('superieure', str(r.exception))
        with self.assertRaises(P.Refus) as r:
            P.verifier_flux(tete, '1.10.1', 40)
        self.assertIn('numero de compilation', str(r.exception))
        P.verifier_flux(tete, '1.10.1', 41)
        P.verifier_flux(tete, '1.11.0')
        self.assertEqual(P.versions_du_flux(tete), [('1.10.0', 40)])

    def test_flux_mal_forme(self):
        for texte in ('<rss><channel><item>', '<autre/>',
                      '<rss xmlns:sparkle="%s"><channel><item><sparkle:version>3</sparkle:version></item></channel>'
                      '</rss>' % P.ESPACE_SPARKLE):
            with self.subTest(texte=texte), self.assertRaises(P.Refus) as r:
                P.verifier_flux(texte, '1.2.3')
            self.assertIn('illisible', str(r.exception))

    def test_flux_produit_relu(self):
        """Un flux existant ou la nouvelle version ne se placerait pas en tete (retrait inattendu) : refus."""
        un = P.ajouter_au_flux(None, 'Essai Inventee', '1.2.2', self.item('1.2.2', 56))
        decale = un.replace('    <item>', '  <item>')
        with self.assertRaises(P.Refus) as r:
            P.ajouter_au_flux(decale, 'Essai Inventee', '1.2.3', self.item('1.2.3', 57), 57)
        self.assertIn('en tete', str(r.exception))


class CheminsPersonnelsTests(unittest.TestCase):
    def test_motifs_injectes_par_l_environnement(self):
        motifs = P.motifs_personnels({'HOME': '/maison/inventee', 'USER': COMPTE, 'LOGNAME': 'autre-compte'})
        valeurs = [m for m, _ in motifs]
        for attendu in (b'/Users/', b'/maison/inventee', COMPTE.encode(), b'autre-compte'):
            self.assertIn(attendu, valeurs)
        for _, quoi in motifs:
            self.assertNotIn(COMPTE, quoi, 'la description ne recopie jamais la valeur')
            self.assertNotIn('/maison', quoi)

    def test_carte_des_chemins(self):
        """La racine du depot, sous ses deux formes (par un lien, comme /tmp et /private/tmp), ramenee a « . »."""
        with tempfile.TemporaryDirectory() as d:
            vrai = os.path.realpath(os.path.join(d, 'vrai'))
            os.makedirs(vrai)
            lien = os.path.join(d, 'lien')
            os.symlink(vrai, lien)
            self.assertEqual(sorted(P.carte_des_chemins(lien + '/')), sorted([(lien, '.'), (vrai, '.')]))
            self.assertEqual(P.carte_des_chemins(vrai), [(vrai, '.')])
            self.assertEqual(P.carte_des_chemins(''), [])
            self.assertEqual(P.carte_des_chemins('/'), [])

    def test_chemins_personnels(self):
        with tempfile.TemporaryDirectory() as d:
            ecrire(os.path.join(d, 'A.app', 'bin'), 'propre')
            ecrire(os.path.join(d, 'A.app', 'res'), 'x/maison/inventee/y')
            os.symlink('/Users/autre', os.path.join(d, 'lien'))
            os.symlink('/Applications', os.path.join(d, 'Applications'))
            trouves = P.chemins_personnels(d, P.motifs_personnels({'HOME': '/maison/inventee', 'USER': COMPTE}))
            self.assertEqual(sorted(t[0] for t in trouves), ['A.app/res', 'lien'])


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.dossier = os.path.realpath(tempfile.mkdtemp())
        self.m = Monde(self.dossier)

    def tearDown(self):
        shutil.rmtree(self.dossier)

    def refuse(self, *extra, env=None, motif, etiquette='', etiquette_origine=''):
        with self.assertRaises(P.Refus) as r:
            self.m.publier(*extra, env=env)
        self.assertIn(motif, str(r.exception))
        appels = self.m.appels()
        self.assertFalse([a for a in appels if a.startswith('gh release create')], 'rien de publie')
        self.assertEqual(git(self.m.depot, 'tag', '-l'), etiquette, 'aucune etiquette nouvelle')
        self.assertEqual(git(self.m.origine, 'tag', '-l'), etiquette_origine, 'rien de pousse')
        self.assertEqual(git(self.m.origine, 'rev-list', '--count', 'main'), self.commits_origine, 'main inchange')
        return appels

    @property
    def commits_origine(self):
        return getattr(self, '_commits_origine', '3')

    def commit(self, fichier, texte):
        ecrire(os.path.join(self.m.app, fichier), texte)
        git(self.m.depot, 'add', '-A')
        git(self.m.depot, 'commit', '-q', '-m', 'changement')
        git(self.m.depot, 'push', '-q', 'origin', 'main')
        self._commits_origine = git(self.m.origine, 'rev-list', '--count', 'main')

    def autre_clone(self, *commandes):
        """Un autre clone de l'origine, qui y pousse quelque chose (comme un autre poste, ou GitHub)."""
        autre = os.path.join(self.dossier, 'autre')
        subprocess.run(['git', 'clone', '-q', self.m.origine, autre], check=True, env=GIT_ENV, capture_output=True)
        for c in commandes:
            git(autre, *c)
        return autre

    def rien_compile(self, appels):
        self.assertFalse([a for a in appels if a.startswith('xcodebuild')], 'rien de compile')

    # --- la repetition et la publication ---------------------------------------------------------------------

    def test_repetition(self):
        sortie = os.path.join(self.dossier, 'repetition')
        self.m.publier('--repetition', sortie, '--url-base', 'http://127.0.0.1:8123', '--cle-privee', 'cle.txt',
                       '--cle-publique', AUTRE_CLE, '--sans-tests')
        flux = os.path.join(self.m.depot, 'appcast.xml')
        item = ET.parse(flux).getroot().find('channel/item')
        s = '{%s}' % P.ESPACE_SPARKLE
        self.assertEqual(item.find(s + 'version').text, '3', 'trois commits')
        enc = item.find('enclosure')
        self.assertEqual(enc.get('url'),
                         'http://127.0.0.1:8123/Exemple/essai/releases/download/essai-v1.2.3/Essai-Inventee-1.2.3.dmg')
        self.assertEqual(git(self.m.depot, 'log', '-1', '--format=%s'),
                         'Publier Essai Inventee 1.2.3 dans le flux des mises a jour', 'commite dans la copie')
        self.assertEqual(git(self.m.depot, 'status', '--porcelain'), '')
        self.assertEqual(git(self.m.origine, 'rev-list', '--count', 'main'), '3', 'rien de pousse')
        self.assertEqual(enc.get(s + 'edSignature'), 'U0lHTkFUVVJFLUlOVkVOVEVF')
        dmg = os.path.join(sortie, 'Essai-Inventee-1.2.3.dmg')
        self.assertEqual(int(enc.get('length')), os.path.getsize(dmg))
        self.assertIn('-volname Essai Inventee 1.2.3', lire(dmg))
        appels = self.m.appels()
        build = [a for a in appels if a.startswith('xcodebuild')][0]
        for r in ('-configuration Release', 'CURRENT_PROJECT_VERSION=3', 'CODE_SIGN_IDENTITY=-', 'DEVELOPMENT_TEAM= ',
                  'FLUX_MISES_A_JOUR=http://127.0.0.1:8123/Exemple/essai/main/appcast.xml',
                  'CLE_MISES_A_JOUR=' + AUTRE_CLE):
            self.assertIn(r, build)
        self.assertIn('sign_update --ed-key-file cle.txt -p ' + dmg, appels)
        self.assertFalse([a for a in appels if a.startswith(('gh ', 'generate_keys'))], 'ni GitHub ni trousseau')
        controle = [a for a in appels if a.startswith('controles.py')][-1]
        self.assertIn('NOTES-VERSIONS.md', controle)
        self.assertIn(os.path.join(sortie, 'appcast.xml'), controle)
        self.assertIn('project.yml', controle)
        self.assertEqual(git(self.m.depot, 'tag', '-l'), '')
        self.assertEqual(lire(os.path.join(sortie, 'gestes.txt')).count('\n'), 1, 'le commit du flux seul')

    def test_repetition_avec_la_cle_du_trousseau(self):
        """Sans paire d'essai : la cle publique de project.yml, celle du trousseau, et la signature par le trousseau."""
        sortie = os.path.join(self.dossier, 'repetition')
        self.m.publier('--repetition', sortie, '--url-base', 'http://127.0.0.1:8123', '--sans-tests')
        appels = self.m.appels()
        self.assertIn('generate_keys -p', appels)
        self.assertIn('sign_update -p ' + os.path.join(sortie, 'Essai-Inventee-1.2.3.dmg'), appels)
        build = [a for a in appels if a.startswith('xcodebuild')][0]
        self.assertIn('FLUX_MISES_A_JOUR=http://127.0.0.1:8123/Exemple/essai/main/appcast.xml', build)
        self.assertNotIn('CLE_MISES_A_JOUR=', build, 'la cle de project.yml')
        self.assertFalse([a for a in appels if a.startswith('gh ')])
        self.assertEqual(git(self.m.depot, 'tag', '-l'), '')

    def test_publication(self):
        avant = git(self.m.depot, 'rev-parse', 'HEAD')
        self.m.publier()
        appels = self.m.appels()
        self.assertEqual(git(self.m.origine, 'tag', '-l'), 'essai-v1.2.3', 'etiquette de l\'app, creee par GitHub')
        self.assertEqual(git(self.m.origine, 'rev-parse', 'essai-v1.2.3^{commit}'), avant, 'sur le commit verifie')
        self.assertEqual(git(self.m.depot, 'tag', '-l'), '', 'aucune etiquette locale')
        sortie = os.path.join(self.m.depot, 'build', 'publication', '1.2.3')
        dmg = os.path.join(sortie, 'Essai-Inventee-1.2.3.dmg')
        cree = [a for a in appels if a.startswith('gh release create')]
        self.assertEqual(len(cree), 1)
        self.assertIn('essai-v1.2.3 %s -R Exemple/essai --target %s ' % (dmg, avant), cree[0], 'le .dmg seul')
        self.assertNotIn('--verify-tag', cree[0])
        self.assertIn('--title Essai Inventee 1.2.3', cree[0])
        self.assertIn('--notes-file %s/notes.md' % sortie, cree[0])
        self.assertIn('sign_update -p ' + dmg, appels, 'cle du trousseau')
        # GitHub lu deux fois : avant les tests, puis juste avant les gestes publics.
        self.assertEqual(appels.count('gh release view essai-v1.2.3 -R Exemple/essai'), 2)
        self.assertEqual(appels.count('gh auth status --hostname github.com'), 2)
        # Le flux, dans le depot, commite puis pousse sur main, apres la version publiee.
        flux = lire(os.path.join(self.m.depot, 'appcast.xml'))
        self.assertEqual(git(self.m.origine, 'show', 'main:appcast.xml') + '\n', flux)
        self.assertEqual(git(self.m.origine, 'log', '-1', '--format=%an %ae', 'main'),
                         'Essai 0+essai@users.noreply.github.com', 'l\'auteur du depot, adresse noreply')
        message = git(self.m.origine, 'log', '-1', '--format=%B', 'main')
        self.assertEqual(message, 'Publier Essai Inventee 1.2.3 dans le flux des mises a jour\n\n'
                                  'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>')
        self.assertEqual(git(self.m.origine, 'show', '--name-only', '--format=', 'main'), 'appcast.xml')
        self.assertEqual(git(self.m.depot, 'status', '--porcelain'), '')
        controle = [a for a in appels if a.startswith('controles.py')][-1]
        self.assertIn('message-commit.txt', controle, 'le message passe aussi le controle')
        self.assertIn('https://github.com/Exemple/essai/releases/download/essai-v1.2.3/Essai-Inventee-1.2.3.dmg', flux)
        self.assertIn('<li>A new <code>command</code>.</li>', flux)
        with open(os.path.join(self.m.dd, 'Build', 'Products', 'Release', 'Essai Inventee.app', 'Contents',
                               'Info.plist'), 'rb') as f:
            info = plistlib.load(f)
        self.assertEqual(info['CFBundleVersion'], '3')

    def test_ordre_des_gestes_publics(self):
        """La version est publiee avant que le flux l'annonce : au moment de gh release create, main sur GitHub est
        encore le commit verifie. Chaque geste fait est note, dans l'ordre."""
        avant = git(self.m.depot, 'rev-parse', 'HEAD')
        self.m.publier()
        self.assertIn('origine au moment de la publication ' + avant, self.m.appels())
        self.assertNotEqual(git(self.m.origine, 'rev-parse', 'main'), avant, 'le flux, pousse ensuite')
        gestes = lire(os.path.join(self.m.depot, 'build', 'publication', '1.2.3', 'gestes.txt')).splitlines()
        self.assertEqual([g.split(' ', 1)[1].split(' :')[0] for g in gestes],
                         ['version publiee', 'flux commite', 'flux pousse sur main'])
        self.assertIn('essai-v1.2.3, etiquette creee sur ' + avant, gestes[0])

    def test_echec_de_la_version_publiee(self):
        """gh release create en echec : le flux n'est ni commite ni pousse, et gestes.txt ne note rien."""
        with self.assertRaises(subprocess.CalledProcessError):
            self.m.publier(env={'FAUX_ECHEC_CREATE': '1'})
        self.assertFalse(os.path.exists(os.path.join(self.m.depot, 'appcast.xml')), 'le flux du depot ne change pas')
        self.assertEqual(git(self.m.depot, 'rev-list', '--count', 'HEAD'), '3', 'aucun commit')
        self.assertEqual(git(self.m.origine, 'rev-list', '--count', 'main'), '3', 'origine inchangee')
        self.assertEqual(git(self.m.origine, 'tag', '-l'), '', 'aucune etiquette poussee a part')
        self.assertEqual(git(self.m.depot, 'status', '--porcelain'), '')
        gestes = os.path.join(self.m.depot, 'build', 'publication', '1.2.3', 'gestes.txt')
        self.assertFalse(os.path.exists(gestes) and lire(gestes).strip(), 'aucun geste fait')

    def test_app_dans_un_sous_dossier(self):
        """Le cas du pont : l'app et son flux dans apps/macos, macOS 15.0 minimum, publier.sh lance de la."""
        m = Monde(os.path.join(self.dossier, 'pont'), sous='apps/macos', systeme='15.0')
        m.publier()
        flux = git(m.origine, 'show', 'main:apps/macos/appcast.xml')
        item = ET.fromstring(flux).find('channel/item')
        s = '{%s}' % P.ESPACE_SPARKLE
        self.assertEqual(item.find(s + 'minimumSystemVersion').text, '15.0')
        self.assertEqual(git(m.origine, 'show', '--name-only', '--format=', 'main'), 'apps/macos/appcast.xml')
        self.assertTrue(os.path.exists(os.path.join(m.app, 'build', 'publication', '1.2.3',
                                                    'Essai-Inventee-1.2.3.dmg')))
        sortie = os.path.join(self.dossier, 'pont-repetition')
        m.oublier()
        git(m.depot, 'reset', '-q', '--hard', 'HEAD~1')
        m.publier('--repetition', sortie, '--url-base', 'http://127.0.0.1:8123', '--sans-tests')
        build = [a for a in m.appels() if a.startswith('xcodebuild')][0]
        self.assertIn('FLUX_MISES_A_JOUR=http://127.0.0.1:8123/Exemple/essai/main/apps/macos/appcast.xml', build)

    # --- la compilation et le contenu du .dmg ----------------------------------------------------------------

    def test_compilation_sans_symboles_ni_chemins_personnels(self):
        self.m.publier()
        build = [a for a in self.m.appels() if a.startswith('xcodebuild')][0]
        for r in ('DEPLOYMENT_POSTPROCESSING=YES', 'STRIP_INSTALLED_PRODUCT=YES',
                  'OTHER_SWIFT_FLAGS=$(inherited) -file-prefix-map "%s=." ' % self.m.depot,
                  'OTHER_CFLAGS=$(inherited) "-ffile-prefix-map=%s=." ' % self.m.depot):
            self.assertIn(r, build)
        self.assertIn('aucun chemin personnel', self.m.dit)

    def test_refus_chemin_personnel_dans_le_contenu_du_dmg(self):
        """Le dossier personnel, /Users/ ou le nom du compte (injectes), dans un binaire de Sparkle, de l'app, une
        ressource ou la cible d'un lien : refus avant hdiutil, rien de publie."""
        cas = [({'FAUX_CHEMIN': 'home:sparkle'}, 'Sparkle.framework/Versions/B/Sparkle'),
               ({'FAUX_CHEMIN': 'home:ressource'}, 'Localizable.strings'),
               ({'FAUX_CHEMIN': 'users:app'}, 'MacOS/Essai Inventee'),
               ({'FAUX_CHEMIN': 'compte:autoupdate'}, 'Versions/B/Autoupdate'),
               ({'FAUX_LIEN': '1'}, 'Resources/lien')]
        for env, fichier in cas:
            with self.subTest(env=env):
                self.m.oublier()
                appels = self.refuse(env=env, motif='chemin personnel')
                self.assertTrue([a for a in appels if a.startswith('codesign --force')], 'apres la signature')
                self.assertFalse([a for a in appels if a.startswith(('hdiutil', 'sign_update'))], 'aucun .dmg')
                self.assertFalse([a for a in appels if a.startswith('controles.py')])
        self.m.oublier()
        self.m.publier()
        self.assertTrue([a for a in self.m.appels() if a.startswith('gh release create')], 'l\'app propre passe')

    def test_nom_du_compte_injecte(self):
        """Le nom du compte cherche vient de l'environnement (USER), pas seulement du systeme : le nom invente, ecrit
        dans une ressource, est vu comme le nom du compte."""
        self.refuse(env={'FAUX_CHEMIN': 'compte:ressource'}, motif='(le nom du compte)')

    def test_contenu_du_dmg(self):
        """Le .dmg porte l'app, le raccourci vers Applications et la licence de Sparkle."""
        self.m.publier()
        dmg = lire(os.path.join(self.m.depot, 'build', 'publication', '1.2.3', 'Essai-Inventee-1.2.3.dmg'))
        lignes = dmg.splitlines()
        for attendu in ('Essai Inventee.app/Contents/Info.plist', 'Essai Inventee.app/Contents/MacOS/Essai Inventee',
                        'Essai Inventee.app/Contents/Frameworks/Sparkle.framework/Versions/B/Sparkle',
                        'Applications -> /Applications', 'Sparkle-LICENSE.txt'):
            self.assertIn(attendu, lignes)

    def test_refus_licence_absente(self):
        appels = self.refuse('--licence', os.path.join(self.dossier, 'absente.txt'), motif='licence')
        self.rien_compile(appels)

    def test_controle_du_contenu_du_dmg(self):
        """Le controle d'anonymisation passe sur chaque fichier du .dmg, Sparkle et la licence compris, avant
        hdiutil."""
        self.m.publier()
        appels = self.m.appels()
        controle = [a for a in appels if a.startswith('controles.py')][0]
        self.assertLess(appels.index(controle), [i for i, a in enumerate(appels) if a.startswith('hdiutil')][0])
        for attendu in ('/dmg/Essai Inventee.app/Contents/Info.plist',
                        '/dmg/Essai Inventee.app/Contents/Frameworks/Sparkle.framework/Versions/B/Sparkle',
                        '/dmg/Essai Inventee.app/Contents/Resources/fr.lproj/Localizable.strings',
                        '/dmg/Sparkle-LICENSE.txt'):
            self.assertIn(attendu, controle)
        self.assertIn('--table %s/table.json' % self.m.bin, controle)

    def test_refus_controle_du_contenu_du_dmg(self):
        appels = self.refuse(env={'FAUX_TROUVE': 'Sparkle.framework'}, motif='contenu du .dmg')
        self.assertFalse([a for a in appels if a.startswith(('hdiutil', 'sign_update'))])

    def test_refus_info_plist_de_l_app_compilee(self):
        appels = self.refuse(env={'FAUX_INFO': 'cle'}, motif='Info.plist')
        self.assertFalse([a for a in appels if a.startswith('codesign --force')], 'rien de signe')

    # --- la signature et son certificat ----------------------------------------------------------------------

    def signatures(self):
        """Les signatures du code, dans l'ordre : le chemin signe, depuis le dossier des produits."""
        base = os.path.join(self.m.dd, 'Build', 'Products', 'Release') + '/'
        return [a[a.index(base) + len(base):].replace('Essai Inventee.app/Contents/Frameworks/', '')
                for a in self.m.appels() if a.startswith('codesign --force')]

    def test_signature_du_code(self):
        """Le code imbrique d'abord, du plus profond au moins profond, chaque cadre apres son contenu, l'app en
        dernier ; par l'empreinte de l'identite, jamais son nom ; le runtime renforce, les droits gardes ; puis la
        verification, le certificat feuille relu et l'exigence de signature."""
        self.m.publier()
        self.assertEqual(self.signatures(), [
            'Coeur.framework',
            'Sparkle.framework/Versions/Current/XPCServices/Downloader.xpc',
            'Sparkle.framework/Versions/Current/XPCServices/Installer.xpc',
            'Sparkle.framework/Versions/Current/Autoupdate',
            'Sparkle.framework/Versions/Current/Updater.app',
            'Sparkle.framework',
            'Essai Inventee.app'])
        appels = self.m.appels()
        for a in [a for a in appels if a.startswith('codesign --force')]:
            self.assertIn('--sign %s --options runtime --preserve-metadata=entitlements --timestamp=none'
                          % EMPREINTE, a)
            self.assertNotIn(IDENTITE, a)
            self.assertNotIn('--keychain', a)
        app = os.path.join(self.m.dd, 'Build', 'Products', 'Release', 'Essai Inventee.app')
        self.assertIn('codesign --verify --deep --strict ' + app, appels)
        sortie = os.path.join(self.m.depot, 'build', 'publication', '1.2.3')
        self.assertIn('codesign -d --extract-certificates=%s/certificat- %s' % (sortie, app), appels)
        self.assertIn('openssl x509 -inform DER -in %s/certificat-0 -noout -subject -nameopt RFC2253 -fingerprint '
                      '-sha1' % sortie, appels)
        self.assertIn('certificate leaf', lire(os.path.join(sortie, 'exigence.txt')))

    def test_signature_dans_un_trousseau_a_part(self):
        """En repetition, un certificat d'essai dans un trousseau a part : codesign et security y cherchent."""
        sortie = os.path.join(self.dossier, 'repetition')
        self.m.publier('--repetition', sortie, '--url-base', 'http://127.0.0.1:8123', '--cle-privee', 'cle.txt',
                       '--cle-publique', AUTRE_CLE, '--sans-tests', '--trousseau', '/tmp/essai.keychain-db')
        appels = self.m.appels()
        self.assertIn('security find-identity -p codesigning /tmp/essai.keychain-db', appels)
        self.assertIn('security find-certificate -a -c %s -Z -p /tmp/essai.keychain-db' % IDENTITE, appels)
        signe = [a for a in appels if a.startswith('codesign --force')]
        self.assertEqual(len(signe), 7)
        self.assertTrue(all('--keychain /tmp/essai.keychain-db' in a for a in signe))

    def test_refus_identite_absente(self):
        appels = self.refuse(env={'FAUSSE_IDENTITE': 'Autre Signing'}, motif='identite de signature')
        self.rien_compile(appels)

    def test_refus_deux_identites_du_meme_nom(self):
        """Deux certificats au meme nom : lequel signerait ? Refus (une identite listee deux fois, elle, passe)."""
        appels = self.refuse(env={'FAUSSE_IDENTITE_DOUBLE': '1'}, motif='une seule attendue')
        self.rien_compile(appels)
        sortie = 'Policy\n  1) %s "%s" (x)\n\n  Valid\n  1) %s "%s"\n  2) %s "%s 2"\n' % (
            EMPREINTE, IDENTITE, EMPREINTE, IDENTITE, AUTRE_EMPREINTE, IDENTITE)
        self.assertEqual(P.empreinte_identite(sortie, IDENTITE), EMPREINTE, 'le nom exact, pas un nom plus long')

    def test_refus_sujet_du_certificat(self):
        """Le certificat est public : seulement CN=<nom>, sans adresse, organisation ni autre nom ; et le bon."""
        cas = [({'FAUX_SUJET': 'CN=%s,emailAddress=x@exemple.invalid' % IDENTITE}, 'sujet'),
               ({'FAUX_SUJET': 'O=Exemple,CN=%s' % IDENTITE}, 'sujet'),
               ({'FAUX_SUJET': 'CN=Autre'}, 'sujet'),
               ({'FAUX_SAN': '1'}, 'subjectAltName'),
               ({'FAUX_EMETTEUR': '1'}, 'une adresse'),
               ({'FAUX_CERTIFICAT_ABSENT': '1'}, 'introuvable')]
        for env, motif in cas:
            with self.subTest(env=env):
                self.m.oublier()
                appels = self.refuse(env=env, motif=motif)
                self.rien_compile(appels)

    def test_refus_certificat_feuille_de_l_app(self):
        """Le certificat dans la signature de l'app, relu : refus avant hdiutil s'il n'est pas conforme."""
        cas = [{'FAUX_SUJET_FEUILLE': 'CN=%s,emailAddress=x@exemple.invalid' % IDENTITE},
               {'FAUSSE_EMPREINTE_FEUILLE': AUTRE_EMPREINTE}, {'FAUX_SANS_CERTIFICAT': '1'}]
        for env in cas:
            with self.subTest(env=env):
                self.m.oublier()
                appels = self.refuse(env=env, motif='certificat')
                self.assertTrue([a for a in appels if a.startswith('codesign --force')], 'apres la signature')
                self.assertFalse([a for a in appels if a.startswith('hdiutil')], 'aucun .dmg')

    def test_notarisation_pas_par_defaut(self):
        self.m.publier()
        self.assertFalse([a for a in self.m.appels() if a.startswith(('xcrun', 'spctl'))])

    def test_refus_notarisation_sans_profil(self):
        appels = self.refuse(env={'NOTARISER': '1'}, motif='PROFIL_NOTARISATION')
        self.assertFalse([a for a in appels if a.startswith(('xcodebuild', 'xcrun'))], 'rien de compile ni soumis')

    def test_notarisation(self):
        """NOTARISER=1 : signatures horodatees, le .dmg signe, soumis, agrafe, evalue, puis signe par Sparkle (la
        signature Ed25519 porte sur le .dmg agrafe)."""
        self.m.publier(env={'NOTARISER': '1', 'PROFIL_NOTARISATION': 'profil-essai'})
        appels = self.m.appels()
        dmg = os.path.join(self.m.depot, 'build', 'publication', '1.2.3', 'Essai-Inventee-1.2.3.dmg')
        signe = [a for a in appels if a.startswith('codesign --force')]
        self.assertTrue(all('--timestamp ' in a and '--timestamp=none' not in a for a in signe))
        suite = [a for a in appels if a.startswith(('xcrun', 'spctl', 'sign_update')) or a.endswith(' ' + dmg)
                 and a.startswith('codesign')]
        self.assertEqual(suite, [
            'codesign --force --sign %s --timestamp %s' % (EMPREINTE, dmg),
            'xcrun notarytool submit %s --keychain-profile profil-essai --wait' % dmg,
            'xcrun stapler staple %s' % dmg,
            'spctl --assess --type open --context context:primary-signature --verbose %s' % dmg,
            'sign_update -p %s' % dmg])

    # --- les verifications d'avant les tests -----------------------------------------------------------------

    def test_refus_version_differente(self):
        self.commit('project.yml', PROJET.replace('"1.2.3"', '"1.2.4"'))
        appels = self.refuse(motif='MARKETING_VERSION')
        self.rien_compile(appels)

    def test_refus_arbre_pas_propre(self):
        ecrire(os.path.join(self.m.depot, 'oubli.txt'), 'x\n')
        self.refuse(motif='propre')

    def test_refus_etiquette_existante(self):
        git(self.m.depot, 'tag', 'essai-v1.2.3')
        self.refuse(motif='existe deja', etiquette='essai-v1.2.3')

    def test_refus_etiquette_seulement_sur_github(self):
        """L'etiquette poussee par un autre poste, absente de la copie : refus, sans la rapatrier."""
        self.autre_clone(['tag', 'essai-v1.2.3'], ['push', '-q', 'origin', 'essai-v1.2.3'])
        appels = self.refuse(motif='existe deja sur GitHub', etiquette_origine='essai-v1.2.3')
        self.rien_compile(appels)

    def test_etiquette_d_une_autre_app(self):
        """L'etiquette d'une autre app du meme depot, au meme numero, ne gene pas."""
        git(self.m.depot, 'tag', 'autre-v1.2.3')
        git(self.m.depot, 'push', '-q', 'origin', 'autre-v1.2.3')
        self.m.publier()
        self.assertEqual(git(self.m.origine, 'tag', '-l').split(), ['autre-v1.2.3', 'essai-v1.2.3'])

    def test_ajout_a_un_flux_existant(self):
        """Le flux du depot garde les versions d'avant : la nouvelle s'ajoute en tete."""
        self.commit('appcast.xml', P.ajouter_au_flux(None, 'Essai Inventee', '1.2.2', P.item_flux(
            '1.2.2', 2, 'https://github.com/Exemple/essai/releases/download/essai-v1.2.2/Essai-Inventee-1.2.2.dmg',
            10, 'QU5DSUVOTkU=', '26.0', '<p>Ancienne</p>', datetime.datetime(2026, 10, 1))))
        self.m.publier()
        s = '{%s}' % P.ESPACE_SPARKLE
        items = ET.fromstring(lire(os.path.join(self.m.depot, 'appcast.xml'))).findall('channel/item')
        self.assertEqual([i.find(s + 'shortVersionString').text for i in items], ['1.2.3', '1.2.2'])
        self.assertEqual(items[0].find(s + 'version').text, '4')

    def flux_existant(self, version, numero):
        self.commit('appcast.xml', P.ajouter_au_flux(None, 'Essai Inventee', version, P.item_flux(
            version, numero, 'https://exemple.invalid/x.dmg', 10, 'QQ==', '26.0', '', datetime.datetime(2026, 10, 1))))

    def test_refus_version_deja_dans_le_flux(self):
        self.flux_existant('1.2.3', 2)
        self.rien_compile(self.refuse(motif='deja dans le flux'))

    def test_refus_version_pas_superieure_a_la_tete_du_flux(self):
        """1.2.3 apres 1.10.0 : refus (une comparaison de chaines la laisserait passer)."""
        self.flux_existant('1.10.0', 2)
        self.rien_compile(self.refuse(motif='pas superieure'))

    def test_refus_numero_pas_superieur_a_la_tete_du_flux(self):
        """Un historique reecrit peut faire baisser le nombre de commits : Sparkle ne proposerait pas la version."""
        self.flux_existant('1.2.2', 4)
        self.rien_compile(self.refuse(motif='numero de compilation'))

    def test_refus_flux_existant_mal_forme(self):
        self.commit('appcast.xml', '<rss><channel><item>\n')
        self.rien_compile(self.refuse(motif='illisible'))

    def test_refus_auteur_sans_adresse_noreply(self):
        """Le commit du flux est public : son auteur porte l'adresse noreply de GitHub, jamais une vraie."""
        git(self.m.depot, 'config', 'user.email', 'essai@example.invalid')
        self.rien_compile(self.refuse(motif='noreply'))

    def test_refus_identite_git_par_l_environnement(self):
        """git var : l'environnement l'emporte sur la configuration, pour l'auteur comme pour le committer, et le nom
        compte aussi."""
        cas = [{'GIT_AUTHOR_EMAIL': 'essai@example.invalid'}, {'GIT_COMMITTER_EMAIL': 'essai@example.invalid'},
               {'GIT_AUTHOR_NAME': 'Autre'}, {'GIT_COMMITTER_NAME': 'Autre'}]
        for env in cas:
            with self.subTest(env=env):
                self.m.oublier()
                self.rien_compile(self.refuse(env=env, motif='noreply'))
        with self.subTest('EMAIL sans user.email'):
            self.m.oublier()
            git(self.m.depot, 'config', '--unset', 'user.email')
            self.rien_compile(self.refuse(env={'EMAIL': 'essai@example.invalid'}, motif='noreply'))

    def test_refus_adresse_du_flux(self):
        """L'app doit lire le flux a l'adresse brute du depot, celle ou publier.sh le pousse."""
        self.commit('project.yml', PROJET.replace('raw.githubusercontent.com/Exemple/essai/main/appcast.xml',
                                                  'github.com/Exemple/essai/releases/latest/download/appcast.xml'))
        self.refuse(motif='FLUX_MISES_A_JOUR')

    def test_refus_hors_de_main(self):
        git(self.m.depot, 'checkout', '-q', '-b', 'autre')
        self.refuse(motif='depuis main')

    def test_refus_main_pas_a_jour(self):
        ecrire(os.path.join(self.m.depot, 'f0.txt'), 'local\n')
        git(self.m.depot, 'commit', '-q', '-am', 'pas pousse')
        self.refuse(motif='a jour')

    def test_refus_origine_avancee_par_un_autre_clone(self):
        """main avance sur GitHub, rien en local : seul le fetch le voit."""
        self.autre_clone(['commit', '-q', '--allow-empty', '-m', 'ailleurs'], ['push', '-q', 'origin', 'main'])
        self._commits_origine = '4'
        self.rien_compile(self.refuse(motif='a jour'))

    def test_refus_deja_publiee(self):
        self.refuse(env={'FAUX_PUBLIEE': '1'}, motif='deja publiee')

    def test_refus_gh_release_view_en_erreur(self):
        """Toute reponse de gh release view autre que « release not found » : l'etat de GitHub est inconnu."""
        self.rien_compile(self.refuse(env={'FAUX_VUE': 'HTTP 401: Bad credentials'}, motif='inconnu'))

    def test_refus_gh_sans_session(self):
        self.rien_compile(self.refuse(env={'FAUX_AUTH_ECHEC': '1'}, motif='gh auth status'))

    def test_refus_push_impossible(self):
        git(self.m.depot, 'config', 'remote.origin.pushurl', os.path.join(self.dossier, 'absente.git'))
        self.rien_compile(self.refuse(motif='push --dry-run'))

    def test_refus_notes_absentes(self):
        self.commit('NOTES-VERSIONS.md', NOTES.replace('## 1.2.3', '## 1.2.1'))
        self.refuse(motif='pas de section 1.2.3')

    def test_refus_cle_du_trousseau_differente(self):
        appels = self.refuse(env={'FAUSSE_CLE': AUTRE_CLE}, motif='trousseau')
        self.assertIn('generate_keys -p', appels)

    def test_refus_cle_publique_invalide(self):
        sortie = os.path.join(self.dossier, 'repetition')
        self.rien_compile(self.refuse('--repetition', sortie, '--url-base', 'http://127.0.0.1:8123', '--cle-privee',
                                      'cle.txt', '--cle-publique', 'pas-une-cle', '--sans-tests', motif='invalide'))

    def test_refus_sans_sparkle_bin(self):
        """Hors repetition, sign_update et generate_keys viennent de l'archive de Sparkle 2.10.0, jamais du PATH."""
        self.m.env['SPARKLE_BIN'] = ''
        appels = self.refuse(motif='SPARKLE_BIN (le dossier bin')
        self.assertFalse([a for a in appels if a.startswith(('generate_keys', 'xcodebuild'))])
        self.m.oublier()
        self.m.env['SPARKLE_BIN'] = os.path.join(self.dossier, 'vide')
        self.rien_compile(self.refuse(motif='introuvable dans SPARKLE_BIN'))

    def test_refus_tests_en_echec(self):
        appels = self.refuse('--test', 'exit 3', motif='tests en echec')
        self.rien_compile(appels)

    def test_refus_sans_tests_hors_repetition(self):
        self.refuse('--sans-tests', motif='repetition')

    # --- juste avant les gestes publics ----------------------------------------------------------------------

    def test_refus_origine_avancee_pendant_les_tests(self):
        """Pendant les tests et la compilation, main avance sur GitHub : l'etat relu avant les gestes publics le
        voit, et rien n'est publie."""
        autre = os.path.join(self.dossier, 'autre')
        pousse = ('git clone -q %s %s && git -C %s -c user.name=X -c user.email=x@exemple.invalid commit -q '
                  '--allow-empty -m ailleurs && git -C %s push -q origin main' % (self.m.origine, autre, autre, autre))
        self._commits_origine = '4'
        appels = self.refuse('--test', pousse, motif='a jour')
        self.assertTrue([a for a in appels if a.startswith('sign_update')], 'refuse juste avant les gestes publics')

    def test_refus_commit_local_pendant_les_tests(self):
        """Un commit local pendant les tests (non pousse) : HEAD n'est plus le commit verifie, rien n'est publie."""
        appels = self.refuse('--test', 'git -c user.name=X -c user.email=x@exemple.invalid commit -q --allow-empty '
                             '-m local', motif='HEAD a change')
        self.assertTrue([a for a in appels if a.startswith('sign_update')], 'refuse juste avant les gestes publics')

    def test_refus_fichier_suivi_modifie_pendant_les_tests(self):
        appels = self.refuse('--test', 'echo x >> f0.txt', motif='propre')
        self.assertTrue([a for a in appels if a.startswith('sign_update')], 'refuse juste avant les gestes publics')

    # --- le controle d'anonymisation -------------------------------------------------------------------------

    def test_refus_controle_d_anonymisation(self):
        appels = self.refuse(env={'FAUX_TROUVE': 'message-commit.txt'}, motif='anonymisation')
        self.assertTrue([a for a in appels if a.startswith('sign_update')], 'le controle passe apres la signature')
        self.assertFalse(os.path.exists(os.path.join(self.m.depot, 'appcast.xml')), 'le flux du depot ne change pas')
        self.assertEqual(git(self.m.depot, 'status', '--porcelain'), '')

    def test_refus_controle_absent_hors_repetition(self):
        for cle in ('CONTROLE_ANONYMISATION', 'TABLE_ANONYMISATION'):
            with self.subTest(absent=cle):
                self.m.oublier()
                ancien = self.m.env[cle]
                self.m.env[cle] = os.path.join(self.dossier, 'absent')
                self.rien_compile(self.refuse(motif='absent'))
                self.m.env[cle] = ancien

    def test_controle_absent_saute_en_repetition(self):
        self.m.env['CONTROLE_ANONYMISATION'] = os.path.join(self.dossier, 'absent.py')
        self.m.publier('--repetition', os.path.join(self.dossier, 'repetition'), '--url-base',
                       'http://127.0.0.1:8123', '--sans-tests')
        self.assertIn("controle d'anonymisation absent de ce Mac : saute", self.m.dit)
        self.assertFalse([a for a in self.m.appels() if a.startswith('controles.py')])

    # --- la repetition et le Bureau --------------------------------------------------------------------------

    def test_refus_repetition_dans_une_copie_de_github(self):
        """Un flux d'essai commite dans un clone de GitHub pourrait y etre pousse a la main : refus."""
        git(self.m.depot, 'remote', 'set-url', 'origin', 'https://github.com/Exemple/essai.git')
        appels = self.refuse('--repetition', os.path.join(self.dossier, 'repetition'), '--url-base',
                             'http://127.0.0.1:8123', '--sans-tests', motif='GitHub')
        self.rien_compile(appels)
        self.assertEqual(git(self.m.depot, 'rev-list', '--count', 'HEAD'), '3', 'aucun commit')

    def test_copie_sur_le_bureau(self):
        self.m.publier(bureau=True)
        self.assertEqual(os.listdir(os.path.join(self.m.maison, 'Desktop')), ['Essai-Inventee-1.2.3.dmg'])

    def test_repetition_jamais_sur_le_bureau(self):
        self.m.publier('--repetition', os.path.join(self.dossier, 'repetition'), '--url-base',
                       'http://127.0.0.1:8123', '--sans-tests', bureau=True)
        self.assertEqual(os.listdir(os.path.join(self.m.maison, 'Desktop')), [])

    # --- les arguments, les erreurs, les outils --------------------------------------------------------------

    def test_arguments_de_la_repetition(self):
        base = ['publier', '1.2.3', '--nom-app', 'A', '--fichier', 'A', '--depot-github', 'E/a', '--projet',
                'A.xcodeproj', '--schema', 'A', '--cible', 'A', '--identite', 'I', '--auteur', 'E', '--etiquette',
                'a-v', '--flux', 'appcast.xml', '--licence', 'L']
        P.arguments(base)
        with mock.patch('sys.stderr'):
            for extra in (['--repetition', 'x'], ['--repetition', 'x', '--url-base', 'http://127.0.0.1:1',
                                                   '--cle-privee', 'k'], ['--cle-privee', 'k', '--cle-publique', CLE]):
                with self.subTest(extra=extra), self.assertRaises(SystemExit):
                    P.arguments(base + extra)
            for manque in ('--auteur', '--licence'):
                i = base.index(manque)
                with self.subTest(manque=manque), self.assertRaises(SystemExit):
                    P.arguments(base[:i] + base[i + 2:])

    def test_echec_d_un_outil_sans_trace(self):
        """Un outil introuvable : main rend 1 et un message, sans trace Python."""
        code, erreurs = self.m.main(env={'HDIUTIL': os.path.join(self.dossier, 'absent', 'hdiutil')})
        self.assertEqual(code, 1)
        self.assertIn('echec', erreurs)
        self.assertNotIn('Traceback', erreurs)
        self.assertFalse([a for a in self.m.appels() if a.startswith('gh release create')])

    def test_aucun_outil_reel_dans_les_tests(self):
        """Chaque commande externe des tests est une fausse commande, sauf git (sur un faux depot)."""
        o = P.Outils(self.m.env)
        for nom, valeur in vars(o).items():
            if nom != 'git':
                with self.subTest(outil=nom):
                    self.assertTrue(valeur == self.m.bin or valeur.startswith(self.m.bin + '/'), nom)


if __name__ == '__main__':
    unittest.main()
