#!/usr/bin/env python3
"""Collect notices from the exact build inputs; run inside the build container."""
from pathlib import Path
import importlib.metadata as metadata
import json
import shutil
import subprocess
import sys

arch = sys.argv[1]
repo = Path(__file__).resolve().parents[2]
base = repo / 'build/linux' / arch
out = base / 'licenses'
out.mkdir(exist_ok=True)
names = ('license', 'copying', 'copyright', 'notice', 'authors', 'ofl')
components = []


def notices(source, destination):
    found = 0
    for directory, children, files in __import__('os').walk(source, followlinks=False):
        children[:] = [n for n in children if n not in ('.git', 'build', '__pycache__')]
        for name in files:
            if not name.lower().startswith(names):
                continue
            p = Path(directory) / name
            if p.is_symlink() or p.stat().st_size > 2 * 1024 * 1024:
                continue
            q = destination / p.relative_to(source)
            q.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, q)
            found += 1
    return found


versions = {'sdl': '3.4.16', 'ffmpeg': '9.0.2', 'shaderc': '2026.3', 'vma': '3.3.0', 'vulkan-headers': '1.4.350.1'}
for name in ('sdl', 'ffmpeg', 'shaderc', 'vma', 'vulkan-headers'):
    source = base / 'deps-build' / name
    count = notices(source, out / name)
    if not count:
        raise SystemExit(f'Missing notices for {name}')
    components.append({'component': name, 'version': versions[name], 'notice_files': count})
for name in ('imgui', 'smaa', 'fonts', 'ryujinx', 'fsr'):
    count = notices(repo / 'third_party' / name, out / name)
    if not count:
        raise SystemExit(f'Missing notices for {name}')
    components.append({'component': name, 'notice_files': count})
notices(repo / 'build/zig', out / 'zig')
components.append({'component': 'zig', 'version': subprocess.check_output([str(repo / 'build/zig/zig'), 'version'], text=True).strip()})
shutil.copy2(repo / 'LICENSE', out / 'FE3H-MIT.txt')

# This interpreter is the generator's actual virtual environment.
for name in ('capstone', 'unicorn', 'lz4', 'pyinstaller', 'pyinstaller-hooks-contrib', 'altgraph', 'packaging'):
    dist = metadata.distribution(name)
    count = 0
    for rel in dist.files or []:
        p = Path(dist.locate_file(rel))
        if p.name.lower().startswith(names) and p.is_file():
            q = out / 'generator' / name / str(rel).replace('../', '')
            q.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, q)
            count += 1
    if not count and (base / 'license-input' / name).is_dir():
        count = notices(base / 'license-input' / name, out / 'generator' / name)
    if not count:
        raise SystemExit(f'Missing installed notices for {name} {dist.version}')
    components.append({'component': name, 'version': dist.version, 'notice_files': count})

for package in ('python3.12', 'libpython3.12-stdlib', 'libbz2-1.0', 'libexpat1', 'libffi8', 'liblzma5', 'zlib1g'):
    p = Path('/usr/share/doc') / package / 'copyright'
    if not p.is_file():
        raise SystemExit(f'Missing system-runtime notice for {package}')
    q = out / 'generator-system' / package / 'copyright'
    q.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, q)
    version = subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', package], text=True)
    components.append({'component': package, 'version': version})

(out / 'components.json').write_text(json.dumps(components, indent=2), encoding='utf-8')
print('Collected notices for', len(components), 'components into', out.relative_to(repo))
