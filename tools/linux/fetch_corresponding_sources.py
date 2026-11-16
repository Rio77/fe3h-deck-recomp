#!/usr/bin/env python3
"""Freeze corresponding upstream source archives and recover omitted wheel notices."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import shutil
import sys
import tarfile
import urllib.request

repo = Path(__file__).resolve().parents[2]
base = repo / 'build/linux' / sys.argv[1]
out = base / 'corresponding-sources'
out.mkdir(exist_ok=True)
manifest = []


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):h.update(block)
    return h.hexdigest()


ffmpeg = repo / 'build/linux/src/ffmpeg-9.0.2.tar.xz'
expected = '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e'
assert sha(ffmpeg) == expected
shutil.copy2(ffmpeg, out / ffmpeg.name)
manifest.append({'component': 'FFmpeg', 'version': '9.0.2', 'file': ffmpeg.name, 'sha256': expected,
                 'source': 'https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz'})

with urllib.request.urlopen('https://pypi.org/pypi/unicorn/2.1.4/json', timeout=60) as response:
    data = json.load(response)
source = next(item for item in data['urls'] if item['packagetype'] == 'sdist')
path = out / Path(source['filename']).name
if not path.exists():
    temporary = path.with_suffix(path.suffix + '.part')
    with urllib.request.urlopen(source['url'], timeout=120) as response, temporary.open('wb') as f:
        shutil.copyfileobj(response, f)
    temporary.replace(path)
assert sha(path) == source['digests']['sha256']
licenses = base / 'license-input/unicorn'
with tarfile.open(path) as archive:
    members = archive.getmembers()
    c_files = sum(m.isfile() and m.name.endswith(('.c', '.cpp')) for m in members)
    assert c_files > 200 and any('/qemu/' in m.name for m in members), 'Unicorn source archive lacks the core engine'
    for member in members:
        relative = PurePosixPath(member.name)
        if not member.isfile() or relative.is_absolute() or '..' in relative.parts:
            continue
        if relative.name.lower().startswith(('copying', 'license', 'notice', 'copyright', 'authors')):
            target = licenses.joinpath(*relative.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as src, target.open('wb') as dst:shutil.copyfileobj(src, dst)
manifest.append({'component': 'Unicorn', 'version': '2.1.4', 'file': path.name,
                 'sha256': source['digests']['sha256'], 'source': source['url'], 'core_source_files': c_files})
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print('Frozen corresponding sources:', ', '.join(item['component'] for item in manifest))
