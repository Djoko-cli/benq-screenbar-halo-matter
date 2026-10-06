#!/usr/bin/env python3
"""Publication d'une version de l'app (spec du deploiement, section 3), appelee par publier.sh.

  publication.py publier X.Y.Z --nom-app N --fichier F --depot-github D --projet P --schema S --cible C
                 --identite NOM --auteur NOM --etiquette PREFIXE --flux CHEMIN --licence FICHIER [--test CMD]...
                 [--textes FICHIER]... [--sans-bureau]
                 [--repetition DOSSIER --url-base URL [--cle-privee FICHIER --cle-publique CLE] [--trousseau T]
                  [--sans-tests]]

Dans l'ordre :
  1. les verifications, avant tout test et toute compilation : la version est X.Y.Z, celle de MARKETING_VERSION ;
     l'arbre est propre ; l'auteur et le committer du commit du flux (git var) portent le nom --auteur et l'adresse
     noreply de GitHub ; l'etiquette de l'app (PREFIXE suivi de X.Y.Z, par exemple maillage-v1.0.0) n'existe pas ;
     le flux existant s'analyse (ElementTree), la version n'y est pas, elle est superieure a sa tete (en nombres),
     et le numero de compilation aussi ; l'app lit le flux a l'adresse brute du depot
     (https://raw.githubusercontent.com/<depot>/main/<CHEMIN>) ; hors repetition, l'etat de GitHub (voir 7) ;
     NOTES-VERSIONS.md a sa section ; SPARKLE_BIN est donne (hors repetition) ; la cle publique de l'app est celle
     du trousseau ; le controle d'anonymisation est present (hors repetition) ; la licence de Sparkle est la ;
     l'identite de signature est seule a ce nom dans le trousseau, et son certificat n'a pour sujet que CN=<nom>
     (ni adresse, ni organisation, ni autre nom) ; puis les tests passent ;
  2. les numeros : la version, et le numero de compilation, le nombre de commits de main ;
  3. la compilation Release, ad hoc (une equipe de Local.xcconfig n'y entre pas), sans symboles de debogage dans
     les binaires (strip : la table OSO nomme les fichiers objets sous DerivedData ; le dSYM reste a part, jamais
     publie) et avec les chemins des sources ramenes a des noms neutres (-file-prefix-map) ; puis signee par
     l'empreinte de l'identite (un certificat auto-signe stable, plus tard un Developer ID) : le code imbrique
     d'abord, le runtime renforce, les droits gardes ; les droits de l'app signee sont relus (le bac a sable, les
     seuls services de Sparkle en mach-lookup, jamais get-task-allow) ; le certificat feuille de la signature est
     relu ; l'exigence de signature (codesign -d -r-) est ecrite dans exigence.txt ;
  4. le .dmg (hdiutil) : l'app, un raccourci vers Applications et la licence de Sparkle. Avant hdiutil, tout le
     contenu est refuse s'il porte un chemin personnel ($HOME, /Users/, le nom du compte) ou si le controle
     d'anonymisation y trouve une donnee reelle ; la liste des noms du contenu (chemins relatifs, cibles des liens),
     ecrite dans contenu-dmg.txt, passe aussi par ce controle. Avec NOTARISER=1 seulement (desactive par defaut), le .dmg signe,
     soumis a Apple (notarytool, profil PROFIL_NOTARISATION du trousseau), agrafe (stapler) et evalue (spctl) ;
  5. la signature Ed25519 du .dmg (sign_update de Sparkle, cle du trousseau), puis le flux : le fichier CHEMIN du
     depot (appcast.xml), qui garde toutes les versions publiees, la nouvelle en tete (relu apres l'ajout) ;
     l'adresse de chaque .dmg est celle de sa version publiee ;
  6. le controle d'anonymisation (prive) sur les notes, le flux, le message du commit du flux et les textes de l'app ;
  7. juste avant les gestes publics, l'etat est relu : HEAD est toujours le commit verifie, a jour avec GitHub, et
     l'arbre est propre ; l'etiquette n'existe pas sur GitHub ; gh release view repond « release not found » (toute
     autre reponse est un refus) ; gh a une session ; git push --dry-run origin HEAD:main passe. Puis la version
     publiee (gh release create --target <commit verifie>, qui cree l'etiquette sur GitHub), avec le .dmg ; puis le
     flux, commite (git add de ce seul fichier), et pousse aussitot par HEAD:main, ce qui est verifie (git ls-remote).
     Chaque geste est note dans gestes.txt, dans le dossier des produits : « tentative : ... » AVANT le geste, puis,
     APRES, sa ligne de reussite (« version publiee », « flux commite », « flux pousse sur main ») ou « echec : ... »
     avec la reprise. Une « tentative » sans suite (coupure, Ctrl-C) est un geste ambigu : lire GitHub d'abord ;
  8. le .dmg copie sur le Bureau (sauf --sans-bureau).

Rien n'est publie si une etape de 1 a 6, ou la relecture de l'etape 7, echoue. Un echec au milieu de l'etape 7
laisse les gestes deja faits, notes dans gestes.txt : la reprise part de ce fichier, qui dit ou reprendre, et du
dossier des produits (notes.md, appcast.xml, message-commit.txt, le .dmg). Un echec de gh release create est ambigu
(GitHub a pu creer la version avant que l'erreur arrive) : il est note comme tel.

En repetition (--repetition DOSSIER), ni GitHub, ni etiquette, ni Bureau : la branche peut etre une autre que main,
mais l'origine ne doit pas etre sur GitHub ; --url-base tient lieu des deux adresses de GitHub (le flux :
<URL>/<depot>/main/<CHEMIN> ; un .dmg : <URL>/<depot>/releases/download/<etiquette>/<fichier>), comme les servirait
un serveur local ; le flux est commite dans la copie, sans etre pousse ; les produits vont dans DOSSIER ; le
controle d'anonymisation absent est saute, avec un message. Avec --cle-privee et --cle-publique, une paire d'essai,
sans le trousseau : l'app porte cette cle publique, et le .dmg est signe avec la cle privee du fichier. Sans elles,
la cle du trousseau, comme pour la vraie publication. Avec --trousseau, l'identite de signature est cherchee dans ce
trousseau a part (un certificat d'essai), jamais dans celui de la session.

Les commandes externes se remplacent par l'environnement, pour les tests : XCODEGEN, XCODEBUILD, HDIUTIL, DITTO,
CODESIGN, SECURITY, OPENSSL, XCRUN, SPCTL, SPARKLE_BIN (dossier de sign_update et generate_keys), GH, GIT,
CONTROLE_ANONYMISATION et TABLE_ANONYMISATION ; le dossier personnel et le nom du compte cherches dans le .dmg, par
HOME, USER et LOGNAME. Aucun identifiant Apple, Team ID, empreinte ni mot de passe n'est ecrit ici : l'empreinte de
l'identite est lue dans le trousseau au moment de publier, et la notarisation lit le profil que notarytool
store-credentials a range dans le trousseau.
"""
import argparse
import datetime
import html
import os
import plistlib
import pwd
import re
import shlex
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from types import SimpleNamespace

VERSION = re.compile(r'^\d+\.\d+\.\d+$')
ESPACE_SPARKLE = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
PRIVE = os.path.expanduser('~/Dev/maillage-thread/.superpowers/anonymisation')
NOREPLY = re.compile(r'^[^@\s<>]+@users\.noreply\.github\.com$')
BAC_A_SABLE = 'com.apple.security.app-sandbox'
MACH_LOOKUP = 'com.apple.security.temporary-exception.mach-lookup.global-name'
GET_TASK_ALLOW = 'com.apple.security.get-task-allow'


class Refus(Exception):
    """Une verification qui arrete la publication. Jusqu'a la relecture de l'etape 7 comprise, rien n'est publie ;
    au-dela, les gestes deja faits sont dans gestes.txt."""


def lire(chemin):
    with open(chemin, encoding='utf-8') as f:
        return f.read()


def ecrire(chemin, texte):
    with open(chemin, 'w', encoding='utf-8') as f:
        f.write(texte)


# --- les numeros ---------------------------------------------------------------------------------------------

def version_valide(v):
    return bool(VERSION.match(v))


def nombres(version):
    """X.Y.Z en nombres, pour comparer : 1.10.0 vient apres 1.2.3."""
    return tuple(int(x) for x in version.split('.'))


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


def versions_du_flux(texte):
    """Les versions d'un flux (appcast.xml), lu par ElementTree : (X.Y.Z, numero de compilation) de chaque <item>,
    la tete d'abord. Refus si le flux ne s'analyse pas, ou si un <item> n'a pas les deux."""
    try:
        racine = ET.fromstring(texte)
    except ET.ParseError as e:
        raise Refus('flux illisible (%s)' % e)
    canal = racine.find('channel')
    if racine.tag != 'rss' or canal is None:
        raise Refus('flux illisible : ni <rss>, ni <channel>')
    s = '{%s}' % ESPACE_SPARKLE
    versions = []
    for item in canal.findall('item'):
        court = (item.findtext(s + 'shortVersionString') or '').strip()
        numero = (item.findtext(s + 'version') or '').strip()
        if not version_valide(court) or not numero.isdigit():
            raise Refus('flux illisible : un <item> sans sparkle:shortVersionString X.Y.Z ni sparkle:version entier')
        versions.append((court, int(numero)))
    return versions


def verifier_flux(existant, version, numero=None):
    """La version peut entrer dans le flux existant : elle n'y est pas, elle est superieure a sa tete (en nombres),
    et son numero de compilation (que compare Sparkle) aussi. Leve Refus."""
    versions = versions_du_flux(existant)
    if version in [v for v, _ in versions]:
        raise Refus('la version %s est deja dans le flux' % version)
    if versions:
        tete, numero_tete = versions[0]
        if nombres(version) <= nombres(tete):
            raise Refus('la version %s n\'est pas superieure a la tete du flux (%s)' % (version, tete))
        if numero is not None and numero <= numero_tete:
            raise Refus('le numero de compilation %d n\'est pas superieur a celui de la tete du flux (%d) : Sparkle '
                        'ne proposerait pas la version' % (numero, numero_tete))


def ajouter_au_flux(existant, titre, version, item, numero=None):
    """Le flux (appcast.xml) avec une version de plus, en tete : il garde toutes les versions publiees. Sans flux
    existant (None), un flux neuf. Refus si la version n'y entre pas (verifier_flux), ou si le flux produit ne
    s'analyse pas avec elle en tete."""
    if existant is None:
        xml = '''<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="%s">
  <channel>
    <title>%s</title>
%s  </channel>
</rss>
''' % (ESPACE_SPARKLE, html.escape(titre), item)
    else:
        verifier_flux(existant, version, numero)
        i = existant.find('    <item>')
        if i < 0:
            i = existant.find('  </channel>')
        if i < 0:
            raise Refus('flux illisible : ni <item>, ni </channel>')
        xml = existant[:i] + item + existant[i:]
    versions = versions_du_flux(xml)
    if not versions or versions[0][0] != version:
        raise Refus('le flux produit n\'a pas la version %s en tete' % version)
    return xml


def message_flux(nom_app, version):
    """Le message du commit du flux, en francais sans accents."""
    return ('Publier %s %s dans le flux des mises a jour\n\n'
            'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n' % (nom_app, version))


# --- les chemins personnels ----------------------------------------------------------------------------------

def guillemets(texte):
    """Un mot d'une liste de reglages de Xcode (OTHER_SWIFT_FLAGS...), entre guillemets : les espaces y restent."""
    return '"%s"' % texte.replace('\\', '\\\\').replace('"', '\\"')


def carte_des_chemins(racine):
    """Le prefixe ramene a « . » dans les binaires : la racine du depot, ou sont les sources (#filePath), sous ses
    deux formes (/tmp et /private/tmp). Le dossier de produits n'y est pas : ses chemins ne vont que dans la table OSO,
    que le strip retire, et dans le dSYM, qui doit les garder pour retrouver les modules precompiles."""
    if not racine or not racine.strip('/'):
        return []
    return sorted({(racine.rstrip('/'), '.'), (os.path.realpath(racine), '.')}, key=lambda p: (-len(p[0]), p))


def reglages_compilation(numero, carte):
    """Les reglages de la compilation de publication, en ligne de commande (ils l'emportent sur project.yml) :
    ad hoc, le numero de compilation, sans symboles de debogage dans les binaires (DEPLOYMENT_POSTPROCESSING et
    STRIP_INSTALLED_PRODUCT : le strip retire la table OSO, qui nomme les fichiers objets ; le dSYM reste a part),
    et les chemins des sources (#filePath, debogage) ramenes a la carte (-file-prefix-map), apres ceux du projet ;
    le controle du contenu du .dmg (chemins_personnels) refuse ce qui resterait."""
    swift = ' '.join('-file-prefix-map %s' % guillemets('%s=%s' % p) for p in carte)
    c = ' '.join(guillemets('-ffile-prefix-map=%s=%s' % p) for p in carte)
    return ['CURRENT_PROJECT_VERSION=%d' % numero, 'CODE_SIGN_IDENTITY=-', 'DEVELOPMENT_TEAM=',
            'CODE_SIGN_STYLE=Manual', 'DEPLOYMENT_POSTPROCESSING=YES', 'STRIP_INSTALLED_PRODUCT=YES',
            'OTHER_SWIFT_FLAGS=$(inherited) ' + swift, 'OTHER_CFLAGS=$(inherited) ' + c]


def motifs_personnels(env=None):
    """Ce qu'aucun fichier publie ne doit porter : tout chemin sous /Users/, le dossier personnel (HOME) et le nom du
    compte macOS (USER, LOGNAME et celui du systeme). Chaque motif avec ce qu'il est, jamais sa valeur."""
    e = os.environ if env is None else env
    motifs = [(b'/Users/', 'un chemin sous /Users/')]
    maison = e.get('HOME', '').rstrip('/')
    if maison:
        motifs.append((maison.encode(), 'le dossier personnel (HOME)'))
    comptes = {e.get('USER', ''), e.get('LOGNAME', '')}
    try:
        comptes.add(pwd.getpwuid(os.getuid()).pw_name)
    except KeyError:
        pass
    motifs += [(c.encode(), 'le nom du compte') for c in sorted(comptes) if c]
    return motifs


def chemins_personnels(dossier, motifs):
    """Ce qui, dans le dossier (le contenu du .dmg), porte un motif personnel : le contenu de chaque fichier
    ordinaire (binaires compris), la cible de chaque lien et chaque nom. Rend [(chemin relatif, motif)] ; le chemin
    relatif est tu s'il porte lui-meme le motif."""
    trouves = []
    for racine, dossiers, fichiers in os.walk(dossier):
        for nom in sorted(dossiers + fichiers):
            p = os.path.join(racine, nom)
            rel = os.path.relpath(p, dossier)
            if os.path.islink(p):
                contenu = os.readlink(p).encode()
            elif os.path.isfile(p):
                with open(p, 'rb') as f:
                    contenu = f.read()
            else:
                contenu = b''
            for motif, quoi in motifs:
                if motif in rel.encode():
                    trouves.append(('(un nom de fichier)', quoi))
                    break
                if motif in contenu:
                    trouves.append((rel, quoi))
                    break
    return trouves


def liste_du_contenu(dossier):
    """Les noms du contenu du .dmg, un par ligne : chaque chemin relatif (dossiers, fichiers et liens) et, pour un
    lien, sa cible. Le controle d'anonymisation ne lit que le contenu des fichiers : il lit aussi cette liste."""
    lignes = []
    for racine, dossiers, fichiers in os.walk(dossier):
        dossiers.sort()
        for nom in sorted(dossiers + fichiers):
            p = os.path.join(racine, nom)
            lignes.append(os.path.relpath(p, dossier) + (' -> ' + os.readlink(p) if os.path.islink(p) else ''))
    return '\n'.join(lignes) + '\n'


def fichiers_ordinaires(dossier):
    """Les fichiers ordinaires du dossier, sans suivre les liens, dans l'ordre."""
    liste = []
    for racine, dossiers, fichiers in os.walk(dossier):
        dossiers.sort()
        for nom in sorted(fichiers):
            p = os.path.join(racine, nom)
            if os.path.isfile(p) and not os.path.islink(p):
                liste.append(p)
    return liste


# --- la publication ------------------------------------------------------------------------------------------

class Outils:
    """Les commandes externes, remplacables par l'environnement (tests)."""

    def __init__(self, env=None):
        e = os.environ if env is None else env
        self.sparkle_bin = e.get('SPARKLE_BIN', '')
        self.git = e.get('GIT', 'git')
        self.xcodegen = e.get('XCODEGEN', 'xcodegen')
        self.xcodebuild = e.get('XCODEBUILD', 'xcodebuild')
        self.hdiutil = e.get('HDIUTIL', 'hdiutil')
        self.ditto = e.get('DITTO', 'ditto')
        self.codesign = e.get('CODESIGN', 'codesign')
        self.security = e.get('SECURITY', 'security')
        self.openssl = e.get('OPENSSL', '/usr/bin/openssl')
        self.xcrun = e.get('XCRUN', 'xcrun')
        self.spctl = e.get('SPCTL', 'spctl')
        self.gh = e.get('GH', 'gh')
        self.sign_update = os.path.join(self.sparkle_bin, 'sign_update') if self.sparkle_bin else 'sign_update'
        self.generate_keys = os.path.join(self.sparkle_bin, 'generate_keys') if self.sparkle_bin else 'generate_keys'
        self.controle = e.get('CONTROLE_ANONYMISATION', os.path.join(PRIVE, 'outils', 'controles.py'))
        self.table = e.get('TABLE_ANONYMISATION', os.path.join(PRIVE, 'execution', 'table.json'))


def lancer(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def dire(texte):
    print(texte, flush=True)


def identite_git(o, variable):
    """Le nom et l'adresse que git mettra dans le commit du flux (git var GIT_AUTHOR_IDENT ou GIT_COMMITTER_IDENT :
    la configuration, mais aussi GIT_AUTHOR_*, GIT_COMMITTER_* et EMAIL)."""
    r = subprocess.run([o.git, 'var', variable], capture_output=True, text=True)
    m = re.match(r'^(.*) <([^<>]*)> \d+ [+-]\d{4}$', r.stdout.strip())
    if r.returncode != 0 or not m:
        raise Refus('%s illisible (git var) : regler user.name et user.email du depot' % variable)
    return m.group(1), m.group(2)


def empreinte_identite(sortie, nom):
    """L'empreinte SHA-1 de la seule identite de signature a ce nom exact, dans la sortie de security find-identity
    (qui peut la lister deux fois : toutes les identites, puis les valides). Leve Refus s'il n'y en a pas, ou plus
    d'une."""
    empreintes = set(re.findall(r'^\s*\d+\)\s+([0-9A-F]{40})\s+"%s"' % re.escape(nom), sortie, re.M))
    if not empreintes:
        raise Refus('identite de signature introuvable dans le trousseau : %s' % nom)
    if len(empreintes) > 1:
        raise Refus('%d identites de signature portent le nom %s : une seule attendue' % (len(empreintes), nom))
    return empreintes.pop()


def certificat_du_trousseau(sortie, empreinte):
    """Le certificat (PEM) d'empreinte SHA-1 donnee, dans la sortie de security find-certificate -a -Z -p."""
    for sha1, pem in re.findall(r'SHA-1 hash: ([0-9A-F]{40})\s*\n(-----BEGIN CERTIFICATE-----.*?'
                                r'-----END CERTIFICATE-----)', sortie, re.S):
        if sha1 == empreinte:
            return pem + '\n'
    raise Refus("le certificat de l'identite de signature est introuvable dans le trousseau")


def verifier_certificat(o, nom, empreinte, pem=None, der=None, quoi='le certificat de signature'):
    """Le certificat est public (il est dans chaque signature) : son sujet doit etre CN=<nom> et rien d'autre (ni
    adresse, ni organisation), sans autre nom (subjectAltName) ni « @ » ; son empreinte, celle de l'identite.
    Le certificat est donne en PEM (pem) ou dans un fichier DER (der). Leve Refus, sans recopier le sujet."""
    base = [o.openssl, 'x509'] + (['-inform', 'DER', '-in', der] if der else []) + ['-noout']
    tete = lancer(base + ['-subject', '-nameopt', 'RFC2253', '-fingerprint', '-sha1'], input=pem)
    texte = lancer(base + ['-text'], input=pem)
    sujet = re.search(r'^subject=\s*(.*?)\s*$', tete, re.M)
    lue = re.search(r'Fingerprint=([0-9A-Fa-f:]+)', tete)
    if not sujet or sujet.group(1) != 'CN=' + nom:
        raise Refus('%s : son sujet doit etre seulement CN=%s (ni adresse, ni organisation)' % (quoi, nom))
    if '@' in tete + texte or 'Subject Alternative Name' in texte:
        raise Refus('%s porte une adresse ou un autre nom (subjectAltName) : rien n\'est publie' % quoi)
    if not lue or lue.group(1).replace(':', '').upper() != empreinte:
        raise Refus("%s n'est pas celui de l'identite choisie (empreinte)" % quoi)


def verifier_github(a, o, etiquette, sha):
    """L'etat de la copie et de GitHub, lu avant les tests puis de nouveau juste avant les gestes publics : HEAD est
    toujours le commit verifie, a jour avec GitHub (apres fetch), et l'arbre est propre ; l'etiquette n'existe pas
    sur GitHub ; gh release view repond « release not found » (publiee, ou toute autre erreur : refus) ; gh a une
    session ; git push --dry-run origin HEAD:main passe. Leve Refus."""
    lancer([o.git, 'fetch', '-q', '--no-tags', 'origin', 'main'])
    if lancer([o.git, 'rev-parse', 'HEAD']).strip() != sha:
        raise Refus('HEAD a change depuis les verifications : rien n\'est publie')
    if lancer([o.git, 'rev-parse', 'origin/main']).strip() != sha:
        raise Refus("main n'est pas a jour avec GitHub (origin/main)")
    if lancer([o.git, 'status', '--porcelain']).strip():
        raise Refus("l'arbre n'est pas propre (git status)")
    if lancer([o.git, 'ls-remote', '--tags', 'origin', 'refs/tags/' + etiquette]).strip():
        raise Refus("l'etiquette %s existe deja sur GitHub" % etiquette)
    vue = subprocess.run([o.gh, 'release', 'view', etiquette, '-R', a.depot_github], capture_output=True, text=True)
    if vue.returncode == 0:
        raise Refus('la version %s est deja publiee sur GitHub' % etiquette)
    if 'release not found' not in vue.stderr + vue.stdout:
        raise Refus('gh release view %s : ni publiee, ni « release not found » (code %d) : etat de GitHub inconnu'
                    % (etiquette, vue.returncode))
    if subprocess.run([o.gh, 'auth', 'status', '--hostname', 'github.com'], capture_output=True).returncode != 0:
        raise Refus("gh n'a pas de session sur github.com (gh auth status)")
    if subprocess.run([o.git, 'push', '--dry-run', '-q', 'origin', 'HEAD:main'], capture_output=True).returncode != 0:
        raise Refus('git push --dry-run origin HEAD:main en echec : le flux ne pourrait pas etre pousse')


def verifier(a, o, racine_git, version):
    """L'etape 1 : tout ce qui doit tenir avant les tests et la compilation. Rend l'etat verifie (numero, commit,
    empreinte de l'identite, controle present). Leve Refus."""
    if not version_valide(version):
        raise Refus('version attendue sous la forme X.Y.Z : ' + version)
    marketing = reglage('project.yml', a.cible, 'MARKETING_VERSION')
    if marketing != version:
        raise Refus('MARKETING_VERSION de project.yml : %s, pas %s' % (marketing, version))
    if lancer([o.git, 'status', '--porcelain']).strip():
        raise Refus("l'arbre n'est pas propre (git status)")
    for variable, role in (('GIT_AUTHOR_IDENT', "l'auteur"), ('GIT_COMMITTER_IDENT', 'le committer')):
        nom, adresse = identite_git(o, variable)
        if nom != a.auteur or not NOREPLY.match(adresse):
            raise Refus("le commit du flux est public : %s (git var %s) doit etre %s, a l'adresse noreply de GitHub"
                        % (role, variable, a.auteur))
    etiquette = a.etiquette + version
    if lancer([o.git, 'tag', '-l', etiquette]).strip():
        raise Refus("l'etiquette %s existe deja" % etiquette)
    sha = lancer([o.git, 'rev-parse', 'HEAD']).strip()
    numero = numero_compilation(racine_git, o.git)
    if os.path.exists(a.flux):
        verifier_flux(lire(a.flux), version, numero)
    adresse = 'https://raw.githubusercontent.com/%s/main/%s' % (a.depot_github, chemin_depot(a.flux, racine_git))
    if reglage('project.yml', a.cible, 'FLUX_MISES_A_JOUR') != adresse:
        raise Refus('FLUX_MISES_A_JOUR de project.yml : %s attendu (le flux du depot)' % adresse)
    if a.repetition:
        origine = subprocess.run([o.git, 'remote', 'get-url', 'origin'], capture_output=True, text=True).stdout
        if 'github.com' in origine:
            raise Refus("repetition dans une copie dont l'origine est sur GitHub : son flux d'essai pourrait y etre "
                        'pousse ; cloner le depot dans un dossier a part')
    else:
        branche = lancer([o.git, 'rev-parse', '--abbrev-ref', 'HEAD']).strip()
        if branche != 'main':
            raise Refus('la publication se fait depuis main, pas ' + branche)
        verifier_github(a, o, etiquette, sha)
    notes('NOTES-VERSIONS.md', version)
    if not a.repetition:
        if not o.sparkle_bin:
            raise Refus("SPARKLE_BIN (le dossier bin de l'archive de Sparkle) est obligatoire pour publier")
        for outil in (o.sign_update, o.generate_keys):
            if not os.access(outil, os.X_OK):
                raise Refus('outil de Sparkle introuvable dans SPARKLE_BIN : %s' % os.path.basename(outil))
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
    controle = os.path.exists(o.controle) and os.path.exists(o.table)
    if not controle and not a.repetition:
        raise Refus("controle d'anonymisation absent (CONTROLE_ANONYMISATION, TABLE_ANONYMISATION) : rien n'est "
                    'publie')
    if not os.path.isfile(a.licence) or not os.path.getsize(a.licence):
        raise Refus('licence de Sparkle absente : %s' % a.licence)
    trousseau = [a.trousseau] if a.trousseau else []
    empreinte = empreinte_identite(lancer([o.security, 'find-identity', '-p', 'codesigning'] + trousseau),
                                   a.identite)
    pem = certificat_du_trousseau(lancer([o.security, 'find-certificate', '-a', '-c', a.identite, '-Z', '-p']
                                         + trousseau), empreinte)
    verifier_certificat(o, a.identite, empreinte, pem=pem)
    if notariser() and not os.environ.get('PROFIL_NOTARISATION'):
        raise Refus('NOTARISER=1 demande PROFIL_NOTARISATION, le profil de notarytool store-credentials')
    return SimpleNamespace(numero=numero, sha=sha, empreinte=empreinte, controle=controle)


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


def signer(a, o, empreinte, chemin, droits=True):
    """Signe un code avec l'identite de la publication, par son empreinte (un nom seul prendrait aussi une identite
    dont le nom le contient) : runtime renforce, droits gardes, horodatage si notarise."""
    cmd = [o.codesign, '--force', '--sign', empreinte]
    if droits:
        cmd += ['--options', 'runtime', '--preserve-metadata=entitlements']
    cmd.append('--timestamp' if notariser() else '--timestamp=none')
    if a.trousseau:
        cmd += ['--keychain', a.trousseau]
    lancer(cmd + [chemin])


def verifier_droits(o, app, identifiant):
    """Les droits de l'app signee, relus (codesign -d --entitlements - --xml) : le bac a sable, en mach-lookup les
    seuls services de Sparkle (<identifiant>-spks et <identifiant>-spki, ni plus ni moins), et jamais get-task-allow
    (un debogueur pourrait s'attacher a l'app). Leve Refus."""
    sortie = subprocess.run([o.codesign, '-d', '--entitlements', '-', '--xml', app], check=True,
                            capture_output=True).stdout
    try:
        droits = plistlib.loads(sortie)
    except Exception:
        droits = None
    if not isinstance(droits, dict):
        raise Refus("droits de l'app signee illisibles (codesign -d --entitlements)")
    if droits.get(BAC_A_SABLE) is not True:
        raise Refus("droits de l'app signee : le bac a sable (%s) manque" % BAC_A_SABLE)
    services = droits.get(MACH_LOOKUP)
    attendus = [identifiant + '-spks', identifiant + '-spki']
    if not isinstance(services, list) or sorted(services) != sorted(attendus):
        raise Refus("droits de l'app signee : %s doit etre exactement %s" % (MACH_LOOKUP, ' et '.join(attendus)))
    if GET_TASK_ALLOW in droits:
        raise Refus("droits de l'app signee : %s est present" % GET_TASK_ALLOW)


def controler(o, fichiers):
    """Le controle d'anonymisation (prive) sur des fichiers, binaires compris : il n'imprime que des comptes."""
    r = subprocess.run(['/usr/bin/python3', o.controle, 'fichiers', '--table', o.table] + fichiers,
                       capture_output=True, text=True)
    dire("controle d'anonymisation (%d fichiers) : %s" % (len(fichiers), ' ; '.join(r.stdout.strip().splitlines())))
    return r.returncode == 0


def noter_geste(sortie, texte):
    """Une ligne horodatee de gestes.txt, dans le dossier des produits : la reprise part de la."""
    with open(os.path.join(sortie, 'gestes.txt'), 'a', encoding='utf-8') as f:
        f.write('%s %s\n' % (datetime.datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ'), texte))


def geste(sortie, quoi, reprise, action):
    """Un geste de l'etape 7, encadre dans gestes.txt : « tentative : QUOI » est note AVANT, jamais apres. Si l'action
    reussit, elle rend la ligne de sa reussite, notee ensuite ; si elle echoue (ou est interrompue), « echec : QUOI ;
    REPRISE » est note, et l'erreur remonte. Une « tentative » sans suite : le geste est ambigu."""
    noter_geste(sortie, 'tentative : ' + quoi)
    try:
        fait = action()
    except BaseException:
        noter_geste(sortie, 'echec : %s ; %s' % (quoi, reprise))
        raise
    noter_geste(sortie, fait)


def pousser_flux(o):
    """Pousse HEAD (le commit du flux) sur main de GitHub, par HEAD:main (jamais par la branche : si HEAD n'etait pas
    sur main, un push de main ne pousserait rien), puis verifie que main de GitHub est a ce commit (ls-remote)."""
    lancer([o.git, 'push', 'origin', 'HEAD:main'])
    tete = lancer([o.git, 'rev-parse', 'HEAD']).strip()
    distant = lancer([o.git, 'ls-remote', 'origin', 'refs/heads/main']).split()
    if distant[:1] != [tete]:
        raise Refus("apres le push, main de GitHub n'est pas au commit du flux (%s)" % tete)


def publier(a, o=None, maintenant=None):
    o = o or Outils()
    version = a.version
    racine_git = lancer([o.git, 'rev-parse', '--show-toplevel']).strip()
    etat = verifier(a, o, racine_git, version)
    if not etat.controle:
        dire("controle d'anonymisation absent de ce Mac : saute (repetition seulement)")
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
    numero = etat.numero
    systeme = systeme_minimum('project.yml')
    dire('version %s, numero de compilation %d, macOS %s minimum' % (version, numero, systeme))

    # 3. la compilation Release, ad hoc, sans symboles ni chemins personnels
    etiquette = a.etiquette + version
    chemin_flux_depot = chemin_depot(a.flux, racine_git)
    if a.repetition:
        flux = '%s/%s/main/%s' % (a.url_base, a.depot_github, chemin_flux_depot)
        url_dmg = '%s/%s/releases/download/%s' % (a.url_base, a.depot_github, etiquette)
    else:
        flux = None
        url_dmg = 'https://github.com/%s/releases/download/%s' % (a.depot_github, etiquette)
    reglages = reglages_compilation(numero, carte_des_chemins(racine_git))
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
        signer(a, o, etat.empreinte, chemin)
    lancer([o.codesign, '--verify', '--deep', '--strict', app])
    if not info.get('CFBundleIdentifier'):
        raise Refus("Info.plist de l'app compilee : CFBundleIdentifier manque")
    verifier_droits(o, app, info['CFBundleIdentifier'])
    # Le certificat feuille de la signature, tel qu'il sera publie.
    prefixe = os.path.join(sortie, 'certificat-')
    for n in os.listdir(sortie):
        if n.startswith('certificat-'):
            os.remove(os.path.join(sortie, n))
    lancer([o.codesign, '-d', '--extract-certificates=' + prefixe, app])
    if not os.path.isfile(prefixe + '0'):
        raise Refus("aucun certificat dans la signature de l'app")
    verifier_certificat(o, a.identite, etat.empreinte, der=prefixe + '0', quoi='le certificat feuille de l\'app')
    exigence = subprocess.run([o.codesign, '-d', '-r-', app], check=True, capture_output=True,
                              text=True).stdout.strip()
    ecrire(os.path.join(sortie, 'exigence.txt'), exigence + '\n')
    dire('exigence de signature : ' + exigence)

    # 4. le .dmg : l'app, le raccourci vers Applications et la licence de Sparkle, controles avant hdiutil
    nom_dmg = '%s-%s.dmg' % (a.fichier, version)
    dmg = os.path.join(sortie, nom_dmg)
    scene = os.path.join(sortie, 'dmg')
    shutil.rmtree(scene, ignore_errors=True)
    os.makedirs(scene)
    lancer([o.ditto, app, os.path.join(scene, a.nom_app + '.app')])
    os.symlink('/Applications', os.path.join(scene, 'Applications'))
    shutil.copyfile(a.licence, os.path.join(scene, os.path.basename(a.licence)))
    trouves = chemins_personnels(scene, motifs_personnels())
    if trouves:
        raise Refus('le contenu du .dmg porte un chemin personnel (%d) : %s ; rien n\'est publie'
                    % (len(trouves), ' ; '.join('%s (%s)' % t for t in trouves[:5])))
    dire('contenu du .dmg : aucun chemin personnel')
    liste = os.path.join(sortie, 'contenu-dmg.txt')
    ecrire(liste, liste_du_contenu(scene))
    if etat.controle and not controler(o, fichiers_ordinaires(scene) + [liste]):
        raise Refus("le controle d'anonymisation a trouve des donnees reelles dans le contenu du .dmg : rien n'est "
                    'publie')
    if os.path.exists(dmg):
        os.remove(dmg)
    lancer([o.hdiutil, 'create', '-quiet', '-volname', '%s %s' % (a.nom_app, version), '-srcfolder', scene,
            '-fs', 'HFS+', '-format', 'UDZO', dmg])
    shutil.rmtree(scene)
    if notariser():
        # Avant la signature Ed25519 : l'agrafe change le .dmg.
        signer(a, o, etat.empreinte, dmg, droits=False)
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
    xml = ajouter_au_flux(lire(a.flux) if os.path.exists(a.flux) else None, a.nom_app, version, item, numero)
    # Le nouveau flux, d'abord a cote : il n'entre dans le depot qu'apres le controle.
    chemin_flux = os.path.join(sortie, 'appcast.xml')
    ecrire(chemin_flux, xml)
    chemin_notes = os.path.join(sortie, 'notes.md')
    ecrire(chemin_notes, texte_notes)
    chemin_message = os.path.join(sortie, 'message-commit.txt')
    ecrire(chemin_message, message_flux(a.nom_app, version))
    dire('signe : %s (%d octets) ; flux : %s' % (dmg, os.path.getsize(dmg), chemin_flux))

    # 6. le controle d'anonymisation (prive) sur les textes publies
    if etat.controle:
        if not controler(o, ['NOTES-VERSIONS.md', chemin_flux, chemin_notes, chemin_message] + a.textes):
            raise Refus("le controle d'anonymisation a trouve des donnees reelles : rien n'est publie")

    # 7. la publication : l'etat relu, puis la version publiee, avec le .dmg (l'etiquette creee par GitHub sur le
    # commit verifie) ; puis le flux, commite et pousse. Chaque geste est note AVANT (tentative), puis APRES.
    if not a.repetition:
        verifier_github(a, o, etiquette, etat.sha)

        def creer_version():
            lancer([o.gh, 'release', 'create', etiquette, dmg, '-R', a.depot_github, '--target', etat.sha,
                    '--title', '%s %s' % (a.nom_app, version), '--notes-file', chemin_notes])
            return 'version publiee : %s, etiquette creee sur %s, avec %s' % (etiquette, etat.sha, nom_dmg)

        geste(sortie, 'gh release create %s sur %s, avec %s' % (etiquette, etat.sha, nom_dmg),
              "GitHub a peut-etre cree la version malgre l'erreur : lire gh release view %s et git ls-remote --tags "
              "origin ; si rien n'y est, relancer publier.sh ; sinon reprendre au commit du flux (appcast.xml et "
              "message-commit.txt de ce dossier)" % etiquette, creer_version)
        dire('publie : https://github.com/%s/releases/tag/%s' % (a.depot_github, etiquette))

    def commiter_flux():
        shutil.copyfile(chemin_flux, a.flux)
        lancer([o.git, 'add', a.flux])
        lancer([o.git, 'commit', '-q', '-F', chemin_message])
        return 'flux commite : %s (%s)' % (chemin_flux_depot, lancer([o.git, 'rev-parse', 'HEAD']).strip())

    geste(sortie, 'commit du flux %s' % chemin_flux_depot,
          "repetition : rien n'est public, relancer" if a.repetition else
          "la version est publiee, le flux ne l'est pas : reprendre ici (git status ; copier appcast.xml de ce "
          "dossier dans le depot, git add, git commit -F message-commit.txt, puis git push origin HEAD:main)",
          commiter_flux)
    if a.repetition:
        dire('repetition : flux commite dans la copie (%s), ni etiquette, ni GitHub, ni Bureau ; produits dans %s'
             % (chemin_flux_depot, sortie))
        return sortie

    def pousser():
        pousser_flux(o)
        return 'flux pousse sur main'

    geste(sortie, 'push du flux (git push origin HEAD:main)',
          "la version est publiee et le flux est commite en local : reprendre au push (lire d'abord git ls-remote "
          "origin refs/heads/main, puis git push origin HEAD:main)", pousser)
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
    q.add_argument('--auteur', required=True, help='nom attendu de l\'auteur du commit du flux (git var)')
    q.add_argument('--etiquette', required=True, help="debut de l'etiquette de l'app, suivi de X.Y.Z (maillage-v)")
    q.add_argument('--flux', required=True, help='le flux du depot (appcast.xml), depuis le dossier de publier.sh')
    q.add_argument('--licence', required=True, help='la licence de Sparkle, copiee dans le .dmg')
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
    except OSError as e:
        print('echec : %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
