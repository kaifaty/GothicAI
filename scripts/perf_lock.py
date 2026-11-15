#!/usr/bin/env python3
"""Run a command under an exclusive file lock, waiting for the lock first.

Usage: perf_lock.py LOCKFILE COMMAND [ARGS...]

scripts/perf.sh uses it to keep one Ryzen run per checkout. The lock lives in
this parent process and the command does not inherit it: a child that outlives
the run (a shared SSH master connection) cannot keep the lock held.
"""

import fcntl
import os
import signal
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    path, command = sys.argv[1], sys.argv[2:]
    # Python opens files non-inheritable, so the command never sees this descriptor.
    with open(path, "a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            lock.seek(0)
            holder = lock.read().strip() or "unknown"
            print(f"perf: another Ryzen run of this checkout is active (pid {holder}); "
                  "waiting for it to finish", file=sys.stderr, flush=True)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX)
            except KeyboardInterrupt:
                return 130
        lock.seek(0)
        lock.truncate()
        lock.write(f"{os.getpid()}\n")
        lock.flush()
        child = subprocess.Popen(command)

        def stop(signum, _frame):
            if child.poll() is None:
                child.send_signal(signum)

        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, stop)
        result = child.wait()
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    sys.exit(main())
