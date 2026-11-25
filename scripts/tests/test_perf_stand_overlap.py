"""Overlapping Ryzen runs of scripts/perf.sh, with ssh, rsync and the queue replaced by stubs.

Two runs of one checkout share a source tree and a build on the stand, so the second must wait
(scripts/perf_lock.py); runs of different checkouts have their own stand folders and do not wait.
"""
import json
import os
from pathlib import Path
import shlex
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
if [[ -n "${STUB_NAS:-}" && "$1" == *"test -d /mnt/shared/"* ]]; then test -d "$STUB_NAS"; exit; fi
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
        for file in ("perf.sh", "perf_lock.py", "perf_variants.py"):
            shutil.copy(SCRIPTS / file, root / "scripts" / file)
        for file in ("perf_report.py", "ryzen_game.py", "ryzen-build.sh", "base-ensure.sh",
                     "bench/perf-market.bench"):
            (root / "scripts" / file).write_text("")
        (root / "scripts/gq").write_text(GQ)
        (root / "scripts/gq").chmod(0o755)
        return root

    def environment(self, run):
        environment = dict(os.environ, BENCH_HOST="ryzen", PERF_TEST_RUN=run,
                           PERF_TEST_LOG=str(self.log), PERF_TEST_REMOTE_HOME=str(self.home),
                           PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}")
        for name in ("PERF_STAND_LOCKED", "PERF_SOURCE_FINGERPRINT", "NO_BUILD", "PERF_TEST_GATE"):
            environment.pop(name, None)
        return environment

    def start(self, root, run, gate=None):
        environment = self.environment(run)
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


@pytest.mark.parametrize("selection", [[], ["F64", "F64", "F1024-vsmstats"]])
def test_tree_cost_runs_the_whole_plan_in_one_quiet_job(stand, selection):
    root = stand.checkout("tree")
    recipe = Path("reports/engine/2026-11-16-modern-trees/tree-cost/run.sh")
    (root / recipe).parent.mkdir(parents=True)
    shutil.copy(SCRIPTS.parent / recipe, root / recipe)
    out = stand.tmp / "forest results"
    # Build logs may already be on the NAS when archiving the measurements fails.
    nas = stand.tmp / "nas"
    nas.mkdir()
    # The transport stages the actual variant runner. Only the per-round engine run is a stub.
    (stand.bin / "rsync").write_text(f"#!{sys.executable}\n" + r'''
from pathlib import Path
import os, shutil, sys
dest = sys.argv[-1]
if dest.endswith("/scripts/"):
    for arg in sys.argv[1:-1]:
        if arg.endswith("/perf_variants.py"):
            shutil.copy(arg, dest.split(":", 1)[1])
        elif arg.endswith("/perf.sh"):
            shutil.copy(os.environ["STUB_PERF"], Path(dest.split(":", 1)[1]) / "perf.sh")
elif sys.argv[-2].startswith("ryzen:"):
    ignore = shutil.ignore_patterns("*.png") if "--exclude=*.png" in sys.argv else None
    source = sys.argv[-2].split(":", 1)[1]
    if source.startswith("/mnt/shared/"):
        source = os.environ["STUB_NAS"]
    shutil.copytree(source, dest, dirs_exist_ok=True, ignore=ignore)
''')
    (root / "scripts/gq").write_text(f"#!{sys.executable}\n" + r'''
import json, os, shutil, subprocess, sys
args = sys.argv[1:]
kind = args[args.index("--class") + 1]
with open(os.environ["QUEUE_LOG"], "a") as log:
    print(json.dumps([kind, args[args.index("--max") + 1]]), file=log)
if kind == "quiet":
    command = args[args.index("--") + 1:]
    status = subprocess.run(command).returncode
    if not status:
        source = args[args.index("--archive") + 1]
        shutil.copytree(source, os.environ["STUB_NAS"], dirs_exist_ok=True)
        shutil.rmtree(source)
    sys.exit(status)
''')
    child = stand.tmp / "measure.sh"
    child.write_text(r'''
set -eu
[[ "$1" == __run && "$3" == trees && "$#" == 3 && "$NO_BUILD" == 1 && "$PERF_REPEAT" == 1 ]]
mkdir -p "$2"
printf '%s/%s\n' "$(basename "$(dirname "$2")")" "${2##*/}" >> "$MEASURE_LOG"
printf '%s\n' "$PERF_ENV" > "$2/settings"
touch "$2/frame.png"
[[ "$2" != */F64/round-3 ]] || exit 7
''')
    jobs = stand.tmp / "jobs"
    measured = stand.tmp / "measurements"
    environment = dict(stand.environment("forest"), STUB_PERF=str(child), QUEUE_LOG=str(jobs),
                       MEASURE_LOG=str(measured), STUB_NAS=str(nas))
    if selection:
        checkout = subprocess.check_output(["bash", "-c", 'source "$1"; perf_checkout "$2"',
                                            "bash", SCRIPTS / "perf.sh", root], text=True).strip()
        stamp = stand.home / "gothic-ai/perf-stand" / checkout / "source/OpenGothic/build-perf/source_fingerprint"
        stamp.parent.mkdir(parents=True)
        stamp.write_text("existing-build\n")
        environment.update(NO_BUILD="1", PERF_SOURCE_FINGERPRINT="existing-build")
    result = subprocess.run(["bash", root / recipe, out, *selection], text=True, capture_output=True,
                            env=environment, timeout=30)
    assert result.returncode == (7 if selection else 0), result.stdout + result.stderr
    stages = ["C", "F64", "F256", "F1024", "F1024h", "F1024q", "F1024e",
              "F1024-rt0", "C-rt0", "F1024-wind0", "F1024-crown0"]
    if selection:
        expected = ["F64/round-3", "F64/round-4", "F1024-vsmstats/round-1"]
    else:
        expected = ([f"{name}/round-1" for name in stages] + [f"{name}/round-2" for name in reversed(stages)]
                    + [f"{name}-vsmstats/round-1" for name in ("F1024", "F64", "F256", "F1024h", "F1024q", "F1024e")])
    expected_jobs = [] if selection else [["quick", "30m"]]
    expected_jobs.append(["quiet", f"{10 + 5 * len(expected)}m"])
    assert [json.loads(row) for row in jobs.read_text().splitlines()] == expected_jobs
    assert [row.partition("|")[0] for row in (out / "variants.txt").read_text().splitlines()] == expected
    assert measured.read_text().splitlines() == expected
    assert sorted(p.parent.relative_to(out).as_posix() for p in out.rglob("settings")) == sorted(expected)
    assert not list(out.rglob("*.png"))
    name = expected[-1]
    assert "OPENGOTHIC_VSM_STATS=1" in shlex.split((out / name / "settings").read_text())
    if not selection:
        assert shlex.split((out / "F1024-rt0/round-2/settings").read_text()) == [
            "OPENGOTHIC_GPU_PASSES=1", "OPENGOTHIC_CONTENT=~/gothic-ai/tree-cost/content/tree-cost-1024",
            "OPENGOTHIC_RT=0"]


@pytest.mark.parametrize("plan", ["", "A|FLAG=1\nA/round-1|FLAG=2", "../escape|", "A|not-an-assignment"])
def test_invalid_perf_variant_plan_is_rejected_before_upload(stand, plan):
    root = stand.checkout("invalid")
    result = subprocess.run(["bash", root / "scripts/perf.sh", "--variants", stand.tmp / "out", "market"],
                            input=plan, text=True, capture_output=True, env=stand.environment("invalid"), timeout=10)
    assert result.returncode != 0 and "perf:" in result.stderr
    assert stand.lines() == []
    assert not list(stand.home.iterdir())


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
