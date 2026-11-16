from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time
import unittest
from processes import stop_session, session_members


class ProcessTests(unittest.TestCase):
    def test_cancel_stops_nested_compiler_group_but_preserves_other_session(self):
        with tempfile.TemporaryDirectory() as t:
            script=Path(t)/'parent.py'
            script.write_text("import subprocess,sys,signal,time,os\nsignal.signal(signal.SIGINT,signal.SIG_IGN)\np=subprocess.Popen([sys.executable,'-c','import signal,time;signal.signal(signal.SIGINT,signal.SIG_IGN);time.sleep(120)'],preexec_fn=os.setpgrp)\nprint(p.pid,flush=True)\ntime.sleep(120)\n")
            other=subprocess.Popen([sys.executable,'-c','import time;time.sleep(120)'],start_new_session=True)
            parent=subprocess.Popen([sys.executable,str(script)],start_new_session=True,stdout=subprocess.PIPE,text=True)
            try:
                child=int(parent.stdout.readline())
                self.assertIn(child,session_members(parent.pid))
                stop_session(parent,grace=.1)
                deadline=time.monotonic()+5
                while session_members(parent.pid) and time.monotonic()<deadline:time.sleep(.05)
                self.assertFalse(session_members(parent.pid))
                self.assertIsNone(other.poll())
            finally:
                stop_session(parent,grace=.1)
                parent.stdout.close()
                other.terminate();other.wait()


if __name__=='__main__':unittest.main()
