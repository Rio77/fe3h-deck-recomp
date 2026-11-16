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
import subprocess
import tempfile

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('appimage', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--sources', required=True, type=Path,
                   help='Complete corresponding-source archive to include in the installer')
    args = p.parse_args()
    source = Path(__file__).resolve().parent
    repo = source.parents[1]
    app = args.appimage.resolve(strict=True)
    corresponding = args.sources.resolve(strict=True)
    # Sources accompany the binary in the same download; do not publish a kit
    # which accidentally drops the generator or shared-library source archives.
    with tarfile.open(corresponding) as archive:
        for required in ('project/LICENSE', 'project/tools/gen.py',
                         'dependencies/dependency-sources.json'):
            if not archive.getmember(required).isfile():
                raise ValueError('Incomplete corresponding-source archive: ' + required)
        components = json.load(archive.extractfile('dependencies/dependency-sources.json'))
        if {item['component'] for item in components} != {'FFmpeg', 'Unicorn'}:
            raise ValueError('Missing corresponding sources for bundled dependencies')
        for item in components:
            if Path(item['file']).name != item['file']:
                raise ValueError('Invalid corresponding-source filename')
            with archive.extractfile('dependencies/' + item['file']) as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != item['sha256']:
                    raise ValueError('Corresponding-source checksum mismatch')
    kit = args.output.resolve() / 'FE3H-Deck-Recomp-opt5'
    kit.mkdir(parents=True, exist_ok=False)
    for name in ('manage.py', 'install_core.py', 'game_files.py', 'processes.py', 'Install.sh', 'Install.desktop'):
        shutil.copy2(source / name, kit / name)
    shutil.copy2(app, kit / app.name)
    (kit / app.name).chmod(0o755)
    (kit / 'Install.sh').chmod(0o755)
    (kit / 'Install.desktop').chmod(0o755)
    for name in ('LICENSE', 'THIRD_PARTY.md', 'UPSTREAM.md', 'README.md', 'README.es.md'):
        shutil.copy2(repo / name, kit / name)
    (kit / 'docs').mkdir()
    for name in ('linux.md', 'validation.md'):
        shutil.copy2(repo / 'docs' / name, kit / 'docs' / name)
    with tempfile.TemporaryDirectory(prefix='fe3h-notices-') as temporary:
        subprocess.run([str(app), '--appimage-extract'], cwd=temporary,
                       stdout=subprocess.DEVNULL, check=True, timeout=120)
        shutil.copytree(Path(temporary) / 'squashfs-root/usr/share/licenses/fe3hrecomp', kit / 'licenses')
    shutil.copy2(repo / 'docs/linux.md', kit / 'INSTRUCTIONS.md')
    shutil.copy2(repo / 'docs/validation.md', kit / 'VALIDATION.md')
    with app.open('rb') as stream:
        sha = hashlib.file_digest(stream, 'sha256').hexdigest()
    shutil.copy2(repo / 'docs/icon/icon_1024.png', kit / 'icon.png')
    (kit / 'sources').mkdir()
    shutil.copy2(corresponding, kit / 'sources' / corresponding.name)
    (kit / 'release.json').write_text(json.dumps({'appimage': app.name, 'sha256': sha, 'version': '0.2.3-linux-opt5'}, indent=2) + '\n')
    hashes = []
    for f in sorted(kit.rglob('*')):
        if f.is_file():
            with f.open('rb') as stream:
                hashes.append(hashlib.file_digest(stream, 'sha256').hexdigest() + '  ' + f.relative_to(kit).as_posix())
    (kit / 'SHA256SUMS').write_text('\n'.join(hashes) + '\n')
    target = args.output.resolve() / 'FE3H-Deck-Recomp-0.2.3-linux-opt5-steamdeck.tar.gz'
    with tarfile.open(target, 'w:gz') as archive:
        archive.add(kit, arcname=kit.name)
    print(target)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
