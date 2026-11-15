"""Overlapping Ryzen runs of scripts/perf.sh, with ssh, rsync and the queue replaced by stubs.

Two runs of one checkout share a source tree and a build on the stand, so the second must wait
(scripts/perf_lock.py); runs of different checkouts have their own stand folders and do not wait.
"""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import pytest


SCRIPTS = Path(__file__).parents[1]

SSH = r'''#!/usr/bin/env bash
# ssh -o K=V ... HOST COMMAND: run the command here, with the fake Ryzen home.
while [[ "$1" == -* ]]; do shift 2; done
shift
HOME="$PERF_TEST_REMOTE_HOME" exec bash -c "$1"
'''

RSYNC = r'''#!/usr/bin/env bash
printf '%s\trsync\t%s\n' "$PERF_TEST_RUN" "${@: -1}" >>"$PERF_TEST_LOG"
'''

GQ = r'''#!/usr/bin/env bash
# Records the job and what it would run; holds it "in the queue" until the gate file appears.
name='' script='' fingerprint=''
for ((i = 1; i <= $#; i++)); do
  [[ "${!i}" != --name ]] || { j=$((i + 1)); name="${!j}"; }
  [[ "${!i}" != */scripts/perf.sh ]] || script="${!i}"
  [[ "${!i}" != PERF_SOURCE_FINGERPRINT=* ]] || fingerprint="${!i#*=}"
done
printf '%s\tgq-start\t%s\t%s\t%s\n' "$PERF_TEST_RUN" "$name" "$script" "$fingerprint" >>"$PERF_TEST_LOG"
if [[ -n "${PERF_TEST_GATE:-}" ]]; then
  for ((i = 0; i < 600; i++)); do
    [[ ! -e "$PERF_TEST_GATE" ]] || break
    sleep 0.05
  done
fi
printf '%s\tgq-end\t%s\n' "$PERF_TEST_RUN" "$name" >>"$PERF_TEST_LOG"
'''


def wait_for(condition, what, timeout=20):
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, f"timed out waiting for {what}"
        time.sleep(0.02)


class Stand:
    def __init__(self, tmp_path):
        self.tmp = tmp_path
        self.log = tmp_path / "log"
        self.log.touch()
        self.home = tmp_path / "ryzen-home"
        self.home.mkdir()
        self.bin = tmp_path / "bin"
        self.bin.mkdir()
        for name, text in (("ssh", SSH), ("rsync", RSYNC)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        self.processes = []

    def checkout(self, name):
        root = self.tmp / name
        (root / "scripts/bench").mkdir(parents=True)
        (root / "scripts/convert").mkdir()
        (root / "OpenGothic").mkdir()
        (root / "OpenGothic/main.cpp").write_text(name)
        for file in ("perf.sh", "perf_lock.py"):
            shutil.copyfile(SCRIPTS / file, root / "scripts" / file)
        for file in ("perf_report.py", "ryzen_game.py", "ryzen-build.sh", "base-ensure.sh",
                     "bench/perf-market.bench"):
            (root / "scripts" / file).write_text("")
        (root / "scripts/gq").write_text(GQ)
        (root / "scripts/gq").chmod(0o755)
        return root

    def start(self, root, run, gate=None):
        environment = dict(os.environ, BENCH_HOST="ryzen", PERF_TEST_RUN=run,
                           PERF_TEST_LOG=str(self.log), PERF_TEST_REMOTE_HOME=str(self.home),
                           PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}")
        for name in ("PERF_STAND_LOCKED", "PERF_SOURCE_FINGERPRINT", "NO_BUILD", "PERF_TEST_GATE"):
            environment.pop(name, None)
        if gate is not None:
            environment["PERF_TEST_GATE"] = str(gate)
        stderr = (self.tmp / f"stderr-{run}").open("w+")
        process = subprocess.Popen(["bash", str(root / "scripts/perf.sh"), "market"], env=environment,
                                   stdout=subprocess.DEVNULL, stderr=stderr, start_new_session=True)
        process.errors = stderr
        self.processes.append(process)
        return process

    def lines(self, run=None):
        rows = [line.split("\t") for line in self.log.read_text().splitlines()]
        return rows if run is None else [row for row in rows if row[0] == run]

    def queued(self, run, job):
        return any(row[1:3] == ["gq-start", job] for row in self.lines(run))

    def stderr(self, process):
        process.errors.seek(0)
        return process.errors.read()

    def stop(self):
        for process in self.processes:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            process.errors.close()


@pytest.fixture
def stand(tmp_path):
    fixture = Stand(tmp_path)
    yield fixture
    fixture.stop()


def test_second_run_of_one_checkout_waits_for_the_first(stand):
    root = stand.checkout("one")
    gate = stand.tmp / "gate"
    first = stand.start(root, "A", gate)
    wait_for(lambda: stand.queued("A", "build perf stand"), "the first run's build job")

    second = stand.start(root, "B")
    wait_for(lambda: "waiting for it to finish" in stand.stderr(second), "the second run to wait")
    # An edit made while the second run waits belongs to the second run only.
    (root / "OpenGothic/main.cpp").write_text("edited while the first run was queued")
    time.sleep(0.2)
    assert stand.lines("B") == []
    assert first.poll() is None

    gate.touch()
    assert first.wait(timeout=20) == 0, stand.stderr(first)
    assert second.wait(timeout=20) == 0, stand.stderr(second)
    rows = stand.lines()
    assert max(i for i, row in enumerate(rows) if row[0] == "A") < min(
        i for i, row in enumerate(rows) if row[0] == "B")
    fingerprints = {run: {row[4] for row in stand.lines(run) if row[1] == "gq-start"} for run in "AB"}
    assert all(len(values) == 1 and "" not in values for values in fingerprints.values())
    assert fingerprints["A"] != fingerprints["B"]
    assert [row[2] for row in stand.lines("B") if row[1] == "gq-start"] == ["build perf stand", "perf stand"]


def test_runs_of_different_checkouts_overlap_in_their_own_stand_folders(stand):
    gate = stand.tmp / "gate"
    first = stand.start(stand.checkout("one"), "A", gate)
    wait_for(lambda: stand.queued("A", "build perf stand"), "the first run's build job")

    second = stand.start(stand.checkout("two"), "B")
    assert second.wait(timeout=20) == 0, stand.stderr(second)
    assert "waiting" not in stand.stderr(second)
    assert first.poll() is None
    gate.touch()
    assert first.wait(timeout=20) == 0, stand.stderr(first)

    sources = {}
    for run, name in (("A", "one"), ("B", "two")):
        scripts = {row[3] for row in stand.lines(run) if row[1] == "gq-start"}
        assert len(scripts) == 1
        source = Path(scripts.pop()).parents[1]
        assert source.parent.name.startswith(name + "-")
        assert source.parent.parent == stand.home / "gothic-ai/perf-stand"
        uploads = [row[2] for row in stand.lines(run) if row[1] == "rsync" and row[2].startswith("ryzen:")]
        assert uploads and all(target.startswith(f"ryzen:{source}/") for target in uploads)
        sources[run] = source
    assert sources["A"] != sources["B"]


def test_lock_is_not_held_by_a_child_that_outlives_the_command(tmp_path):
    lock = tmp_path / "lock"
    command = [sys.executable, str(SCRIPTS / "perf_lock.py"), str(lock)]
    lingering = subprocess.run([*command, "bash", "-c", "sleep 30 >/dev/null 2>&1 & echo $!"],
                               check=True, capture_output=True, text=True, timeout=10)
    try:
        os.kill(int(lingering.stdout), 0)
        assert subprocess.run([*command, "bash", "-c", "exit 7"], timeout=10).returncode == 7
    finally:
        os.kill(int(lingering.stdout), signal.SIGKILL)
