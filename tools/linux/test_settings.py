"""Settings uses the installed data and preserves the normal play command."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import manage


class Settings(unittest.TestCase):
    def test_installed_shortcuts_and_data_root(self):
        with tempfile.TemporaryDirectory(prefix='FE3H settings ') as temporary:
            root = Path(temporary) / 'Game with spaces'
            desktop = Path(temporary) / 'Desktop'
            root.mkdir(); desktop.mkdir(); (root / 'apps').mkdir()
            payload = b'#!/bin/sh\nprintf "%s\\n%s\\n" "$LARECOMP_DATA_ROOT" "$*" > "$LARECOMP_DATA_ROOT/result"\n'
            sha = hashlib.sha256(payload).hexdigest()
            app = root / 'apps' / (sha + '.AppImage')
            app.write_bytes(payload); app.chmod(0o755)
            (root / 'current.json').write_text(json.dumps({'app_sha256': sha}))
            with patch('manage.subprocess.check_output', return_value=str(desktop)):
                manage.desktop_files(root)
            self.assertTrue((root / 'Settings.desktop').is_file())
            self.assertTrue((desktop / 'FE3H-Deck-Recomp-Settings.desktop').is_file())
            for launcher, argument in [('Settings.sh', ''), ('FE3H.sh', '--play')]:
                subprocess.run([str(root / launcher)], check=True, timeout=20)
                self.assertEqual((root / 'result').read_text().splitlines(), [str(root), argument])

    def test_settings_cannot_open_during_play(self):
        from install_core import Installation
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'current.json').write_text('{}')
            with Installation(root).lock():
                result = subprocess.run([sys.executable, str(manage.HERE / 'manage.py'), 'settings', '--root', str(root), '--headless'], capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'already running', result.stderr)
