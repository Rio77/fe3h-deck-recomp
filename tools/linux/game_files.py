"""Validate original FE3H 1.2.0 data, independently of the generated library ID.

Only format metadata and build identifiers are included; no game bytes.
SPDX-License-Identifier: MIT
"""
from pathlib import Path
import os
import struct
import hashlib

PROGRAM_ID = 0x010055D009F78000
NSO_IDS = {
    'main': '89048449ba238c8cf565518b83bf02d300000000000000000000000000000000',
    'sdk': 'b2872a7bcc589969fb7afb8e372414e4fac5a750000000000000000000000000',
    'subsdk0': '8d073b25b9abda677e6967cef9ef6070178e374e000000000000000000000000',
}
ROMFS_REQUIRED = {'DATA0.bin': 997120, 'DATA1.bin': 6936719456, 'patch4/INFO0.bin': 348480}
ROMFS_FILES = 3696
DLC_IDS = {f'010055D009F7900{i}' for i in range(1, 7)}
DLC_DATA1_SIZES = dict(zip(sorted(DLC_IDS), [427676000, 128944512, 47980256, 42816, 0, 11311616]))


class GameFilesError(ValueError):
    pass


def verify_copy(source, target):
    """Compare imported files; hard links are already byte-identical."""
    source, target = Path(source).resolve(strict=True), Path(target).resolve(strict=True)
    for original in files_in(source):
        rel = original.relative_to(source)
        if any(part.startswith('.') for part in rel.parts):
            continue
        copied = target / rel
        before = original.stat()
        if not copied.is_file() or copied.stat().st_size != before.st_size:
            raise GameFilesError(f'The imported copy is incomplete: {rel}')
        other = copied.stat()
        if (before.st_dev, before.st_ino) != (other.st_dev, other.st_ino):
            hashes = []
            for p in (original, copied):
                h = hashlib.sha256()
                with p.open('rb') as f:
                    for chunk in iter(lambda: f.read(1024 * 1024), b''):h.update(chunk)
                hashes.append(h.digest())
            if hashes[0] != hashes[1]:
                raise GameFilesError(f'The imported copy differs from the source: {rel}')
        after = original.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise GameFilesError(f'The input changed during verification: {rel}')


def files_in(root):
    root = Path(root).resolve(strict=True)
    files = []
    for directory, children, names in os.walk(root, followlinks=False):
        for name in children + names:
            p = Path(directory) / name
            if p.is_symlink():
                raise GameFilesError(f'The extracted files contain a symbolic link: {p.relative_to(root)}')
        for name in names:
            p = Path(directory) / name
            if not p.is_file():
                raise GameFilesError(f'The extracted data contains a special file: {p.relative_to(root)}')
            files.append(p)
    return files


def validate_exefs(folder):
    folder = Path(folder).resolve(strict=True)
    for name, expected in NSO_IDS.items():
        try:
            with (folder / name).open('rb') as f:
                header = f.read(0x80)
        except OSError as e:
            raise GameFilesError(f'Missing or unreadable exefs/{name}.') from e
        if len(header) != 0x80 or header[:4] != b'NSO0' or header[0x40:0x60].hex() != expected:
            raise GameFilesError(f'exefs/{name} is not the supported FE3H 1.2.0 module.')
    try:
        with (folder / 'main.npdm').open('rb') as f:
            meta = f.read(0x80)
            if len(meta) != 0x80 or meta[:4] != b'META':
                raise GameFilesError('Invalid exefs/main.npdm.')
            offset = struct.unpack_from('<I', meta, 0x70)[0]
            if offset > 1024 * 1024:
                raise GameFilesError('Invalid application metadata offset.')
            f.seek(offset)
            aci = f.read(0x18)
        if len(aci) != 0x18 or aci[:4] != b'ACI0' or struct.unpack_from('<Q', aci, 0x10)[0] != PROGRAM_ID:
            raise GameFilesError('The extracted application is not Fire Emblem: Three Houses.')
    except OSError as e:
        raise GameFilesError('Missing or unreadable exefs/main.npdm.') from e
    return folder


def validate_romfs(folder):
    folder = Path(folder).resolve(strict=True)
    if not (folder / 'nx').is_dir():
        raise GameFilesError('The base romfs is missing its nx directory.')
    count = sum(not any(part.startswith('.') for part in p.relative_to(folder).parts) for p in files_in(folder))
    if count != ROMFS_FILES:
        raise GameFilesError(f'The base romfs has {count} files; a clean merged FE3H 1.2.0 copy has {ROMFS_FILES}. Use data mods separately.')
    for name, size in ROMFS_REQUIRED.items():
        p = folder / name
        if not p.is_file() or p.stat().st_size != size:
            raise GameFilesError(f'romfs/{name} is missing or does not match the supported 1.2.0 layout.')
    return folder


def validate_dlc(folder):
    if folder is None:
        return {}
    folder = Path(folder).resolve(strict=True)
    packs = {}
    for p in sorted(folder.iterdir()):
        if p.name.startswith('.'):
            continue
        if p.name.upper() not in DLC_IDS or not p.is_dir():
            raise GameFilesError(f'Unrecognized DLC folder: {p.name}. Select the folder containing FE3H DLC packs only.')
        romfs = p / 'romfs'
        if not romfs.is_dir():
            raise GameFilesError(f'DLC {p.name} needs an extracted romfs folder, not an NSP/NCA archive.')
        files = files_in(romfs)
        required = {'DATA0.bin': 33920, 'DATA1.bin': DLC_DATA1_SIZES[p.name.upper()], 'DlcVersion.bin': 1}
        for name, size in required.items():
            if not (romfs / name).is_file() or (romfs / name).stat().st_size != size:
                raise GameFilesError(f'DLC {p.name} is incomplete: {name}.')
        if p.name.upper().endswith('003'):
            for name in ('BGM_DLC.ktsl2stbin', 'BGM_DLC_EN.ktsl2stbin', 'MV_S00.webm'):
                if not (romfs / name).is_file() or (romfs / name).stat().st_size == 0:
                    raise GameFilesError(f'DLC {p.name} is incomplete: {name}.')
        if any(f.suffix.lower() in ('.nsp', '.nca', '.xci', '.nsz') for f in files):
            raise GameFilesError(f'DLC {p.name} contains packed data instead of extracted files.')
        packs[p.name.upper()] = p
    return packs
