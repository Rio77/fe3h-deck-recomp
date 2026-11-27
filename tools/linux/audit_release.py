#!/usr/bin/env python3
"""Audit actual exported files, independent of .gitignore."""
import argparse
import hashlib
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser()
group = parser.add_mutually_exclusive_group(required=True)
group.add_argument('--source', type=Path)
group.add_argument('--appdir', type=Path)
parser.add_argument('--manifest', type=Path)
args = parser.parse_args()
root = (args.source or args.appdir).resolve()
if not root.is_dir():raise SystemExit('Artifact folder does not exist')
forbidden_parts = {'save', 'saves', 'amiibo', 'extracted', 'romfs', 'exefs', 'cache', '.ssh', 'private', 'node_modules'}
forbidden_names = {'DATA0.bin', 'DATA1.bin', 'INFO0.bin', 'INFO2.bin', 'DlcVersion.bin', 'romfs.bin',
                   'warm.bin', 'warm_cs.bin', 'rclone.conf', 'deck_build_key', 'known_hosts', 'fe3hrecomp.ini'}
forbidden_suffixes = {'.nso', '.npdm', '.nca', '.nsp', '.nsz', '.xci', '.webm', '.mp4', '.bntx', '.bfres', '.bfttf', '.spv', '.rdc', '.log', '.rej'}
images = {'icon.svg', 'icon_1024.png', 'icon.png', 'fe3hrecomp.png'}
fonts = {'Baloo2-SemiBold.ttf', 'EBGaramond12-Regular.ttf'}
problems = []
manifest = []
if args.appdir:
    if not (root / 'AppRun').is_file() or not (root / 'AppRun').stat().st_mode & 0o111:
        problems.append('Missing executable AppRun')
    for required in ['usr/bin/fe3hrecomp', 'fe3hrecomp.desktop', 'fe3hrecomp.png', 'usr/share/licenses/fe3hrecomp/components.json']:
        if not (root / required).is_file():problems.append(f'Missing package component: {required}')
for p in sorted(root.rglob('*')):
    rel = p.relative_to(root)
    if '.git' in rel.parts:
        continue
    if args.source and any(part in {'build', 'generated', 'input'} for part in rel.parts):
        problems.append(f'Build/generated/input content in source export: {rel}')
        continue
    if p.is_symlink():
        if not p.resolve().is_relative_to(root):problems.append(f'Escaping symlink: {rel}')
        continue
    if not p.is_file():continue
    compiler_source = rel.as_posix() in {
        'usr/bin/zig/lib/std/Build/Cache/DepTokenizer.zig',
        'usr/bin/zig/lib/std/Build/Cache/Directory.zig',
        'usr/bin/zig/lib/std/Build/Cache/Path.zig'}
    if (any(part.lower() in forbidden_parts for part in rel.parts) and not compiler_source) or p.name in forbidden_names or p.suffix.lower() in forbidden_suffixes:
        problems.append(f'Private/game/generated artifact: {rel}')
    if p.name.startswith('fe3hgame.') or p.name.lower().startswith('nintendo_'):
        problems.append(f'Game library or proprietary font: {rel}')
    if p.suffix.lower() in {'.png', '.jpg', '.jpeg', '.webp', '.ico', '.icns', '.svg', '.bmp', '.gif', '.avif', '.tga', '.dds', '.ktx'} and p.name not in images:
        problems.append(f'Image without an approved provenance entry: {rel}')
    if p.suffix.lower() in {'.ttf', '.otf', '.woff', '.woff2'} and p.name not in fonts:
        problems.append(f'Font without an approved provenance entry: {rel}')
    raw = p.read_bytes()
    if raw.startswith(b'NSO0'):
        problems.append(f'Original game module: {rel}')
    if len(raw) < 2 * 1024 * 1024:
        try:
            text = raw.decode('utf-8')
            # Construct these patterns so this audit program does not flag itself.
            if re.search('-----BEGIN ' + r'(?:OPENSSH |RSA |EC )?PRIVATE KEY-----', text):
                problems.append(f'Private key: {rel}')
            if re.search('ssh-ed25519 ' + r'AAAA[A-Za-z0-9+/]{30}', text):
                problems.append(f'Access key: {rel}')
        except UnicodeDecodeError:
            if args.source and p.suffix.lower() not in {'.png', '.ttf'}:
                problems.append(f'Unexpected binary in source export: {rel}')
    manifest.append({'path': rel.as_posix(), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
if problems:
    raise SystemExit('\n'.join(problems))
if args.manifest:
    args.manifest.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print('PASS artifact audit:', len(manifest), 'files')
