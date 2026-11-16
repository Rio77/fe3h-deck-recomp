#!/usr/bin/env python3
"""Assemble a Linux installer kit from an already audited AppImage.
SPDX-License-Identifier: MIT
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import tarfile

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('appimage', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    source = Path(__file__).resolve().parent
    repo = source.parents[1]
    app = args.appimage.resolve(strict=True)
    kit = args.output.resolve() / 'FE3H-Linux-opt5'
    kit.mkdir(parents=True, exist_ok=False)
    for name in ('manage.py', 'install_core.py', 'game_files.py', 'processes.py', 'Install.sh', 'Install.desktop'):
        shutil.copy2(source / name, kit / name)
    shutil.copy2(app, kit / app.name)
    (kit / app.name).chmod(0o755)
    (kit / 'Install.sh').chmod(0o755)
    (kit / 'Install.desktop').chmod(0o755)
    for name in ('LICENSE', 'THIRD_PARTY.md', 'UPSTREAM.md'):
        shutil.copy2(repo / name, kit / name)
    shutil.copy2(repo / 'docs/linux.md', kit / 'INSTRUCTIONS.md')
    shutil.copy2(repo / 'docs/validation.md', kit / 'VALIDATION.md')
    with app.open('rb') as stream:
        sha = hashlib.file_digest(stream, 'sha256').hexdigest()
    shutil.copy2(repo / 'docs/icon/icon.svg', kit / 'icon.svg')
    (kit / 'release.json').write_text(json.dumps({'appimage': app.name, 'sha256': sha, 'version': '0.2.3-linux-opt5'}, indent=2) + '\n')
    hashes = []
    for f in sorted(kit.iterdir()):
        if f.is_file():
            with f.open('rb') as stream:
                hashes.append(hashlib.file_digest(stream, 'sha256').hexdigest() + '  ' + f.name)
    (kit / 'SHA256SUMS').write_text('\n'.join(hashes) + '\n')
    target = args.output.resolve() / 'FE3H-Linux-opt5-installer.tar.gz'
    with tarfile.open(target, 'w:gz') as archive:
        archive.add(kit, arcname=kit.name)
    print(target)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
