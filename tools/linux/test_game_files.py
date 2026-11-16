from pathlib import Path
import struct
import tempfile
import unittest
from game_files import NSO_IDS, ROMFS_REQUIRED, PROGRAM_ID, GameFilesError, validate_exefs, validate_romfs, validate_dlc, verify_copy


class GameFilesTests(unittest.TestCase):
    def test_copy_verification_detects_corruption_and_truncation(self):
        with tempfile.TemporaryDirectory() as t:
            import shutil
            source, target = Path(t) / 'original', Path(t) / 'copied'
            source.mkdir();(source / 'file').write_bytes(b'original content')
            shutil.copytree(source, target);verify_copy(source, target)
            (target / 'file').write_bytes(b'original contenX')
            with self.assertRaises(GameFilesError):verify_copy(source, target)
            (target / 'file').write_bytes(b'partial')
            with self.assertRaises(GameFilesError):verify_copy(source, target)
    def exefs(self, p):
        p.mkdir()
        for name, identity in NSO_IDS.items():
            h = bytearray(0x80);h[:4] = b'NSO0';h[0x40:0x60] = bytes.fromhex(identity)
            (p / name).write_bytes(h)
        m = bytearray(0x118);m[:4] = b'META';struct.pack_into('<I', m, 0x70, 0x100)
        m[0x100:0x104] = b'ACI0';struct.pack_into('<Q', m, 0x110, PROGRAM_ID)
        (p / 'main.npdm').write_bytes(m)

    def test_metadata_validation_and_wrong_version(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'exefs with spaces';self.exefs(p)
            self.assertEqual(validate_exefs(p), p)
            h = bytearray((p / 'sdk').read_bytes());h[0x40] ^= 1;(p / 'sdk').write_bytes(h)
            with self.assertRaises(GameFilesError):validate_exefs(p)

    def test_wrong_application_and_truncation(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'exefs';self.exefs(p)
            (p / 'main.npdm').write_bytes(b'META')
            with self.assertRaises(GameFilesError):validate_exefs(p)
            self.exefs(Path(t) / 'other')
            m = bytearray((Path(t) / 'other/main.npdm').read_bytes());struct.pack_into('<Q', m, 0x110, 123)
            (p / 'main.npdm').write_bytes(m)
            with self.assertRaises(GameFilesError):validate_exefs(p)

    def test_sparse_synthetic_romfs_and_incomplete_input(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'romfs';(p / 'nx').mkdir(parents=True)
            with self.assertRaises(GameFilesError):validate_romfs(p)
            for name, size in ROMFS_REQUIRED.items():
                f = p / name;f.parent.mkdir(exist_ok=True)
                with f.open('wb') as out:out.truncate(size)
            for n in range(3693):(p / 'nx' / f'synthetic-{n}').write_bytes(b'')
            self.assertEqual(validate_romfs(p), p)
            (p / 'nx' / 'unsafe').symlink_to(Path(t))
            with self.assertRaises(GameFilesError):validate_romfs(p)

    def test_optional_dlc_and_incomplete_pack(self):
        self.assertEqual(validate_dlc(None), {})
        with tempfile.TemporaryDirectory() as t:
            p = Path(t);romfs = p / '010055D009F79001/romfs';romfs.mkdir(parents=True)
            with self.assertRaises(GameFilesError):validate_dlc(p)
            for name, size in [('DATA0.bin', 33920), ('DATA1.bin', 427676000), ('DlcVersion.bin', 1)]:
                with (romfs / name).open('wb') as f:f.truncate(size)
            self.assertEqual(list(validate_dlc(p)), ['010055D009F79001'])
            (romfs / 'packed.nsp').write_bytes(b'synthetic')
            with self.assertRaises(GameFilesError):validate_dlc(p)

    def test_fifth_dlc_has_a_legitimate_empty_data1(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t);romfs = p / '010055D009F79005/romfs';romfs.mkdir(parents=True)
            for name, size in [('DATA0.bin', 33920), ('DATA1.bin', 0), ('DlcVersion.bin', 1)]:
                with (romfs / name).open('wb') as f:f.truncate(size)
            self.assertEqual(list(validate_dlc(p)), ['010055D009F79005'])


if __name__ == '__main__':unittest.main()
