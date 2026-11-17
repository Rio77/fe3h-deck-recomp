"""Content mismatches warn without rejecting user files."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import game_files


class RomfsHashes(unittest.TestCase):
    def test_runtime_and_installer_references_match(self):
        import re
        source = (Path(__file__).resolve().parents[2] / 'runtime/romfs_build.c').read_text()
        references = dict(re.findall(r'\{"([^"]+)", \d+ull, "([0-9a-f]{64})"', source))
        self.assertEqual(references, game_files.ROMFS_HASHES)

    def test_unreadable_content_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            issues = game_files.romfs_hash_warnings(Path(directory))
            self.assertEqual(len(issues), 4)

    def test_same_size_modified_content_warns(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'DATA0.bin').write_bytes(b'bad')
            reference = {'DATA0.bin': hashlib.sha256(b'yes').hexdigest()}
            with patch.object(game_files, 'ROMFS_HASHES', reference):
                issues = game_files.romfs_hash_warnings(root)
            self.assertEqual(len(issues), 1)
            self.assertIn('DATA0.bin', issues[0])
            self.assertEqual((root / 'DATA0.bin').read_bytes(), b'bad')

    def test_matching_content_has_no_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'DATA0.bin').write_bytes(b'yes')
            with patch.object(game_files, 'ROMFS_HASHES', {'DATA0.bin': hashlib.sha256(b'yes').hexdigest()}):
                self.assertEqual(game_files.romfs_hash_warnings(root), [])
