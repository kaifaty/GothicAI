"""The remote worktree lease survives EOF until its exact queue job ends."""
from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import re
import runpy
import select
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
RUN = "test-run"


class Guardian:
    def __init__(self, directory):
        self.root = Path(directory)
        self.state = self.root / "worktree"
        self.queue_home = self.root / "queue"
        self.queue_home.mkdir()
        self.lock = self.root / "worktree.lock"
        self.output = b""
        # The controller acquires the lock, then execs the guardian with fd 9.
        # Use Python for the fd handoff so this also runs on macOS without flock.
        holder = self.lock.open("a")
        fcntl.flock(holder, fcntl.LOCK_EX)
        launcher = """
import os
import sys
source_fd = int(sys.argv[1])
os.dup2(source_fd, 9)
os.set_inheritable(9, True)
if source_fd != 9:
    os.close(source_fd)
os.execv(sys.executable, [sys.executable, '-u', *sys.argv[2:]])
"""
        try:
            self.process = subprocess.Popen(
                [sys.executable, "-c", launcher, str(holder.fileno()),
                 str(ROOT / "scripts/ryzen-lock.py"), str(self.state),
                 str(ROOT / "scripts/gq.py")],
                pass_fds=(holder.fileno(),), stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=dict(os.environ, GQ_HOME=str(self.queue_home)),
            )
        finally:
            holder.close()
        self.wait_output("Ryzen: queue guard ready")

    def wait_output(self, message, timeout=5):
        expected = message.encode() + b"\n"
        deadline = time.monotonic() + timeout
        while expected not in self.output:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"no guardian message {message!r}: {self.output!r}")
            if not select.select([self.process.stdout], [], [], remaining)[0]:
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                stderr = self.process.stderr.read().decode()
                raise AssertionError(f"guardian exited before {message!r}: {stderr}")
            self.output += chunk

    def send(self, message):
        self.process.stdin.write((message + "\n").encode())
        self.process.stdin.flush()

    def arm(self, run=RUN):
        self.send(f"arm {run}")
        self.wait_output(f"Ryzen: queue guard armed {run}")

    def eof(self):
        self.process.stdin.close()

    def snapshot(self, status, *, run=RUN, script=None):
        job = {
            "id": "job-test", "status": status,
            "command": ["env", f"RYZEN_RUN_ID={run}", "bash",
                        script or str(self.state / "source/scripts/ryzen.sh"),
                        "__remote", "pytest", str(self.state), run],
        }
        self.write_state({"version": 1, "jobs": [job]})

    def write_state(self, snapshot):
        temporary = self.queue_home / "state.json.tmp"
        temporary.write_text(json.dumps(snapshot) + "\n")
        temporary.replace(self.queue_home / "state.json")

    def locked(self):
        with self.lock.open("a") as holder:
            try:
                fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
        return False

    def assert_retained(self, case, duration=0.3):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            case.assertIsNone(self.process.poll(), self.output)
            case.assertTrue(self.locked(), "worktree lock was released before terminal state")
            time.sleep(0.02)

    def assert_released(self, case):
        case.assertEqual(self.process.wait(timeout=5), 0)
        case.assertFalse(self.locked())

    def close(self):
        if self.process.poll() is None:
            self.process.kill()  # Only our isolated guardian; it ignores TERM by design.
        self.process.wait(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            if stream is not None and not stream.closed:
                stream.close()


class RyzenLockTests(unittest.TestCase):
    def guardian(self):
        directory = self.enterContext(tempfile.TemporaryDirectory(prefix="ryzen-lock-test-"))
        guardian = Guardian(directory)
        self.addCleanup(guardian.close)
        return guardian

    def test_waiter_reopens_lock_retired_by_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='ryzen-retired-lock-test-') as directory:
            root = Path(directory)
            lock_path = root / 'worktree.lock'
            waits = root / 'waits.jsonl'
            acquired = root / 'acquired.json'
            binaries = root / 'bin'
            binaries.mkdir()
            flock = binaries / 'flock'
            flock.write_text(f'#!{sys.executable}\n' + r'''
import fcntl
import json
import os
from pathlib import Path
import sys
import time
assert sys.argv[1] == '-w', sys.argv
fd = int(sys.argv[3])
with Path(os.environ['TEST_WAITS']).open('a') as stream:
    stream.write(json.dumps({'inode': os.fstat(fd).st_ino, 'wait': sys.argv[2]}) + '\n')
deadline = time.monotonic() + float(sys.argv[2])
while True:
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        break
    except BlockingIOError:
        if time.monotonic() >= deadline:
            sys.exit(1)
        time.sleep(0.01)
''')
            flock.chmod(0o755)
            source = (ROOT / 'scripts/ryzen.sh').read_text()
            take_lock = re.search(r'^take_lock\(\) \{\n.*?^\}', source, re.M | re.S)
            self.assertIsNotNone(take_lock)
            harness = 'set -euo pipefail\n' + take_lock.group(0) + r'''
take_lock
python3 - <<'PY'
import json
import os
from pathlib import Path
target = Path(os.environ['TEST_ACQUIRED'])
temporary = target.with_suffix('.tmp')
temporary.write_text(json.dumps({'inode': os.fstat(9).st_ino}))
temporary.replace(target)
PY
IFS= read -r release
'''

            def waiting(count):
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    records = waits.read_text().splitlines() if waits.exists() else []
                    if len(records) >= count:
                        return [json.loads(record) for record in records]
                    self.assertIsNone(process.poll(), 'wrapper exited while waiting')
                    time.sleep(0.01)
                self.fail(f'expected {count} lock attempts, got {records!r}')

            old = self.enterContext(lock_path.open('a'))
            fcntl.flock(old, fcntl.LOCK_EX | fcntl.LOCK_NB)
            old_inode = os.fstat(old.fileno()).st_ino
            process = subprocess.Popen(
                ['bash', '-c', harness], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env=dict(os.environ, WORKTREE_LOCK=str(lock_path), LOCK_WAIT='15',
                         TEST_WAITS=str(waits), TEST_ACQUIRED=str(acquired),
                         PATH=str(binaries) + os.pathsep + os.environ['PATH']),
            )
            try:
                self.assertEqual(waiting(1)[0]['inode'], old_inode)
                lock_path.unlink()
                with lock_path.open('a') as current:
                    fcntl.flock(current, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    current_inode = os.fstat(current.fileno()).st_ino
                    self.assertNotEqual(current_inode, old_inode)
                    fcntl.flock(old, fcntl.LOCK_UN)
                    attempts = waiting(2)
                    self.assertEqual(attempts[1]['inode'], current_inode)
                    self.assertLessEqual(int(attempts[1]['wait']), int(attempts[0]['wait']))
                    self.assertFalse(acquired.exists(), 'wrapper acquired a retired lock')
                    self.assertIsNone(process.poll())
                deadline = time.monotonic() + 5
                while not acquired.exists() and time.monotonic() < deadline:
                    self.assertIsNone(process.poll(), 'wrapper exited before acquiring current lock')
                    time.sleep(0.01)
                self.assertTrue(acquired.exists())
                self.assertEqual(json.loads(acquired.read_text())['inode'], current_inode)
                with lock_path.open('a') as contender:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
                process.stdin.write('release\n')
                process.stdin.flush()
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stdout + stderr)
                self.assertIn('lock acquired', stdout)
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)

    def test_probe_marker_follows_all_merged_output_on_its_own_line(self):
        for trailing_newline in (False, True):
            with self.subTest(trailing_newline=trailing_newline):
                guardian = self.guardian()
                script = guardian.state / "source/scripts/ryzen.sh"
                script.parent.mkdir(parents=True)
                script.write_text(
                    f"exec {shlex.quote(sys.executable)} - <<'PY'\n" + r'''
import os
import threading

def write_lines(fd, prefix):
    for index in range(50):
        os.write(fd, f"{prefix}-{index}\n".encode())

threads = [threading.Thread(target=write_lines, args=(fd, prefix))
           for fd, prefix in ((1, "stdout"), (2, "stderr"))]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
os.write(1, b"stdout-tail|")
os.write(2, b"stderr-tail")
''' + ('os.write(2, b"\\n")\n' if trailing_newline else '') +
                    'raise SystemExit(17)\nPY\n')
                guardian.send('probe ' + json.dumps({
                    'action': 'e2e', 'run': RUN, 'scenario': 'calm',
                }))
                marker = f"Ryzen: preparation check {RUN} exit 17"
                guardian.wait_output(marker)
                guardian.eof()
                guardian.assert_released(self)
                lines = guardian.output.decode().splitlines()
                self.assertEqual(lines[0], 'Ryzen: queue guard ready')
                self.assertCountEqual(lines[1:-2], [
                    f'{prefix}-{index}' for prefix in ('stdout', 'stderr') for index in range(50)
                ])
                self.assertEqual(lines[-2:], ['stdout-tail|stderr-tail', marker])
                self.assertEqual(guardian.process.stderr.read(), b'')

    def test_registration_grace_period_is_60_seconds(self):
        guardian = self.guardian()
        namespace = runpy.run_path(str(ROOT / 'scripts/ryzen-lock.py'))
        wait = namespace['wait_terminal']
        elapsed = 0

        def sleep(seconds):
            nonlocal elapsed
            elapsed += seconds

        clock = SimpleNamespace(monotonic=lambda: elapsed, sleep=sleep)
        with patch.dict(wait.__globals__, time=clock), patch.dict(wait.__globals__, report=lambda message: messages.append(message)):
            messages = []
            wait(guardian.state, RUN, False, guardian.queue_home, 1, {'done'})
        self.assertGreaterEqual(elapsed, 60)
        self.assertLess(elapsed, 60.3)
        releasing = [message for message in messages if 'releasing' in message]
        self.assertEqual(releasing, [f'Ryzen: no gq request registered for {RUN} in 60s; releasing'])

    def test_observed_request_waits_past_registration_deadline(self):
        guardian = self.guardian()
        guardian.snapshot('running')
        namespace = runpy.run_path(str(ROOT / 'scripts/ryzen-lock.py'))
        wait = namespace['wait_terminal']
        elapsed = 0

        def sleep(_seconds):
            nonlocal elapsed
            elapsed += 61
            # Even a temporarily missing observed record cannot release it.
            if elapsed == 61:
                guardian.write_state({'version': 1, 'jobs': []})
            else:
                guardian.snapshot('done')

        clock = SimpleNamespace(monotonic=lambda: elapsed, sleep=sleep)
        messages = []
        with patch.dict(wait.__globals__, time=clock, report=messages.append):
            wait(guardian.state, RUN, False, guardian.queue_home, 1, {'done'})
        self.assertEqual(elapsed, 122)
        self.assertFalse(any('releasing' in message for message in messages))

    def test_eof_keeps_active_job_locked_until_terminal(self):
        guardian = self.guardian()
        guardian.snapshot("running")
        guardian.arm()
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot("cancelled")
        guardian.assert_released(self)

    def test_eof_waits_for_late_registration(self):
        guardian = self.guardian()
        guardian.arm()
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot("queued")
        guardian.assert_retained(self)
        guardian.snapshot("running")
        guardian.assert_retained(self)
        guardian.snapshot("done")
        guardian.assert_released(self)

    def test_invalid_queue_snapshot_keeps_lock(self):
        guardian = self.guardian()
        guardian.snapshot("running")
        guardian.arm()
        guardian.eof()
        guardian.assert_retained(self)
        temporary = guardian.queue_home / "state.json.tmp"
        temporary.write_text("{broken json")
        temporary.replace(guardian.queue_home / "state.json")
        guardian.assert_retained(self)
        guardian.snapshot("done")
        guardian.assert_released(self)

    def test_closed_output_pipe_keeps_lock_until_terminal(self):
        guardian = self.guardian()
        guardian.snapshot("running")
        guardian.arm()
        guardian.process.stdout.close()
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot("cancelled")
        guardian.assert_released(self)

    def test_complete_without_submission_releases_validation_failure(self):
        guardian = self.guardian()
        guardian.arm()
        guardian.send(f"complete {RUN}")
        guardian.send(f"drain {RUN}")
        guardian.wait_output(f"Ryzen: queue guard complete {RUN}")
        self.assertTrue(guardian.locked(), "drain must retain the lease for log collection")
        guardian.eof()
        guardian.assert_released(self)

    def test_rearm_retains_lock_between_build_and_game(self):
        guardian = self.guardian()
        build, game = RUN + '-build', RUN + '-game'
        guardian.snapshot('done', run=build)
        guardian.arm(build)
        guardian.send(f'complete {build}')
        guardian.send(f'drain {build}')
        guardian.wait_output(f'Ryzen: queue guard complete {build}')
        guardian.assert_retained(self)
        guardian.snapshot('running', run=game)
        guardian.arm(game)
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot('cancelled', run=game)
        guardian.assert_released(self)

    def test_eof_after_rearm_waits_for_game_registration(self):
        guardian = self.guardian()
        build, game = RUN + '-build', RUN + '-game'
        guardian.snapshot('done', run=build)
        guardian.arm(build)
        guardian.send(f'complete {build}')
        guardian.send(f'drain {build}')
        guardian.wait_output(f'Ryzen: queue guard complete {build}')
        guardian.arm(game)
        guardian.eof()
        # Neither complete nor drain from the build may acknowledge the game.
        guardian.assert_retained(self)
        guardian.snapshot('running', run=game)
        guardian.assert_retained(self)
        guardian.snapshot('done', run=game)
        guardian.assert_released(self)

    def test_queued_cancellation_is_terminal(self):
        guardian = self.guardian()
        guardian.snapshot("queued")
        guardian.arm()
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot("cancelled")
        guardian.assert_released(self)

    def test_foreign_run_cannot_confirm_completion(self):
        guardian = self.guardian()
        guardian.snapshot("done", run="another-run")
        guardian.arm()
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot("done")
        guardian.assert_released(self)

    def test_matching_marker_with_foreign_script_cannot_confirm_completion(self):
        guardian = self.guardian()
        guardian.snapshot("done", script=str(guardian.root / "other-worktree/source/scripts/ryzen.sh"))
        guardian.arm()
        guardian.eof()
        guardian.assert_retained(self)
        guardian.snapshot("done")
        guardian.assert_released(self)


if __name__ == "__main__":
    unittest.main()
