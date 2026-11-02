#!/usr/bin/env python3
"""Publishes what build-nx.sh made (gx/dist-nx) as a release on GitHub.

    python scripts/release.py              # makes gx/dist-release, then publishes v<version>
    python scripts/release.py --dry-run    # makes gx/dist-release only
    python scripts/release.py --local DIR  # lays a release out in DIR as GitHub does, to
                                           # serve to a launcher built with UPDATE_RELEASES

The version is gx/version.h's. The release has the program, Chromium's image
and manifest.txt (what the launcher reads: gx/launcher/update.c); --zip adds a
zip of the folder for the SD card. Publishing needs gh,
logged in to GitHub.
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
APP = os.path.join(ROOT, 'gx', 'dist-nx', 'switch', 'chromium-nx')
OUT = os.path.join(ROOT, 'gx', 'dist-release')
FILES = ['chromium-nx.nro', 'chromium-full.ext4']


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
    p.add_argument('--zip', action='store_true')
    a = p.parse_args()

    v = version()
    tag = 'v' + v
    for name in FILES:
        if not os.path.exists(os.path.join(APP, name)):
            sys.exit('%s is missing: run build-nx.sh first' % os.path.join(APP, name))

    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    lines = ['version=' + v, 'tag=' + tag]
    for name in FILES:
        src = os.path.join(APP, name)
        print('hashing', name)
        lines.append('file=%s %d %s' % (name, os.path.getsize(src), sha256(src)))
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
        for name in FILES:
            shutil.copy(os.path.join(APP, name), tagged)
        print('laid out in', a.local)
        return

    assets = [manifest] + [os.path.join(APP, name) for name in FILES]
    if a.zip:
        z = os.path.join(OUT, 'chromium-nx-%s.zip' % v)
        print('zipping', z)
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_STORED) as f:
            for name in FILES:
                f.write(os.path.join(APP, name), 'switch/chromium-nx/' + name)
        assets.append(z)
    if a.dry_run:
        print('dry run: nothing published')
        return

    if subprocess.run(['gh', 'release', 'view', tag], capture_output=True).returncode == 0:
        sys.exit('%s exists already: change GX_VERSION in gx/version.h' % tag)
    notes = a.notes or (
        'Copy chromium-nx.nro to /switch/chromium-nx/ on the SD card: the launcher downloads '
        'Chromium\'s image (chromium-full.ext4) the first time.\n\n'
        'Already installed: the launcher offers the update itself.')
    subprocess.run(['gh', 'release', 'create', tag, '--title', 'Chromium NX ' + v, '--notes', notes]
                   + assets, check=True)


if __name__ == '__main__':
    main()
