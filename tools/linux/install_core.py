"""Crash-recoverable Linux installation; never replaces saves or configuration.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import uuid


class InstallError(RuntimeError):
    pass


def sync_dir(path: Path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_json(path: Path, value):
    tmp = path.with_name(path.name + '.tmp')
    with tmp.open('w', encoding='utf-8', newline='\n') as f:
        json.dump(value, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    sync_dir(path.parent)


def digest(path: Path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def sync_tree(root):
    for directory, _, names in os.walk(root, topdown=False, followlinks=False):
        for name in names:
            p = Path(directory) / name
            if p.is_symlink():
                continue
            with p.open('rb') as f:
                os.fsync(f.fileno())
        sync_dir(directory)


class Installation:
    def __init__(self, root):
        root = Path(root).expanduser()
        if root.is_symlink():
            raise InstallError('The installation folder must not be a symbolic link.')
        self.root = root.resolve()
        if self.root == Path('/') or self.root == Path.home().resolve():
            raise InstallError('Choose a dedicated FE3H installation folder.')
        self.root.mkdir(parents=True, exist_ok=True)
        for name in ('apps', 'Amiibo', '.installer-work', '.recovery'):
            p = self.root / name
            if p.is_symlink():
                raise InstallError(f'Unsafe installation folder: {name}')
            p.mkdir(exist_ok=True)
        if (self.root / 'extracted').is_symlink():
            raise InstallError('The installed game folder must not be a symbolic link.')
        self.journal = self.root / '.install-journal.json'

    @contextlib.contextmanager
    def lock(self):
        with (self.root / '.install.lock').open('a+b') as f:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as e:
                raise InstallError('FE3H or another installer is already running.') from e
            try:
                yield
            finally:
                fcntl.flock(f, fcntl.LOCK_UN)

    def workspace(self):
        token = uuid.uuid4().hex
        p = self.root / '.installer-work' / token
        p.mkdir()
        (p / '.owned').write_text(token, encoding='ascii')
        (p / 'data').mkdir()
        return token, p

    def _paths(self, token):
        if not re.fullmatch('[0-9a-f]{32}', token):
            raise InstallError('Invalid installation recovery record.')
        work = self.root / '.installer-work' / token
        backup = self.root / '.recovery' / ('extracted-' + token)
        for p in (work, backup, self.root / 'extracted'):
            if p.is_symlink():
                raise InstallError('A recovery path is a symbolic link.')
            if not p.resolve().is_relative_to(self.root):
                raise InstallError('Recovery path escapes the installation.')
        return work, backup, self.root / 'extracted'

    def add_app(self, source, expected_sha256):
        if not re.fullmatch('[0-9a-f]{64}', expected_sha256):
            raise InstallError('Invalid application checksum.')
        target = self.root / 'apps' / (expected_sha256 + '.AppImage')
        if target.is_symlink():
            raise InstallError('The application must not be a symbolic link.')
        if target.exists():
            if digest(target) != expected_sha256:
                raise InstallError('The installed application checksum is incorrect.')
            return target
        temporary = target.with_suffix('.tmp')
        try:
            with Path(source).open('rb') as src, temporary.open('xb') as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
                dst.flush()
                os.fsync(dst.fileno())
            if digest(temporary) != expected_sha256:
                raise InstallError('The downloaded application checksum is incorrect.')
            temporary.chmod(0o755)
            os.replace(temporary, target)
            sync_dir(target.parent)
            return target
        finally:
            temporary.unlink(missing_ok=True)

    def commit(self, token, app_hash, version, failpoint=lambda _: None):
        """Caller holds lock and has verified the complete staged game build."""
        if self.journal.exists():
            raise InstallError('Recover the previous installation first.')
        work, backup, current = self._paths(token)
        if (work / '.owned').read_text(encoding='ascii') != token:
            raise InstallError('Installation workspace ownership is incorrect.')
        staged = work / 'data' / 'extracted'
        if staged.is_symlink() or not staged.is_dir() or not staged.resolve().is_relative_to(work.resolve()):
            raise InstallError('The prepared game files are missing.')
        app = self.root / 'apps' / (app_hash + '.AppImage')
        if not re.fullmatch('[0-9a-f]{64}', app_hash) or app.is_symlink() or digest(app) != app_hash:
            raise InstallError('The prepared application could not be verified.')
        # User mods are separate from the original files and are retained on reinstall.
        if (current / 'mods').exists():
            if (staged / 'mods').exists():
                raise InstallError('The staged import unexpectedly contains mods.')
            if (current / 'mods').is_symlink():
                (staged / 'mods').symlink_to(os.readlink(current / 'mods'))
            else:
                shutil.copytree(current / 'mods', staged / 'mods', symlinks=True)
        marker = staged / '.install-id'
        with marker.open('w', encoding='ascii') as f:
            f.write(token)
            f.flush()
            os.fsync(f.fileno())
        sync_tree(staged)
        entry = {'format': 1, 'token': token, 'app_sha256': app_hash, 'version': version}
        manifest = self.root / 'current.json'
        previous = json.loads(manifest.read_text(encoding='utf-8')) if manifest.exists() else None
        record = dict(entry, previous=previous)
        atomic_json(self.journal, record)
        failpoint('prepared')
        try:
            if current.exists():
                os.replace(current, backup)
                sync_dir(self.root)
                sync_dir(backup.parent)
            failpoint('old-moved')
            os.replace(staged, current)
            sync_dir(self.root)
            sync_dir(staged.parent)
            failpoint('new-moved')
            atomic_json(self.root / 'current.json', entry)
            failpoint('manifest-written')
        except Exception:
            self.recover()
            raise
        self.journal.unlink()
        sync_dir(self.root)
        return backup if backup.exists() else None

    def recover(self):
        if not self.journal.exists():
            return 'clean'
        record = json.loads(self.journal.read_text(encoding='utf-8'))
        if record.get('format') != 1:
            raise InstallError('Unsupported installation recovery record.')
        work, backup, current = self._paths(record['token'])
        app_hash = record.get('app_sha256', '')
        if not re.fullmatch('[0-9a-f]{64}', app_hash):
            raise InstallError('Invalid application in the recovery record.')
        app = self.root / 'apps' / (app_hash + '.AppImage')
        marker = current / '.install-id'
        is_new = marker.is_file() and not marker.is_symlink() and marker.read_text(encoding='ascii') == record['token']
        if is_new and app.is_file() and not app.is_symlink() and digest(app) == app_hash:
            atomic_json(self.root / 'current.json', {k: v for k, v in record.items() if k != 'previous'})
            result = 'completed'
        elif is_new and backup.is_dir():
            failed = backup.with_name('failed-' + record['token'])
            if failed.exists():
                raise InstallError('Recovery copies already exist; nothing was removed.')
            os.replace(current, failed)
            os.replace(backup, current)
            if record.get('previous') is not None:
                atomic_json(self.root / 'current.json', record['previous'])
            else:
                (self.root / 'current.json').unlink(missing_ok=True)
            sync_dir(self.root)
            sync_dir(backup.parent)
            result = 'restored'
        elif not current.exists() and backup.is_dir():
            os.replace(backup, current)
            sync_dir(self.root)
            result = 'restored'
        elif current.exists() and not is_new:
            result = 'preserved'
        elif not current.exists() and not backup.exists():
            result = 'not-installed'
        else:
            raise InstallError('Recovery needs attention; all copies were preserved.')
        self.journal.unlink()
        sync_dir(self.root)
        return result

    def cleanup_workspace(self, token):
        work, _, _ = self._paths(token)
        if self.journal.exists():
            raise InstallError('Recover the installation before removing temporary files.')
        if work.exists():
            if (work / '.owned').read_text(encoding='ascii') != token:
                raise InstallError('Refusing to remove an unowned workspace.')
            shutil.rmtree(work)
