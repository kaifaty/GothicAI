#!/usr/bin/env python3
"""Hold fd 9 until gq is terminal, or a request stays unregistered for 60s."""
import json
import os
from pathlib import Path
import runpy
import signal
import subprocess
import sys
import time


def report(message):
    try:
        print(message, flush=True)
    except OSError:
        pass  # The lock must outlive a disconnected SSH client's output pipe.


def wait_terminal(state, run, complete, queue_home, version, terminal):
    marker = f"RYZEN_RUN_ID={run}"
    script = str(state / "source/scripts/ryzen.sh")
    observed = set()
    registration_deadline = time.monotonic() + 60
    next_report = 0
    while True:
        try:
            path = queue_home / "state.json"
            snapshot = json.loads(path.read_text()) if path.exists() else {"version": version, "jobs": []}
            if snapshot["version"] != version:
                raise ValueError("incompatible gq state version")
            jobs = {job["id"]: job for job in snapshot["jobs"]
                    if marker in job["command"] and script in job["command"]}
            observed.update(jobs)
            if observed and all(job_id in jobs and jobs[job_id]["status"] in terminal for job_id in observed):
                return
            # A normal gq return may be a validation error before submission.
            # Interrupted submission gets a registration grace period; any
            # observed request must reach terminal state regardless of age.
            if not observed and complete:
                return
            detail = "waiting for gq registration" if not observed else "waiting for terminal gq state"
        except (OSError, ValueError, KeyError, TypeError) as error:
            detail = str(error)
        if not observed and not complete and time.monotonic() >= registration_deadline:
            report(f"Ryzen: no gq request registered for {run} in 60s; releasing")
            return
        if time.monotonic() >= next_report:
            report(f"Ryzen: keeping worktree lock for {run}: {detail}")
            next_report = time.monotonic() + 60
        time.sleep(0.2)


def main():
    state, gq_script = map(Path, sys.argv[1:])
    # SSH/parent termination must not drop fd 9 during asynchronous unit stop.
    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, signal.SIG_IGN)
    gq = runpy.run_path(str(gq_script), run_name="ryzen_queue_metadata")
    queue_home = Path(os.environ.get("GQ_HOME", "~/gothic-ai/queue")).expanduser().resolve()
    run = None
    complete = confirmed = False
    report("Ryzen: queue guard ready")
    for line in sys.stdin:
        action, request = line.rstrip("\n").split(" ", 1)
        if action == "probe" and (run is None or confirmed):
            arguments = json.loads(request)
            result = subprocess.run(
                ["bash", str(state / "source/scripts/ryzen.sh"), "__remote", "probe",
                 arguments["action"], str(state), arguments["run"], arguments["scenario"]],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, check=False,
            )
            # SSH transports stdout/stderr separately: finish the merged child
            # output before reporting the marker that the launcher waits for.
            if result.stdout:
                report(result.stdout.removesuffix("\n"))
            report(f'Ryzen: preparation check {arguments["run"]} exit {result.returncode}')
        elif action == "arm" and (run is None or confirmed):
            run = request
            complete = confirmed = False
            report(f"Ryzen: queue guard armed {run}")
        elif request == run and action == "complete":
            complete = True
        elif request == run and action == "drain":
            wait_terminal(state, run, complete, queue_home, gq["FORMAT_VERSION"], gq["TERMINAL"])
            confirmed = True
            report(f"Ryzen: queue guard complete {run}")
        else:
            raise ValueError("invalid worktree-lock control message")
    if run is not None and not confirmed:
        wait_terminal(state, run, complete, queue_home, gq["FORMAT_VERSION"], gq["TERMINAL"])


if __name__ == "__main__":
    main()
