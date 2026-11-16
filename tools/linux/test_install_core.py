"""Synthetic installation tests: no game, DLC or private credentials required."""
import errno
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from install_core import Installation, InstallError, digest, atomic_json


class PowerLoss(BaseException):
    pass


class InstallationTests(unittest.TestCase):
    def prepare(self, parent, existing=True):
        root = Path(parent) / 'FE3H path with spaces'
        instance = Installation(root)
        source = Path(parent) / 'application.AppImage'
        source.write_bytes(b'synthetic executable\0version two')
        checksum = digest(source)
        instance.add_app(source, checksum)
        if existing:
            (root / 'extracted').mkdir()
            (root / 'extracted' / 'base').write_text('old')
            (root / 'extracted' / 'mods').mkdir()
            (root / 'extracted' / 'mods' / 'own-mod').write_text('keep')
            atomic_json(root / 'current.json', {'version': 'old'})
        (root / 'save').mkdir()
        (root / 'save' / 'slot').write_bytes(b'confirmed save')
        (root / 'fe3hrecomp.ini').write_text('resolution = match\n')
        token, work = instance.workspace()
        staged = work / 'data' / 'extracted'
        staged.mkdir()
        (staged / 'base').write_text('new')
        return instance, token, checksum, work

    def unchanged_user_data(self, instance):
        self.assertEqual((instance.root / 'save' / 'slot').read_bytes(), b'confirmed save')
        self.assertEqual((instance.root / 'fe3hrecomp.ini').read_text(), 'resolution = match\n')

    def test_commit_keeps_previous_files_mods_and_user_data(self):
        with tempfile.TemporaryDirectory() as t:
            i, token, sha, work = self.prepare(t)
            with i.lock():
                backup = i.commit(token, sha, 'opt5')
                self.assertEqual((i.root / 'extracted' / 'base').read_text(), 'new')
                self.assertEqual((backup / 'base').read_text(), 'old')
                self.assertEqual((i.root / 'extracted' / 'mods' / 'own-mod').read_text(), 'keep')
                self.unchanged_user_data(i)
                i.cleanup_workspace(token)
                self.assertFalse(work.exists())

    def test_power_loss_at_each_commit_boundary(self):
        for existing in [False, True]:
            for boundary in ['prepared', 'old-moved', 'new-moved', 'manifest-written']:
                with self.subTest(existing=existing, boundary=boundary), tempfile.TemporaryDirectory() as t:
                    i, token, sha, _ = self.prepare(t, existing)
                    def failpoint(name):
                        if name == boundary:
                            raise PowerLoss()
                    with self.assertRaises(PowerLoss):
                        i.commit(token, sha, 'opt5', failpoint)
                    recovered = Installation(i.root)
                    result = recovered.recover()
                    self.assertFalse(recovered.journal.exists())
                    self.assertEqual(recovered.recover(), 'clean')
                    self.unchanged_user_data(recovered)
                    if boundary in ['new-moved', 'manifest-written']:
                        self.assertEqual(result, 'completed')
                        self.assertEqual((i.root / 'extracted' / 'base').read_text(), 'new')
                    elif existing:
                        self.assertEqual((i.root / 'extracted' / 'base').read_text(), 'old')
                    else:
                        self.assertFalse((i.root / 'extracted').exists())

    def test_failed_replacement_rolls_back(self):
        with tempfile.TemporaryDirectory() as t:
            i, token, sha, work = self.prepare(t)
            import os
            replace = os.replace
            def fail_new(src, dst):
                if Path(src) == work / 'data' / 'extracted':
                    raise PermissionError('synthetic permission denial')
                return replace(src, dst)
            with patch('install_core.os.replace', side_effect=fail_new), self.assertRaises(PermissionError):
                i.commit(token, sha, 'opt5')
            self.assertEqual((i.root / 'extracted' / 'base').read_text(), 'old')
            self.assertEqual(json.loads((i.root / 'current.json').read_text()), {'version': 'old'})
            self.unchanged_user_data(i)

    def test_disk_full_before_commit_keeps_previous_installation(self):
        with tempfile.TemporaryDirectory() as t:
            i, token, sha, _ = self.prepare(t)
            with patch('install_core.os.fsync', side_effect=OSError(errno.ENOSPC, 'synthetic full disk')):
                with self.assertRaises(OSError):
                    i.commit(token, sha, 'opt5')
            self.assertFalse(i.journal.exists())
            self.assertEqual((i.root / 'extracted' / 'base').read_text(), 'old')
            self.unchanged_user_data(i)

    def test_corrupted_app_after_interruption_restores_old(self):
        with tempfile.TemporaryDirectory() as t:
            i, token, sha, _ = self.prepare(t)
            def failpoint(name):
                if name == 'new-moved':
                    raise PowerLoss()
            with self.assertRaises(PowerLoss):
                i.commit(token, sha, 'opt5', failpoint)
            (i.root / 'apps' / (sha + '.AppImage')).write_bytes(b'corrupted')
            self.assertEqual(i.recover(), 'restored')
            self.assertEqual((i.root / 'extracted' / 'base').read_text(), 'old')
            self.assertEqual(json.loads((i.root / 'current.json').read_text()), {'version': 'old'})
            self.unchanged_user_data(i)

    def test_wrong_download_hash_is_not_installed(self):
        with tempfile.TemporaryDirectory() as t:
            i, _, _, _ = self.prepare(t)
            p = Path(t) / 'wrong';p.write_bytes(b'wrong')
            with self.assertRaises(InstallError):
                i.add_app(p, '0' * 64)
            self.assertFalse((i.root / 'apps' / ('0' * 64 + '.AppImage')).exists())
            self.assertFalse(list((i.root / 'apps').glob('*.tmp')))
            self.unchanged_user_data(i)

    def test_conflicting_launch_or_install_lock(self):
        with tempfile.TemporaryDirectory() as t:
            i, _, _, _ = self.prepare(t)
            with i.lock(), self.assertRaises(InstallError):
                with Installation(i.root).lock():
                    self.fail('second lock acquired')

    def test_invalid_recovery_path_preserves_outside_files(self):
        with tempfile.TemporaryDirectory() as t:
            i, _, _, _ = self.prepare(t)
            sentinel = Path(t) / 'outside';sentinel.write_text('keep')
            atomic_json(i.journal, {'format': 1, 'token': '../../outside'})
            with self.assertRaises(InstallError):
                i.recover()
            self.assertEqual(sentinel.read_text(), 'keep')

    def test_symlinked_staging_parent_is_refused(self):
        with tempfile.TemporaryDirectory() as t:
            i, token, sha, work = self.prepare(t)
            outside = Path(t) / 'outside';outside.mkdir()
            (outside / 'extracted').mkdir()
            import shutil
            shutil.rmtree(work / 'data')
            (work / 'data').symlink_to(outside, target_is_directory=True)
            with self.assertRaises(InstallError):
                i.commit(token, sha, 'opt5')
            self.assertFalse((outside / 'extracted' / '.install-id').exists())
            self.unchanged_user_data(i)


if __name__ == '__main__':
    unittest.main()
