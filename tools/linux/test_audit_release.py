from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class AuditTests(unittest.TestCase):
    def audit(self, root):
        return subprocess.run([sys.executable,str(Path(__file__).with_name('audit_release.py')),'--source',str(root)],capture_output=True,text=True)

    def test_clean_source_and_generated_or_private_files(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'README.md').write_text('synthetic clean source')
            self.assertEqual(self.audit(p).returncode,0)
            for name,payload in [('fe3hgame.so',b'fake library'),('unrelated.dat',b'NS'+b'O0'+b'fake module'),('rclone.conf',b'synthetic private config'),('capture.png',b'synthetic capture'),('source.key',b'-----BEGIN '+b'OPENSSH PRIVATE KEY-----\nsynthetic')]:
                with self.subTest(name=name):
                    f=p/name;f.write_bytes(payload)
                    self.assertNotEqual(self.audit(p).returncode,0)
                    f.unlink()

    def test_build_directory_is_not_a_source_export(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'build').mkdir();(p/'build/private.txt').write_text('synthetic')
            self.assertNotEqual(self.audit(p).returncode,0)

    def test_escaping_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t);(p/'escape').symlink_to(p.parent)
            self.assertNotEqual(self.audit(p).returncode,0)


if __name__=='__main__':unittest.main()
