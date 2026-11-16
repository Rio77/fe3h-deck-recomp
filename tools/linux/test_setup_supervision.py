"""Real child-process regression checks for the graphical import completion path."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from install_core import InstallError
from manage import wait_for_preparation
from processes import stop_session


class SetupSupervision(unittest.TestCase):
    def run_worker(self, code, expected_error=False, *, fast_stop=False, headless=False):
        with tempfile.TemporaryDirectory() as temporary:
            data = Path(temporary)
            proc = subprocess.Popen([sys.executable, '-c', code], cwd=data,
                                    start_new_session=True, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL)
            try:
                with patch('manage.stop_session', side_effect=lambda p: stop_session(p, grace=.2)) if fast_stop else patch('manage.stop_session', wraps=stop_session):
                    if expected_error:
                        with self.assertRaises(InstallError):
                            wait_for_preparation(proc, data, headless, data / 'setup.log', timeout=10, settle=.2)
                    else:
                        wait_for_preparation(proc, data, headless, data / 'setup.log', timeout=10, settle=.2)
                return proc.returncode
            finally:
                stop_session(proc, grace=.2)

    ready = "from pathlib import Path;Path('extracted').mkdir();Path('extracted/fe3hgame.so.id').write_text('test');Path('fe3hrecomp.log').write_text('[gamebuild] done in 1s\\n');"

    def test_requested_completed_build_interrupt_is_not_a_failure(self):
        code = 'import signal,time;' + self.ready + 'signal.signal(signal.SIGINT,signal.SIG_DFL);time.sleep(30)'
        self.assertEqual(self.run_worker(code), -2)

    def test_external_interruption_is_still_a_failure(self):
        self.assertEqual(self.run_worker('raise SystemExit(130)', expected_error=True), 130)

    def test_forced_kill_is_not_mistaken_for_completed_build_success(self):
        code = 'import signal,time;signal.signal(signal.SIGINT,signal.SIG_IGN);' + self.ready + 'time.sleep(30)'
        self.assertEqual(self.run_worker(code, expected_error=True, fast_stop=True), -9)

    def test_normal_headless_completion_remains_successful(self):
        self.assertEqual(self.run_worker(self.ready, headless=True), 0)


if __name__ == '__main__':
    unittest.main()
