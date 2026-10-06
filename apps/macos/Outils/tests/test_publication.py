"""Tests de publication.py : les numeros, les notes, le flux appcast.xml a partir de valeurs inventees (nouveau, ou
une version de plus en tete d'un flux qui les garde toutes), les etiquettes propres a l'app, les controles avant
publication, le commit du flux, la signature du code et la notarisation (desactivee par defaut), sur un faux depot et de
fausses commandes (xcodebuild, codesign, security, hdiutil, sign_update, xcrun, gh...). Aucun reseau, aucun
trousseau, rien sur le Bureau.

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
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import publication as P  # noqa: E402

CLE = 'p7jaASHZk/U9YboWWSgw+Z4xEGYXOfUUhTKtZG7uQlE='
IDENTITE = 'Essai Inventee Signing'
AUTRE_CLE = 'lkPxEHj5erw+omLlr1AVsIoyhfz4YnoLa/N9147SNgc='
GIT_ENV = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_SYSTEM='/dev/null',
               GIT_AUTHOR_NAME='Essai', GIT_AUTHOR_EMAIL='essai@example.invalid',
               GIT_COMMITTER_NAME='Essai', GIT_COMMITTER_EMAIL='essai@example.invalid')

PROJET = textwrap.dedent('''\
    name: Essai
    options:
      bundleIdPrefix: fr.exemple
      deploymentTarget:
        macOS: "26.0"
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
            FLUX_MISES_A_JOUR: https://raw.githubusercontent.com/Exemple/essai/main/appcast.xml
            CLE_MISES_A_JOUR: %s
    ''' % CLE)

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
    'codesign': 'import sys\nif sys.argv[1:3] == ["-d", "-r-"]:\n'
                '    print(\'designated => identifier "fr.exemple.essai" and certificate leaf = H"0123abcd"\')\n',
    'security': 'import os\nprint(\'  1) 0123ABCD "%s" (CSSMERR_TP_NOT_TRUSTED)\' % os.environ.get("FAUSSE_IDENTITE", "Essai Inventee Signing"))\n',
    'xcrun': 'import sys\n',
    'spctl': 'import sys\n',
    'xcodebuild': textwrap.dedent('''\
        import os, plistlib, re, sys
        a = sys.argv[1:]
        dd = a[a.index('-derivedDataPath') + 1]
        reglages = dict(x.split('=', 1) for x in a if '=' in x and not x.startswith('-'))
        projet = open('project.yml').read()
        def lu(nom):
            return reglages.get(nom) or re.search(r'  Essai:\\n(?:.*\\n)*?\\s+%s: "?([^"\\n]+)"?' % nom, projet).group(1)
        app = os.path.join(dd, 'Build', 'Products', 'Release', 'Essai Inventee.app', 'Contents')
        os.makedirs(app, exist_ok=True)
        # Le code imbrique, comme celui de Sparkle : deux services XPC, une app, un executable, puis un autre cadre.
        b = os.path.join(app, 'Frameworks', 'Sparkle.framework', 'Versions', 'B')
        for d in ('XPCServices/Installer.xpc/Contents', 'XPCServices/Downloader.xpc/Contents', 'Updater.app/Contents'):
            os.makedirs(os.path.join(b, d), exist_ok=True)
        for f in ('Autoupdate', 'Sparkle'):
            open(os.path.join(b, f), 'w').close()
            os.chmod(os.path.join(b, f), 0o755)
        if not os.path.lexists(os.path.join(b, '..', 'Current')):
            os.symlink('B', os.path.join(b, '..', 'Current'))
        os.makedirs(os.path.join(app, 'Frameworks', 'Coeur.framework', 'Versions', 'A'), exist_ok=True)
        plistlib.dump({'CFBundleShortVersionString': lu('MARKETING_VERSION'),
                       'CFBundleVersion': reglages['CURRENT_PROJECT_VERSION'],
                       'SUFeedURL': lu('FLUX_MISES_A_JOUR'), 'SUPublicEDKey': lu('CLE_MISES_A_JOUR')},
                      open(os.path.join(app, 'Info.plist'), 'wb'))
        '''),
    'hdiutil': 'import sys\nopen(sys.argv[-1], "w").write("dmg invente " + " ".join(sys.argv[1:]))\n',
    'sign_update': 'import sys\nprint("U0lHTkFUVVJFLUlOVkVOVEVF")\n',
    'generate_keys': 'import os\nprint(os.environ["FAUSSE_CLE"])\n',
    'gh': 'import os, sys\nsys.exit(1 if sys.argv[1:3] == ["release", "view"] and not os.environ.get("FAUX_PUBLIEE") '
          'else 0)\n',
    'controles.py': 'import os, sys\nprint("trouve : " + os.environ.get("FAUX_TROUVE", "aucun"))\n'
                    'sys.exit(1 if os.environ.get("FAUX_TROUVE") else 0)\n',
}


def lire(chemin):
    with open(chemin, encoding='utf-8') as f:
        return f.read()


def ecrire(chemin, texte):
    with open(chemin, 'w', encoding='utf-8') as f:
        f.write(texte)


def git(depot, *args):
    return subprocess.run(['git', '-C', depot] + list(args), check=True, capture_output=True, text=True,
                          env=GIT_ENV).stdout.strip()


class Monde:
    """Un faux depot (et son origine), de fausses commandes, un dossier de produits."""

    def __init__(self, racine):
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
        self.origine = os.path.join(racine, 'origine.git')
        self.depot = os.path.join(racine, 'depot')
        subprocess.run(['git', 'init', '-q', '--bare', '-b', 'main', self.origine], check=True, env=GIT_ENV)
        subprocess.run(['git', 'clone', '-q', self.origine, self.depot], check=True, env=GIT_ENV,
                       capture_output=True)
        git(self.depot, 'config', 'user.name', 'Essai')
        git(self.depot, 'config', 'user.email', '0+essai@users.noreply.github.com')
        git(self.depot, 'checkout', '-q', '-b', 'main')
        ecrire(os.path.join(self.depot, 'project.yml'), PROJET)
        ecrire(os.path.join(self.depot, 'NOTES-VERSIONS.md'), NOTES)
        ecrire(os.path.join(self.depot, '.gitignore'), 'build/\n')
        for i in range(3):
            ecrire(os.path.join(self.depot, 'f%d.txt' % i), '%d\n' % i)
            git(self.depot, 'add', '-A')
            git(self.depot, 'commit', '-q', '-m', 'commit %d' % i)
        git(self.depot, 'push', '-q', 'origin', 'main')
        self.dd = os.path.join(racine, 'dd')
        self.env = {'GIT': 'git', 'XCODEGEN': self.bin + '/xcodegen', 'XCODEBUILD': self.bin + '/xcodebuild',
                    'HDIUTIL': self.bin + '/hdiutil', 'CODESIGN': self.bin + '/codesign', 'GH': self.bin + '/gh',
                    'SECURITY': self.bin + '/security', 'XCRUN': self.bin + '/xcrun', 'SPCTL': self.bin + '/spctl',
                    'SPARKLE_BIN': self.bin, 'CONTROLE_ANONYMISATION': self.bin + '/controles.py',
                    'TABLE_ANONYMISATION': self.journal}

    def appels(self):
        return lire(self.journal).splitlines() if os.path.exists(self.journal) else []

    def arguments(self, *extra):
        return P.arguments(['publier', '1.2.3', '--nom-app', 'Essai Inventee', '--fichier', 'Essai-Inventee',
                            '--depot-github', 'Exemple/essai', '--projet', 'Essai.xcodeproj', '--schema', 'Essai',
                            '--cible', 'Essai', '--textes', 'project.yml', '--sans-bureau', '--identite', IDENTITE,
                            '--etiquette', 'essai-v', '--flux', 'appcast.xml']
                           + list(extra))

    def publier(self, *extra, env=None):
        dedans = os.getcwd()
        os.chdir(self.depot)
        try:
            # Sans GIT_AUTHOR_* ni GIT_COMMITTER_* : le commit du flux prend l'auteur de la configuration du depot.
            e = {k: v for k, v in GIT_ENV.items() if not k.startswith(('GIT_AUTHOR', 'GIT_COMMITTER'))}
            e.update(FAUX_JOURNAL=self.journal, FAUSSE_CLE=CLE, DD=self.dd)
            e.update(env or {})
            with mock.patch.dict(os.environ, e, clear=True), contextlib.redirect_stdout(io.StringIO()):
                return P.publier(self.arguments(*extra), P.Outils(self.env),
                                 maintenant=datetime.datetime(2026, 10, 6, 12, 0, 0))
        finally:
            os.chdir(dedans)


class NumerosTests(unittest.TestCase):
    def test_version_valide(self):
        self.assertTrue(P.version_valide('1.0.0'))
        self.assertTrue(P.version_valide('12.30.4'))
        for v in ('1.0', 'v1.0.0', '1.0.0-beta', '1.0.0 ', ''):
            self.assertFalse(P.version_valide(v), v)

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

    def test_une_version_de_plus_en_tete(self):
        """Le flux garde toutes les versions publiees, la nouvelle en tete."""
        un = P.ajouter_au_flux(None, 'Essai Inventee', '1.2.2', self.item('1.2.2', 56))
        deux = P.ajouter_au_flux(un, 'Essai Inventee', '1.2.3', self.item('1.2.3', 57))
        s = '{%s}' % P.ESPACE_SPARKLE
        items = ET.fromstring(deux).findall('channel/item')
        self.assertEqual([i.find(s + 'shortVersionString').text for i in items], ['1.2.3', '1.2.2'])
        self.assertEqual([i.find(s + 'version').text for i in items], ['57', '56'])
        self.assertEqual(deux.replace(self.item('1.2.3', 57), ''), un, 'le reste du flux ne change pas')
        with self.assertRaises(P.Refus):
            P.ajouter_au_flux(deux, 'Essai Inventee', '1.2.2', self.item('1.2.2', 58))


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.dossier = os.path.realpath(tempfile.mkdtemp())
        self.m = Monde(self.dossier)

    def tearDown(self):
        shutil.rmtree(self.dossier)

    def refuse(self, *extra, env=None, motif, etiquette=''):
        with self.assertRaises(P.Refus) as r:
            self.m.publier(*extra, env=env)
        self.assertIn(motif, str(r.exception))
        appels = self.m.appels()
        self.assertFalse([a for a in appels if a.startswith('gh release create')], 'rien de publie')
        self.assertEqual(git(self.m.depot, 'tag', '-l'), etiquette, 'aucune etiquette nouvelle')
        self.assertEqual(git(self.m.origine, 'tag', '-l'), '', 'rien de pousse')
        return appels

    def commit(self, fichier, texte):
        ecrire(os.path.join(self.m.depot, fichier), texte)
        git(self.m.depot, 'commit', '-q', '-am', 'changement')
        git(self.m.depot, 'push', '-q', 'origin', 'main')

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
        controle = [a for a in appels if a.startswith('controles.py')][0]
        self.assertIn('NOTES-VERSIONS.md', controle)
        self.assertIn(os.path.join(sortie, 'appcast.xml'), controle)
        self.assertIn('project.yml', controle)
        self.assertEqual(git(self.m.depot, 'tag', '-l'), '')

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
        self.m.publier()
        appels = self.m.appels()
        self.assertEqual(git(self.m.origine, 'tag', '-l'), 'essai-v1.2.3', 'etiquette de l\'app, poussee')
        dmg = os.path.join(self.m.depot, 'build', 'publication', '1.2.3', 'Essai-Inventee-1.2.3.dmg')
        cree = [a for a in appels if a.startswith('gh release create')]
        self.assertEqual(len(cree), 1)
        self.assertIn('essai-v1.2.3 %s -R Exemple/essai' % dmg, cree[0], 'le .dmg seul')
        self.assertIn('--title Essai Inventee 1.2.3', cree[0])
        self.assertIn('sign_update -p ' + dmg, appels, 'cle du trousseau')
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
        controle = [a for a in appels if a.startswith('controles.py')][0]
        self.assertIn('message-commit.txt', controle, 'le message passe aussi le controle')
        self.assertIn('https://github.com/Exemple/essai/releases/download/essai-v1.2.3/Essai-Inventee-1.2.3.dmg', flux)
        self.assertIn('<li>A new <code>command</code>.</li>', flux)
        with open(os.path.join(self.m.dd, 'Build', 'Products', 'Release', 'Essai Inventee.app', 'Contents',
                               'Info.plist'), 'rb') as f:
            info = plistlib.load(f)
        self.assertEqual(info['CFBundleVersion'], '3')

    def signatures(self):
        """Les signatures du code, dans l'ordre : le chemin signe, depuis le dossier des produits."""
        base = os.path.join(self.m.dd, 'Build', 'Products', 'Release') + '/'
        return [a[a.index(base) + len(base):].replace('Essai Inventee.app/Contents/Frameworks/', '')
                for a in self.m.appels() if a.startswith('codesign --force')]

    def test_signature_du_code(self):
        """Le code imbrique d'abord, du plus profond au moins profond, chaque cadre apres son contenu, l'app en
        dernier ; avec l'identite donnee, le runtime renforce, les droits gardes ; puis l'exigence de signature."""
        self.m.publier()
        self.assertEqual(self.signatures(), [
            'Coeur.framework',
            'Sparkle.framework/Versions/Current/XPCServices/Downloader.xpc',
            'Sparkle.framework/Versions/Current/XPCServices/Installer.xpc',
            'Sparkle.framework/Versions/Current/Autoupdate',
            'Sparkle.framework/Versions/Current/Updater.app',
            'Sparkle.framework',
            'Essai Inventee.app'])
        appels = [a for a in self.m.appels() if a.startswith('codesign --force')]
        for a in appels:
            self.assertIn('--sign %s --options runtime --preserve-metadata=entitlements --timestamp=none' % IDENTITE, a)
            self.assertNotIn('--keychain', a)
        sortie = os.path.join(self.m.depot, 'build', 'publication', '1.2.3')
        self.assertIn('certificate leaf', lire(os.path.join(sortie, 'exigence.txt')))

    def test_signature_dans_un_trousseau_a_part(self):
        """En repetition, un certificat d'essai dans un trousseau a part : codesign et security y cherchent."""
        sortie = os.path.join(self.dossier, 'repetition')
        self.m.publier('--repetition', sortie, '--url-base', 'http://127.0.0.1:8123', '--cle-privee', 'cle.txt',
                       '--cle-publique', AUTRE_CLE, '--sans-tests', '--trousseau', '/tmp/essai.keychain-db')
        appels = self.m.appels()
        self.assertIn('security find-identity -p codesigning /tmp/essai.keychain-db', appels)
        signe = [a for a in appels if a.startswith('codesign --force')]
        self.assertEqual(len(signe), 7)
        self.assertTrue(all('--keychain /tmp/essai.keychain-db' in a for a in signe))

    def test_refus_identite_absente(self):
        appels = self.refuse(env={'FAUSSE_IDENTITE': 'Autre Signing'}, motif='identite de signature')
        self.assertFalse([a for a in appels if a.startswith('xcodebuild')], 'rien de compile')

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
            'codesign --force --sign %s --timestamp %s' % (IDENTITE, dmg),
            'xcrun notarytool submit %s --keychain-profile profil-essai --wait' % dmg,
            'xcrun stapler staple %s' % dmg,
            'spctl --assess --type open --context context:primary-signature --verbose %s' % dmg,
            'sign_update -p %s' % dmg])

    def test_refus_version_differente(self):
        self.commit('project.yml', PROJET.replace('"1.2.3"', '"1.2.4"'))
        appels = self.refuse(motif='MARKETING_VERSION')
        self.assertFalse([a for a in appels if a.startswith('xcodebuild')], 'rien de compile')

    def test_refus_arbre_pas_propre(self):
        ecrire(os.path.join(self.m.depot, 'oubli.txt'), 'x\n')
        self.refuse(motif='propre')

    def test_refus_etiquette_existante(self):
        git(self.m.depot, 'tag', 'essai-v1.2.3')
        self.refuse(motif='existe deja', etiquette='essai-v1.2.3')

    def test_etiquette_d_une_autre_app(self):
        """L'etiquette d'une autre app du meme depot, au meme numero, ne gene pas."""
        git(self.m.depot, 'tag', 'autre-v1.2.3')
        git(self.m.depot, 'push', '-q', 'origin', 'autre-v1.2.3')
        self.m.publier()
        self.assertEqual(git(self.m.origine, 'tag', '-l').split(), ['autre-v1.2.3', 'essai-v1.2.3'])

    def test_ajout_a_un_flux_existant(self):
        """Le flux du depot garde les versions d'avant : la nouvelle s'ajoute en tete."""
        ancien = P.ajouter_au_flux(None, 'Essai Inventee', '1.2.2', P.item_flux(
            '1.2.2', 2, 'https://github.com/Exemple/essai/releases/download/essai-v1.2.2/Essai-Inventee-1.2.2.dmg',
            10, 'QU5DSUVOTkU=', '26.0', '<p>Ancienne</p>', datetime.datetime(2026, 10, 1)))
        ecrire(os.path.join(self.m.depot, 'appcast.xml'), ancien)
        git(self.m.depot, 'add', 'appcast.xml')
        git(self.m.depot, 'commit', '-q', '-m', 'flux')
        git(self.m.depot, 'push', '-q', 'origin', 'main')
        self.m.publier()
        s = '{%s}' % P.ESPACE_SPARKLE
        items = ET.fromstring(lire(os.path.join(self.m.depot, 'appcast.xml'))).findall('channel/item')
        self.assertEqual([i.find(s + 'shortVersionString').text for i in items], ['1.2.3', '1.2.2'])
        self.assertEqual(items[0].find(s + 'version').text, '4')

    def test_refus_auteur_sans_adresse_noreply(self):
        """Le commit du flux est public : son auteur porte l'adresse noreply de GitHub, jamais une vraie."""
        git(self.m.depot, 'config', 'user.email', 'essai@example.invalid')
        appels = self.refuse(motif='noreply')
        self.assertFalse([a for a in appels if a.startswith('xcodebuild')], 'rien de compile')

    def test_refus_version_deja_dans_le_flux(self):
        ancien = P.ajouter_au_flux(None, 'Essai Inventee', '1.2.3', P.item_flux(
            '1.2.3', 2, 'https://exemple.invalid/x.dmg', 10, 'QQ==', '26.0', '', datetime.datetime(2026, 10, 1)))
        ecrire(os.path.join(self.m.depot, 'appcast.xml'), ancien)
        git(self.m.depot, 'add', 'appcast.xml')
        git(self.m.depot, 'commit', '-q', '-m', 'flux')
        git(self.m.depot, 'push', '-q', 'origin', 'main')
        appels = self.refuse(motif='deja dans le flux')
        self.assertFalse([a for a in appels if a.startswith('xcodebuild')], 'rien de compile')

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

    def test_refus_deja_publiee(self):
        self.refuse(env={'FAUX_PUBLIEE': '1'}, motif='deja publiee')

    def test_refus_notes_absentes(self):
        self.commit('NOTES-VERSIONS.md', NOTES.replace('## 1.2.3', '## 1.2.1'))
        self.refuse(motif='pas de section 1.2.3')

    def test_refus_cle_du_trousseau_differente(self):
        appels = self.refuse(env={'FAUSSE_CLE': AUTRE_CLE}, motif='trousseau')
        self.assertIn('generate_keys -p', appels)

    def test_refus_tests_en_echec(self):
        appels = self.refuse('--test', 'exit 3', motif='tests en echec')
        self.assertFalse([a for a in appels if a.startswith('xcodebuild')], 'rien de compile')

    def test_refus_sans_tests_hors_repetition(self):
        self.refuse('--sans-tests', motif='repetition')

    def test_refus_controle_d_anonymisation(self):
        appels = self.refuse(env={'FAUX_TROUVE': '1'}, motif='anonymisation')
        self.assertTrue([a for a in appels if a.startswith('sign_update')], 'le controle passe apres la signature')
        self.assertFalse(os.path.exists(os.path.join(self.m.depot, 'appcast.xml')), 'le flux du depot ne change pas')
        self.assertEqual(git(self.m.depot, 'status', '--porcelain'), '')

    def test_controle_absent(self):
        self.m.env['CONTROLE_ANONYMISATION'] = os.path.join(self.dossier, 'absent.py')
        self.m.publier()
        self.assertEqual(git(self.m.origine, 'tag', '-l'), 'essai-v1.2.3')

    def test_arguments_de_la_repetition(self):
        with mock.patch('sys.stderr'):
            with self.assertRaises(SystemExit):
                P.arguments(['publier', '1.2.3', '--nom-app', 'A', '--fichier', 'A', '--depot-github', 'E/a',
                             '--projet', 'A.xcodeproj', '--schema', 'A', '--cible', 'A', '--repetition', 'x'])
            with self.assertRaises(SystemExit):
                P.arguments(['publier', '1.2.3', '--nom-app', 'A', '--fichier', 'A', '--depot-github', 'E/a',
                             '--projet', 'A.xcodeproj', '--schema', 'A', '--cible', 'A', '--repetition', 'x',
                             '--url-base', 'http://127.0.0.1:1', '--cle-privee', 'k'])
            with self.assertRaises(SystemExit):
                P.arguments(['publier', '1.2.3', '--nom-app', 'A', '--fichier', 'A', '--depot-github', 'E/a',
                             '--projet', 'A.xcodeproj', '--schema', 'A', '--cible', 'A', '--cle-privee', 'k',
                             '--cle-publique', CLE])


if __name__ == '__main__':
    unittest.main()
