from pathlib import Path
from types import SimpleNamespace
import tempfile,unittest
from unittest.mock import patch
import manage
from install_core import InstallError
class Destinations(unittest.TestCase):
 def test_cli_defaults_and_explicit_destination(self):
  with patch('manage.dialog',side_effect=AssertionError('Unexpected dialog')):
   self.assertEqual(manage.choose_destination(SimpleNamespace(root=None,command='install',headless=True)),manage.DEFAULT_ROOT)
   self.assertEqual(manage.choose_destination(SimpleNamespace(root=Path('/tmp/custom'),command='install',headless=False)),Path('/tmp/custom'))
 def test_cancel_does_not_create_destination(self):
  with patch('manage.dialog',return_value=(2,'')):
   with self.assertRaises(InstallError):manage.choose_destination(SimpleNamespace(root=None,command='install',headless=False))
 def test_unavailable_launch_does_not_create_folder(self):
  with tempfile.TemporaryDirectory() as t:
   p=Path(t)/'absent'
   with self.assertRaises(InstallError):manage.validate_destination(p,'launch')
   self.assertFalse(p.exists())
 def test_empty_unrelated_symlink_and_filesystem(self):
  with tempfile.TemporaryDirectory(prefix='FE3H spaces ') as t:
   root=Path(t);target=root/'Game'
   self.assertEqual(manage.validate_destination(target,'install'),target)
   manage.probe_destination(target);self.assertFalse(target.exists())
   target.mkdir();(target/'unrelated').write_text('keep')
   with self.assertRaises(InstallError):manage.validate_destination(target,'install')
   alias=root/'alias';alias.symlink_to(target)
   with self.assertRaises(InstallError):manage.validate_destination(alias,'install')
