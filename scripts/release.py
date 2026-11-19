#!/usr/bin/env python3
"""Publishes what build-nx.sh made for both consoles as a release on GitHub.

    python scripts/release.py              # makes gx/dist-release, then publishes v<version>
    python scripts/release.py --dry-run    # makes gx/dist-release only
    python scripts/release.py --local DIR  # lays a release out in DIR as GitHub does, to
                                           # serve to a launcher built with UPDATE_RELEASES

One release has both consoles' programs, one image and manifest.txt (what the
launcher reads: gx/launcher/update.c):

    chromium-nx.nro          the Switch 1's (build-nx.sh, gx/dist-nx)
    chromium-nx2.nro         the Switch 2's (SWITCH2=1 build-nx.sh, gx/dist-nx2)
    chromium-full.ext4       Chromium's image, the same for both

Each keeps its name on the card (UPDATE_PROGRAM, gx/launcher/update.h); the
Switch 1's is the one the launchers before 0.2.0 look for. --zip adds a zip
of the SD card's folder for each console. The version is gx/version.h's. Publishing needs gh, logged in to
GitHub.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = 'shodowlo/chromium-nx'
OUT = os.path.join(ROOT, 'gx', 'dist-release')
APP1 = os.path.join(ROOT, 'gx', 'dist-nx', 'switch', 'chromium-nx')
APP2 = os.path.join(ROOT, 'gx', 'dist-nx2', 'switch', 'chromium-nx')
IMAGE = 'chromium-full.ext4'
# The release's files: its name there, and where it is made.
FILES = [
    ('chromium-nx.nro', os.path.join(APP1, 'chromium-nx.nro')),
    ('chromium-nx2.nro', os.path.join(APP2, 'chromium-nx2.nro')),
    (IMAGE, os.path.join(APP1, IMAGE)),
]


def version():
    if os.environ.get('GX_VERSION'):   # as build-nx.sh takes it, for a test
        return os.environ['GX_VERSION']
    text = open(os.path.join(ROOT, 'gx', 'version.h')).read()
    return re.search(r'#define GX_VERSION "([^"]+)"', text).group(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--local')
    p.add_argument('--notes', default='', help='what is new, for the release page')
    p.add_argument('--notes-file', help='the same, from a file (Markdown)')
    p.add_argument('--zip', action='store_true')
    a = p.parse_args()
    if a.notes_file:   # read now: it may be in OUT, which is made again below
        a.notes = open(a.notes_file, encoding='utf-8').read()

    v = version()
    tag = 'v' + v
    for name, src in FILES:
        if not os.path.exists(src):
            sys.exit('%s is missing: run build-nx.sh and SWITCH2=1 build-nx.sh first' % src)

    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    lines = ['version=' + v, 'tag=' + tag]
    hashes = {}
    for name, src in FILES:
        print('hashing', name)
        hashes[name] = sha256(src)
        lines.append('file=%s %d %s' % (name, os.path.getsize(src), hashes[name]))
    # Both consoles are given the one image: the Switch 2's must be it too.
    other = os.path.join(APP2, IMAGE)
    if os.path.exists(other) and sha256(other) != hashes[IMAGE]:
        sys.exit('%s is not the image of %s: build both consoles again' % (other, APP1))
    manifest = os.path.join(OUT, 'manifest.txt')
    open(manifest, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(open(manifest).read())

    if a.local:
        # As GitHub has it: releases/latest/download/<name> is the newest
        # release's, releases/download/<tag>/<name> any release's.
        latest = os.path.join(a.local, 'latest', 'download')
        tagged = os.path.join(a.local, 'download', tag)
        os.makedirs(latest, exist_ok=True)
        os.makedirs(tagged, exist_ok=True)
        shutil.copy(manifest, latest)
        shutil.copy(manifest, tagged)
        for name, src in FILES:
            shutil.copy(src, os.path.join(tagged, name))
        print('laid out in', a.local)
        return

    # Copies under their names in the release (gh names an asset after its
    # file).
    assets = [manifest]
    for name, src in FILES:
        dst = os.path.join(OUT, name)
        shutil.copy(src, dst)
        assets.append(dst)
    if a.zip:
        for console, program in (('switch1', 'chromium-nx.nro'), ('switch2', 'chromium-nx2.nro')):
            z = os.path.join(OUT, 'chromium-nx-%s-%s.zip' % (v, console))
            print('zipping', z)
            with zipfile.ZipFile(z, 'w', zipfile.ZIP_STORED) as f:
                f.write(os.path.join(OUT, program), 'switch/chromium-nx/' + program)
                f.write(os.path.join(OUT, IMAGE), 'switch/chromium-nx/' + IMAGE)
            assets.append(z)
    if a.dry_run:
        print('dry run: nothing published (it would go to %s)' % REPO)
        return

    if subprocess.run(['gh', 'release', 'view', tag, '-R', REPO], capture_output=True).returncode == 0:
        sys.exit('%s exists already on %s: change GX_VERSION in gx/version.h' % (tag, REPO))
    notes = a.notes or (
        'Copy chromium-nx.nro (Switch 1) or chromium-nx2.nro (Switch 2) to '
        '/switch/chromium-nx/ on the SD card: the launcher downloads '
        'Chromium\'s image (chromium-full.ext4) the first time.\n\n'
        'Already installed: the launcher offers the update itself.')
    # Through a file: a long text of several lines on the command line is
    # lost on the way to gh on Windows.
    notes_path = os.path.join(OUT, 'notes.md')
    open(notes_path, 'w', encoding='utf-8', newline='\n').write(notes)
    subprocess.run(['gh', 'release', 'create', tag, '-R', REPO, '--title', 'Chromium NX ' + v,
                    '--notes-file', notes_path] + assets, check=True)


if __name__ == '__main__':
    main()
