#!/usr/bin/env python3
"""Publication d'une version de l'app (spec du deploiement, section 3), appelee par publier.sh.

  publication.py publier X.Y.Z --nom-app N --fichier F --depot-github D --projet P --schema S --cible C
                 --identite NOM --etiquette PREFIXE --flux CHEMIN [--test CMD]... [--textes FICHIER]... [--sans-bureau]
                 [--repetition DOSSIER --url-base URL [--cle-privee FICHIER --cle-publique CLE] [--trousseau T]
                  [--sans-tests]]

Dans l'ordre, et rien n'est publie si une etape echoue :
  1. les verifications : la version est X.Y.Z, celle de MARKETING_VERSION ; l'arbre est propre ; main, a jour avec
     GitHub ; l'etiquette de l'app (PREFIXE suivi de X.Y.Z, par exemple maillage-v1.0.0) et sa version publiee
     n'existent pas, ni la version dans le flux ; l'app lit le flux a l'adresse brute du depot
     (https://raw.githubusercontent.com/<depot>/main/<CHEMIN>) ; NOTES-VERSIONS.md a sa section ; la cle publique
     de l'app est celle du trousseau ; l'identite de signature y est ; les tests passent ;
  2. les numeros : la version, et le numero de compilation, le nombre de commits de main ;
  3. la compilation Release, ad hoc (une equipe de Local.xcconfig n'y entre pas), puis signee avec l'identite
     donnee (un certificat auto-signe stable, plus tard un Developer ID) : le code imbrique d'abord, le runtime
     renforce, les droits gardes ; l'exigence de signature (codesign -d -r-) est ecrite dans exigence.txt ;
  4. le .dmg (hdiutil) : l'app et un raccourci vers Applications ; avec NOTARISER=1 seulement (desactive par
     defaut), le .dmg signe, soumis a Apple (notarytool, profil PROFIL_NOTARISATION du trousseau), agrafe
     (stapler) et evalue (spctl) ;
  5. la signature Ed25519 du .dmg (sign_update de Sparkle, cle du trousseau), puis le flux : le fichier CHEMIN du
     depot (appcast.xml), qui garde toutes les versions publiees, la nouvelle en tete ; l'adresse de chaque .dmg est
     celle de sa version publiee ;
  6. le controle d'anonymisation (prive), s'il est present, sur les notes, le flux, le message du commit du flux et
     les textes de l'app ;
  7. l'etiquette, poussee, puis la version publiee sur GitHub (gh release create), avec le .dmg ; puis le flux,
     commite sur main (git add de ce seul fichier) et pousse aussitot ;
  8. le .dmg copie sur le Bureau (sauf --sans-bureau).

En repetition (--repetition DOSSIER), ni GitHub, ni etiquette, ni Bureau : la branche peut etre une autre que main ;
--url-base tient lieu des deux adresses de GitHub (le flux : <URL>/<depot>/main/<CHEMIN> ; un .dmg :
<URL>/<depot>/releases/download/<etiquette>/<fichier>), comme les servirait un serveur local ; le flux est commite
dans la copie, sans etre pousse ; les produits vont dans DOSSIER. Avec --cle-privee et --cle-publique, une paire
d'essai, sans le trousseau : l'app porte cette cle publique, et le .dmg est signe avec la cle privee du fichier.
Sans elles, la cle du trousseau, comme pour la vraie publication. Avec --trousseau, l'identite de signature est
cherchee dans ce trousseau a part (un certificat d'essai), jamais dans celui de la session.

Les commandes externes se remplacent par l'environnement, pour les tests : XCODEGEN, XCODEBUILD, HDIUTIL, CODESIGN,
SECURITY, XCRUN, SPCTL, SPARKLE_BIN (dossier de sign_update et generate_keys), GH, GIT, CONTROLE_ANONYMISATION et
TABLE_ANONYMISATION. Aucun identifiant Apple, Team ID ni mot de passe n'est ecrit ici : la notarisation lit le
profil que notarytool store-credentials a range dans le trousseau.
"""
import argparse
import datetime
import html
import os
import plistlib
import re
import shlex
import shutil
import subprocess
import sys

VERSION = re.compile(r'^\d+\.\d+\.\d+$')
ESPACE_SPARKLE = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
PRIVE = os.path.expanduser('~/Dev/maillage-thread/.superpowers/anonymisation')


class Refus(Exception):
    """Une verification qui arrete la publication, avant tout changement."""


def lire(chemin):
    with open(chemin, encoding='utf-8') as f:
        return f.read()


def ecrire(chemin, texte):
    with open(chemin, 'w', encoding='utf-8') as f:
        f.write(texte)


# --- les numeros ---------------------------------------------------------------------------------------------

def version_valide(v):
    return bool(VERSION.match(v))


def bloc_cible(projet_yml, cible):
    """Les lignes de la cible `cible` de project.yml (XcodeGen) : de « targets: », la cible a deux espaces de
    retrait, jusqu'a la suivante."""
    lignes = lire(projet_yml).splitlines()
    try:
        debut = lignes.index('targets:')
        i = lignes.index('  %s:' % cible, debut)
    except ValueError:
        raise Refus('cible %s introuvable dans %s' % (cible, projet_yml))
    bloc = []
    for l in lignes[i + 1:]:
        if re.match(r'^ {0,2}\S', l):
            break
        bloc.append(l)
    return bloc


def reglage(projet_yml, cible, nom):
    """La valeur d'un reglage de la cible (« NOM: valeur », guillemets otes)."""
    for l in bloc_cible(projet_yml, cible):
        m = re.match(r'^\s+%s:\s*(.+?)\s*$' % re.escape(nom), l)
        if m:
            return m.group(1).strip('"')
    raise Refus('%s absent de la cible %s' % (nom, cible))


def systeme_minimum(projet_yml):
    """La version minimale de macOS (options.deploymentTarget.macOS)."""
    m = re.search(r'^options:\n(?:  .*\n)*?  deploymentTarget:\n    macOS: "?([\d.]+)"?', lire(projet_yml), re.M)
    if not m:
        raise Refus('deploymentTarget.macOS absent de ' + projet_yml)
    return m.group(1)


def numero_compilation(depot, git='git'):
    """Le numero de compilation (CFBundleVersion), que compare Sparkle : le nombre de commits jusqu'a HEAD."""
    return int(subprocess.run([git, '-C', depot, 'rev-list', '--count', 'HEAD'], check=True, capture_output=True,
                              text=True).stdout.strip())


# --- les notes et le flux ------------------------------------------------------------------------------------

def notes(chemin, version):
    """La section « ## X.Y.Z » de NOTES-VERSIONS.md, sans son titre."""
    texte = lire(chemin)
    m = re.search(r'^## %s[ \t]*\n(.*?)(?=^## |\Z)' % re.escape(version), texte, re.M | re.S)
    if not m or not m.group(1).strip():
        raise Refus('pas de section %s dans %s' % (version, chemin))
    return m.group(1).strip() + '\n'


def en_ligne(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', t)
    return re.sub(r'`(.+?)`', r'<code>\1</code>', t)


def notes_html(texte):
    """Les notes en HTML simple, pour la fenetre de Sparkle : paragraphes, listes « - », gras et code."""
    sortie, liste, para = [], [], []

    def fermer():
        if para:
            sortie.append('<p>%s</p>' % en_ligne(' '.join(para)))
            para.clear()
        if liste:
            sortie.append('<ul>%s</ul>' % ''.join('<li>%s</li>' % en_ligne(e) for e in liste))
            liste.clear()

    for l in texte.splitlines():
        s = l.strip()
        if not s:
            fermer()
        elif s.startswith('- '):
            if para:
                fermer()
            liste.append(s[2:])
        elif liste and l.startswith('  '):
            liste[-1] += ' ' + s
        else:
            if liste:
                fermer()
            para.append(s)
    fermer()
    return '\n'.join(sortie)


def item_flux(version, numero, url, taille, signature, systeme, notes_html_, date):
    """Une version dans le flux de Sparkle : un <item>, avec son retrait et sa fin de ligne."""
    return '''    <item>
      <title>%s</title>
      <pubDate>%s</pubDate>
      <sparkle:version>%d</sparkle:version>
      <sparkle:shortVersionString>%s</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>%s</sparkle:minimumSystemVersion>
      <description><![CDATA[
%s
]]></description>
      <enclosure url="%s" length="%d" type="application/octet-stream" sparkle:edSignature="%s"/>
    </item>
''' % (html.escape(version), date.strftime('%a, %d %b %Y %H:%M:%S +0000'), numero, html.escape(version),
       html.escape(systeme), notes_html_.replace(']]>', ']]&gt;'), html.escape(url), taille, html.escape(signature))


def ajouter_au_flux(existant, titre, version, item):
    """Le flux (appcast.xml) avec une version de plus, en tete : il garde toutes les versions publiees. Sans flux
    existant (None), un flux neuf. Refus si la version y est deja."""
    if existant is None:
        return '''<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="%s">
  <channel>
    <title>%s</title>
%s  </channel>
</rss>
''' % (ESPACE_SPARKLE, html.escape(titre), item)
    if '<sparkle:shortVersionString>%s</sparkle:shortVersionString>' % html.escape(version) in existant:
        raise Refus('la version %s est deja dans le flux' % version)
    i = existant.find('    <item>')
    if i < 0:
        i = existant.find('  </channel>')
    if i < 0:
        raise Refus('flux illisible : ni <item>, ni </channel>')
    return existant[:i] + item + existant[i:]


def message_flux(nom_app, version):
    """Le message du commit du flux, en francais sans accents."""
    return ('Publier %s %s dans le flux des mises a jour\n\n'
            'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n' % (nom_app, version))


# --- la publication ------------------------------------------------------------------------------------------

class Outils:
    """Les commandes externes, remplacables par l'environnement (tests)."""

    def __init__(self, env=None):
        e = os.environ if env is None else env
        sparkle = e.get('SPARKLE_BIN', '')
        self.git = e.get('GIT', 'git')
        self.xcodegen = e.get('XCODEGEN', 'xcodegen')
        self.xcodebuild = e.get('XCODEBUILD', 'xcodebuild')
        self.hdiutil = e.get('HDIUTIL', 'hdiutil')
        self.codesign = e.get('CODESIGN', 'codesign')
        self.security = e.get('SECURITY', 'security')
        self.xcrun = e.get('XCRUN', 'xcrun')
        self.spctl = e.get('SPCTL', 'spctl')
        self.gh = e.get('GH', 'gh')
        self.sign_update = os.path.join(sparkle, 'sign_update') if sparkle else 'sign_update'
        self.generate_keys = os.path.join(sparkle, 'generate_keys') if sparkle else 'generate_keys'
        self.controle = e.get('CONTROLE_ANONYMISATION', os.path.join(PRIVE, 'outils', 'controles.py'))
        self.table = e.get('TABLE_ANONYMISATION', os.path.join(PRIVE, 'execution', 'table.json'))


def lancer(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def dire(texte):
    print(texte, flush=True)


def verifier(a, o, racine_git, version):
    """L'etape 1 : tout ce qui doit tenir avant de compiler. Leve Refus."""
    if not version_valide(version):
        raise Refus('version attendue sous la forme X.Y.Z : ' + version)
    marketing = reglage('project.yml', a.cible, 'MARKETING_VERSION')
    if marketing != version:
        raise Refus('MARKETING_VERSION de project.yml : %s, pas %s' % (marketing, version))
    if lancer([o.git, 'status', '--porcelain']).strip():
        raise Refus("l'arbre n'est pas propre (git status)")
    auteur = subprocess.run([o.git, 'config', 'user.email'], capture_output=True, text=True).stdout.strip()
    if not auteur.endswith('@users.noreply.github.com'):
        raise Refus("le commit du flux est public : l'adresse de l'auteur (git config user.email) doit etre "
                    "l'adresse noreply de GitHub")
    etiquette = a.etiquette + version
    if lancer([o.git, 'tag', '-l', etiquette]).strip():
        raise Refus("l'etiquette %s existe deja" % etiquette)
    if os.path.exists(a.flux):
        ajouter_au_flux(lire(a.flux), a.nom_app, version, '')
    adresse = 'https://raw.githubusercontent.com/%s/main/%s' % (a.depot_github, chemin_depot(a.flux, racine_git))
    if reglage('project.yml', a.cible, 'FLUX_MISES_A_JOUR') != adresse:
        raise Refus('FLUX_MISES_A_JOUR de project.yml : %s attendu (le flux du depot)' % adresse)
    if not a.repetition:
        branche = lancer([o.git, 'rev-parse', '--abbrev-ref', 'HEAD']).strip()
        if branche != 'main':
            raise Refus('la publication se fait depuis main, pas ' + branche)
        lancer([o.git, 'fetch', '-q', 'origin', 'main'])
        if lancer([o.git, 'rev-parse', 'HEAD']) != lancer([o.git, 'rev-parse', 'origin/main']):
            raise Refus("main n'est pas a jour avec GitHub (origin/main)")
        if lancer([o.git, 'ls-remote', '--tags', 'origin', etiquette]).strip():
            raise Refus("l'etiquette %s existe deja sur GitHub" % etiquette)
        if subprocess.run([o.gh, 'release', 'view', etiquette, '-R', a.depot_github],
                          capture_output=True).returncode == 0:
            raise Refus('la version %s est deja publiee sur GitHub' % etiquette)
    notes('NOTES-VERSIONS.md', version)
    cle = reglage('project.yml', a.cible, 'CLE_MISES_A_JOUR')
    if a.cle_publique:
        cle_attendue = a.cle_publique
    else:
        cle_attendue = lancer([o.generate_keys, '-p']).strip()
        if cle != cle_attendue:
            raise Refus('la cle publique de project.yml (CLE_MISES_A_JOUR) differe de celle du trousseau')
    if not re.match(r'^[A-Za-z0-9+/]{43}=$', cle_attendue or ''):
        raise Refus('cle publique Ed25519 invalide : %s' % cle_attendue)
    if a.sans_tests and not a.repetition:
        raise Refus('--sans-tests seulement en repetition')
    identites = lancer([o.security, 'find-identity', '-p', 'codesigning'] + ([a.trousseau] if a.trousseau else []))
    if '"%s"' % a.identite not in identites:
        raise Refus('identite de signature introuvable dans le trousseau : %s' % a.identite)
    if notariser() and not os.environ.get('PROFIL_NOTARISATION'):
        raise Refus('NOTARISER=1 demande PROFIL_NOTARISATION, le profil de notarytool store-credentials')
    return marketing


def chemin_depot(chemin, racine_git):
    """Le chemin d'un fichier depuis la racine du depot (celui de l'adresse brute du flux)."""
    return os.path.relpath(os.path.realpath(chemin), os.path.realpath(racine_git))


def notariser():
    """La notarisation (Developer ID), desactivee par defaut : NOTARISER=1 l'active."""
    return os.environ.get('NOTARISER') == '1'


def code_imbrique(app):
    """Le code a signer avant l'app, dans l'ordre : pour chaque cadre de Contents/Frameworks, ce qu'il contient
    (services XPC, apps, executables), du plus profond au moins profond, puis le cadre lui-meme."""
    cadres = os.path.join(app, 'Contents', 'Frameworks')
    liste = []
    for nom in sorted(os.listdir(cadres)) if os.path.isdir(cadres) else []:
        cadre = os.path.join(cadres, nom)
        if not nom.endswith('.framework'):
            liste.append(cadre)
            continue
        courante = os.path.join(cadre, 'Versions', 'Current')
        dedans = []
        if os.path.isdir(courante):
            for racine, dossiers, fichiers in os.walk(courante):
                for d in list(dossiers):
                    if d.endswith(('.app', '.xpc')):
                        dedans.append(os.path.join(racine, d))
                        dossiers.remove(d)
            for f in sorted(os.listdir(courante)):
                p = os.path.join(courante, f)
                if f != nom[:-len('.framework')] and os.path.isfile(p) and not os.path.islink(p) and os.access(p, os.X_OK):
                    dedans.append(p)
        liste += sorted(dedans, key=lambda p: (-p.count('/'), p))
        liste.append(cadre)
    return liste


def signer(a, o, chemin, droits=True):
    """Signe un code avec l'identite de la publication : runtime renforce, droits gardes, horodatage si notarise."""
    cmd = [o.codesign, '--force', '--sign', a.identite]
    if droits:
        cmd += ['--options', 'runtime', '--preserve-metadata=entitlements']
    cmd.append('--timestamp' if notariser() else '--timestamp=none')
    if a.trousseau:
        cmd += ['--keychain', a.trousseau]
    lancer(cmd + [chemin])


def publier(a, o=None, maintenant=None):
    o = o or Outils()
    version = a.version
    racine_git = lancer([o.git, 'rev-parse', '--show-toplevel']).strip()
    verifier(a, o, racine_git, version)
    sortie = os.path.abspath(a.repetition or os.path.join('build', 'publication', version))
    os.makedirs(sortie, exist_ok=True)
    dd = os.environ.get('DD', os.path.expanduser('~/Library/Developer/Xcode/DerivedData/%s-publication'
                                                  % a.fichier.lower()))
    if not a.sans_tests:
        for i, t in enumerate(a.test, 1):
            journal = os.path.join(sortie, 'tests-%d.log' % i)
            dire('tests %d/%d : %s (journal : %s)' % (i, len(a.test), t, journal))
            with open(journal, 'w') as j:
                if subprocess.run(t, shell=True, stdout=j, stderr=subprocess.STDOUT,
                                  env=dict(os.environ, DD=dd)).returncode != 0:
                    raise Refus('tests en echec : %s (voir %s)' % (t, journal))

    # 2. les numeros
    numero = numero_compilation(racine_git, o.git)
    systeme = systeme_minimum('project.yml')
    dire('version %s, numero de compilation %d, macOS %s minimum' % (version, numero, systeme))

    # 3. la compilation Release, ad hoc
    etiquette = a.etiquette + version
    chemin_flux_depot = chemin_depot(a.flux, racine_git)
    if a.repetition:
        flux = '%s/%s/main/%s' % (a.url_base, a.depot_github, chemin_flux_depot)
        url_dmg = '%s/%s/releases/download/%s' % (a.url_base, a.depot_github, etiquette)
    else:
        flux = None
        url_dmg = 'https://github.com/%s/releases/download/%s' % (a.depot_github, etiquette)
    reglages = ['CURRENT_PROJECT_VERSION=%d' % numero, 'CODE_SIGN_IDENTITY=-', 'DEVELOPMENT_TEAM=',
                'CODE_SIGN_STYLE=Manual']
    if a.repetition:
        reglages += ['FLUX_MISES_A_JOUR=' + flux]
    if a.cle_publique:
        reglages += ['CLE_MISES_A_JOUR=' + a.cle_publique]
    lancer([o.xcodegen, 'generate', '--quiet'])
    with open(os.path.join(sortie, 'compilation.log'), 'w') as j:
        if subprocess.run([o.xcodebuild, '-project', a.projet, '-scheme', a.schema, '-configuration', 'Release',
                           '-destination', 'generic/platform=macOS', '-derivedDataPath', dd] + reglages + ['build'],
                          stdout=j, stderr=subprocess.STDOUT).returncode != 0:
            raise Refus('compilation en echec (voir %s)' % j.name)
    app = os.path.join(dd, 'Build', 'Products', 'Release', a.nom_app + '.app')
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f:
        info = plistlib.load(f)
    attendu = {'CFBundleShortVersionString': version, 'CFBundleVersion': str(numero),
               'SUPublicEDKey': a.cle_publique or reglage('project.yml', a.cible, 'CLE_MISES_A_JOUR'),
               'SUFeedURL': flux or reglage('project.yml', a.cible, 'FLUX_MISES_A_JOUR')}
    for cle, valeur in attendu.items():
        if info.get(cle) != valeur:
            raise Refus('Info.plist de l\'app compilee : %s = %r, attendu %r' % (cle, info.get(cle), valeur))
    for chemin in code_imbrique(app) + [app]:
        signer(a, o, chemin)
    lancer([o.codesign, '--verify', '--deep', '--strict', app])
    exigence = subprocess.run([o.codesign, '-d', '-r-', app], check=True, capture_output=True,
                              text=True).stdout.strip()
    ecrire(os.path.join(sortie, 'exigence.txt'), exigence + '\n')
    dire('exigence de signature : ' + exigence)

    # 4. le .dmg
    nom_dmg = '%s-%s.dmg' % (a.fichier, version)
    dmg = os.path.join(sortie, nom_dmg)
    scene = os.path.join(sortie, 'dmg')
    shutil.rmtree(scene, ignore_errors=True)
    os.makedirs(scene)
    lancer(['ditto', app, os.path.join(scene, a.nom_app + '.app')])
    os.symlink('/Applications', os.path.join(scene, 'Applications'))
    if os.path.exists(dmg):
        os.remove(dmg)
    lancer([o.hdiutil, 'create', '-quiet', '-volname', '%s %s' % (a.nom_app, version), '-srcfolder', scene,
            '-fs', 'HFS+', '-format', 'UDZO', dmg])
    shutil.rmtree(scene)
    if notariser():
        # Avant la signature Ed25519 : l'agrafe change le .dmg.
        signer(a, o, dmg, droits=False)
        lancer([o.xcrun, 'notarytool', 'submit', dmg, '--keychain-profile', os.environ['PROFIL_NOTARISATION'],
                '--wait'])
        lancer([o.xcrun, 'stapler', 'staple', dmg])
        lancer([o.spctl, '--assess', '--type', 'open', '--context', 'context:primary-signature', '--verbose', dmg])
        dire('notarise et agrafe : ' + dmg)

    # 5. la signature, puis le flux
    signe = [o.sign_update] + (['--ed-key-file', a.cle_privee] if a.cle_privee else []) + ['-p', dmg]
    signature = lancer(signe).strip()
    texte_notes = notes('NOTES-VERSIONS.md', version)
    item = item_flux(version, numero, '%s/%s' % (url_dmg, nom_dmg), os.path.getsize(dmg), signature, systeme,
                     notes_html(texte_notes), maintenant or datetime.datetime.utcnow())
    xml = ajouter_au_flux(lire(a.flux) if os.path.exists(a.flux) else None, a.nom_app, version, item)
    # Le nouveau flux, d'abord a cote : il n'entre dans le depot qu'apres le controle.
    chemin_flux = os.path.join(sortie, 'appcast.xml')
    ecrire(chemin_flux, xml)
    chemin_notes = os.path.join(sortie, 'notes.md')
    ecrire(chemin_notes, texte_notes)
    chemin_message = os.path.join(sortie, 'message-commit.txt')
    ecrire(chemin_message, message_flux(a.nom_app, version))
    dire('signe : %s (%d octets) ; flux : %s' % (dmg, os.path.getsize(dmg), chemin_flux))

    # 6. le controle d'anonymisation, s'il est present (prive)
    if os.path.exists(o.controle) and os.path.exists(o.table):
        textes = ['NOTES-VERSIONS.md', chemin_flux, chemin_notes, chemin_message] + a.textes
        r = subprocess.run(['/usr/bin/python3', o.controle, 'fichiers', '--table', o.table] + textes,
                           capture_output=True, text=True)
        dire('controle d\'anonymisation : ' + ' ; '.join(r.stdout.strip().splitlines()))
        if r.returncode != 0:
            raise Refus("le controle d'anonymisation a trouve des donnees reelles : rien n'est publie")
    else:
        dire("controle d'anonymisation absent de ce Mac : saute")

    # 7. la publication : l'etiquette et la version publiee, avec le .dmg ; puis le flux, commite et pousse
    if not a.repetition:
        lancer([o.git, 'tag', etiquette])
        lancer([o.git, 'push', 'origin', etiquette])
        lancer([o.gh, 'release', 'create', etiquette, dmg, '-R', a.depot_github, '--verify-tag',
                '--title', '%s %s' % (a.nom_app, version), '--notes-file', chemin_notes])
        dire('publie : https://github.com/%s/releases/tag/%s' % (a.depot_github, etiquette))
    shutil.copyfile(chemin_flux, a.flux)
    lancer([o.git, 'add', a.flux])
    lancer([o.git, 'commit', '-q', '-F', chemin_message])
    if a.repetition:
        dire('repetition : flux commite dans la copie (%s), ni etiquette, ni GitHub, ni Bureau ; produits dans %s'
             % (chemin_flux_depot, sortie))
        return sortie
    lancer([o.git, 'push', 'origin', 'main'])
    dire('flux commite et pousse sur main : https://raw.githubusercontent.com/%s/main/%s'
         % (a.depot_github, chemin_flux_depot))

    # 8. la remise
    if not a.sans_bureau:
        shutil.copy2(dmg, os.path.expanduser('~/Desktop'))
        dire('copie sur le Bureau : ' + nom_dmg)
    return sortie


def arguments(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sous = p.add_subparsers(dest='commande', required=True)
    q = sous.add_parser('publier')
    q.add_argument('version')
    for nom in ('--nom-app', '--fichier', '--depot-github', '--projet', '--schema', '--cible'):
        q.add_argument(nom, required=True)
    q.add_argument('--test', action='append', default=[], help='commande de tests (shell), dans l\'ordre')
    q.add_argument('--textes', action='append', default=[], help="textes de l'app pour le controle d'anonymisation")
    q.add_argument('--identite', required=True, help='nom du certificat de signature, dans le trousseau')
    q.add_argument('--etiquette', required=True, help="debut de l'etiquette de l'app, suivi de X.Y.Z (maillage-v)")
    q.add_argument('--flux', required=True, help='le flux du depot (appcast.xml), depuis le dossier de publier.sh')
    q.add_argument('--trousseau', help='en repetition : un trousseau a part, ou chercher l\'identite')
    q.add_argument('--sans-bureau', action='store_true')
    q.add_argument('--repetition', metavar='DOSSIER')
    q.add_argument('--url-base')
    q.add_argument('--cle-privee')
    q.add_argument('--cle-publique')
    q.add_argument('--sans-tests', action='store_true')
    a = p.parse_args(argv)
    if a.repetition and not a.url_base:
        p.error('--repetition demande --url-base')
    if bool(a.cle_privee) != bool(a.cle_publique):
        p.error('--cle-privee et --cle-publique vont ensemble')
    if not a.repetition and (a.url_base or a.cle_privee or a.trousseau):
        p.error('--url-base, --cle-privee, --cle-publique et --trousseau seulement en repetition')
    return a


def main(argv=None):
    a = arguments(sys.argv[1:] if argv is None else argv)
    try:
        publier(a)
    except Refus as e:
        print('refus : %s' % e, file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:
        print('echec : %s (code %d)\n%s' % (' '.join(map(shlex.quote, e.cmd)), e.returncode, (e.stderr or '')[-2000:]),
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
