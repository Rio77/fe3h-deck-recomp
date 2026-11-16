"""Stop only the private session created for an application/import process.
SPDX-License-Identifier: MIT
"""
from pathlib import Path
import os
import signal
import subprocess
import time


def session_members(identity):
    result = []
    for p in Path('/proc').glob('[0-9]*/stat'):
        try:
            fields = p.read_text().rsplit(')', 1)[1].split()
            if int(fields[3]) == identity and fields[0] not in ('Z', 'X'):
                result.append(int(p.parent.name))
        except (OSError, ValueError, IndexError):
            pass
    return result


def stop_session(process, grace=30):
    identity = process.pid
    if identity == os.getsid(0):
        raise RuntimeError('Refusing to stop the launcher session')
    if process.poll() is None:
        try:
            os.killpg(identity, signal.SIGINT)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            pass
    # Compiler children have their own process groups, but stay in this private
    # session. Stop producers before enumerating again to catch their children.
    for _ in range(3):
        for pid in session_members(identity):
            try:
                if os.getsid(pid) == identity:os.kill(pid, signal.SIGSTOP)
            except ProcessLookupError:
                pass
    for pid in session_members(identity):
        try:
            if os.getsid(pid) == identity:os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.wait(timeout=10)
