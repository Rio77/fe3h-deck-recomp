"""Regression checks for the UTF-8 documentation distributed to users."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DocumentationEncoding(unittest.TestCase):
    def test_readmes_and_maintained_docs_have_no_mojibake(self):
        files = [ROOT / name for name in ('README.md', 'README.es.md', 'THIRD_PARTY.md', 'UPSTREAM.md')]
        files += list((ROOT / 'docs').glob('*.md'))
        files.append(ROOT / 'tools/linux/README-installer.md')
        bad = re.compile(r'\u00c3[\u0080-\u00bf]|\u00c2[\u0080-\u00bf]|\u00e2(?:\u20ac|\u02c6|\u0080)|\ufffd')
        for path in files:
            with self.subTest(path=path.name):
                raw = path.read_bytes()
                self.assertFalse(raw.startswith(b'\xef\xbb\xbf'))
                self.assertIsNone(bad.search(raw.decode('utf-8')))

    def test_spanish_accents_are_preserved(self):
        self.assertIn('Espa\u00f1ol', (ROOT / 'README.md').read_text(encoding='utf-8'))
        self.assertIn('Instalaci\u00f3n', (ROOT / 'README.es.md').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
