#!/usr/bin/env python3
"""Bound one admitted navigation command independently of the queue worker."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import time


def group_processes(group):
    """Linux diagnostics for this job's process group only."""
    processes = []
    for path in Path("/proc").glob("[0-9]*"):
        try:
            pid = int(path.name)
            if os.getpgid(pid) != group:
                continue
            command = (path / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace").strip()
            processes.append(dict(pid=pid, command=command,
                                  wait=(path / "wchan").read_text().strip()))
        except (OSError, ValueError):
            continue
    return processes


def run(command, max_seconds, *, heartbeat_seconds=30):
    started = time.monotonic()
    # Leave the queue time to stop and archive a failed job.
    deadline_seconds = max_seconds - min(60, max_seconds * .1)
    child = subprocess.Popen(command, start_new_session=True,
                             env={**os.environ, "PYTHONUNBUFFERED": "1", "PYTHONFAULTHANDLER": "1"})

    def emit(event, **values):
        print(json.dumps(dict(event=event, elapsed_seconds=round(time.monotonic() - started, 3),
                              pid=child.pid, **values)), flush=True)

    def send_group(sig):
        try:
            os.killpg(child.pid, sig)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            # macOS may retain an exited group with no signalable members.
            if child.poll() is not None:
                return False
            raise

    def interrupted(sig, _frame):
        raise SystemExit(128 + sig)

    previous = {sig: signal.signal(sig, interrupted)
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    emit("nav_job_start", timeout_seconds=deadline_seconds, gq_max_seconds=max_seconds, command=command)
    try:
        while True:
            remaining = deadline_seconds - (time.monotonic() - started)
            if remaining <= 0:
                processes = group_processes(child.pid)
                if not processes and child.poll() is None:
                    processes = [dict(pid=child.pid, command=" ".join(command), wait="unavailable")]
                emit("nav_job_timeout", processes=processes)
                # -X faulthandler remains enabled even with Python -I. Signal
                # Python workers individually so their waiting shell stays alive
                # to reap them; never abort an unrelated process or service.
                for process in processes:
                    executable = process["command"].split(" ", 1)[0]
                    if Path(executable).name.startswith("python"):
                        try:
                            os.kill(process["pid"], signal.SIGABRT)
                        except ProcessLookupError:
                            pass
                time.sleep(.2)
                send_group(signal.SIGTERM)
                # Keep supervising after the immediate shell exits: descendants
                # may still be blocked in NAS I/O or ignoring TERM.
                grace = time.monotonic() + 5
                while time.monotonic() < grace and send_group(0):
                    child.poll()
                    time.sleep(.1)
                result = 124
                break
            try:
                result = child.wait(timeout=min(heartbeat_seconds, remaining))
                break
            except subprocess.TimeoutExpired:
                emit("nav_job_heartbeat", processes=group_processes(child.pid))
    finally:
        send_group(signal.SIGKILL)
        try:
            child.wait(timeout=1)
        except subprocess.TimeoutExpired:
            emit("nav_job_unreaped", detail="Worker did not exit after SIGKILL")
        for sig, handler in previous.items():
            signal.signal(sig, handler)
    emit("nav_job_end", exit=result)
    return result if result >= 0 else 128 - result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--max-seconds", type=float, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not math.isfinite(args.max_seconds) or args.max_seconds <= 0:
        parser.error("--max-seconds must be positive and finite")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("A navigation command is required after --")
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    return run(command, args.max_seconds)


if __name__ == "__main__":
    sys.exit(main())
