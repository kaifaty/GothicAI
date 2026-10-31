"""Queued remote body runs execute their archive even after another sync/build."""
from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tarfile
import time

import pytest


SCRIPTS = Path(__file__).resolve().parents[1]
CASES = [
    ("body-api-remote.sh", "run", "body-api", "body_api_suite.py", "body-api-v1.json"),
    ("body-observe-remote.sh", "run", "body-headless", "body_observe_suite.py", "body-observe-v1.json"),
    ("body-headless-remote.sh", "run", "body-headless", "body_headless_suite.py", "melee-v1.json"),
    ("body-headless-remote.sh", "replay", "body-headless", "body_headless_suite.py", "melee-v1.json"),
    ("body-arena-remote.sh", "run", "body-arena", "body_arena_suite.py", "body-arena-v1.json"),
    ("body-trajectories-remote.sh", "run", "body", "body_trajectory_suite.py", "body-trajectories-v1.json"),
]
FAKE_GQ = r'''
import json
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(os.environ["FAKE_REMOTE"])
args = sys.argv[1:]
with (root / "calls.jsonl").open("a") as output:
    output.write(json.dumps(args) + "\n")
if args[0] == "submit":
    command = args[args.index("--") + 1:]
    # macOS's /usr/bin/time has no GNU -v; this test exercises the real suite
    # launch path without measuring a stub's resources.
    index = command.index("/usr/bin/time")
    assert command[index + 1:index + 3] == ["-v", "-o"]
    del command[index:index + 4]
    if "/usr/bin/xvfb-run" in command:
        index = command.index("/usr/bin/xvfb-run")
        assert command[index + 1:index + 3] == ["-a", "-s"]
        del command[index:index + 4]
    (root / "command.json").write_text(json.dumps({"command": command, "cwd": os.getcwd()}))
    if os.environ.get("FAKE_SUBMIT_BLOCK") and not (root / "submitting").exists():
        print("fake submit is waiting to publish its ID", file=sys.stderr, flush=True)
        (root / "submitting").touch()
        deadline = time.monotonic() + 10
        while not (root / "release-submit").exists():
            if time.monotonic() > deadline:
                sys.exit(97)
            time.sleep(0.01)
    print("snapshot123")
elif args[0] == "wait":
    (root / "waiting").write_text("1")
    deadline = time.monotonic() + 10
    while not (root / "release").exists():
        if (root / "cancelled").exists():
            sys.exit(73)
        if time.monotonic() > deadline:
            sys.exit(98)
        time.sleep(0.01)
    if os.environ.get("FAKE_WAIT_SKIP_COMMAND"):
        sys.exit(0)
    saved = json.loads((root / "command.json").read_text())
    # This marker belongs only to the admitted command, never the launcher.
    env = {**os.environ, "FAKE_GQ_JOB": "1"}
    sys.exit(subprocess.run(saved["command"], cwd=saved["cwd"], env=env).returncode)
elif args[0] == "cancel":
    (root / "cancelling").write_text(args[1])
    while os.environ.get("FAKE_CANCEL_BLOCK") and not (root / "release-cancel").exists():
        time.sleep(0.01)
    (root / "cancelled").write_text(args[1])
else:
    sys.exit(99)
'''
SUITE = '''import argparse
import json
from pathlib import Path
import snapshot_dependency

parser = argparse.ArgumentParser()
for name in ("output", "source", "engine-library", "config", "scenario",
             "melee-scenario", "senses-scenario", "runner", "rendered"):
    parser.add_argument("--" + name, type=Path)
args, rest = parser.parse_known_args()
result = {"suite": "v1", "dependency": snapshot_dependency.VERSION}
for name in ("engine_library", "config", "scenario", "melee_scenario", "senses_scenario", "runner", "rendered"):
    path = getattr(args, name)
    if path is not None:
        result[name] = path.read_text()
if args.scenario is None:
    result["scenario"] = (args.source.parent / "scripts/bench/body-melee.bench").read_text()
result["source"] = (args.source / "native.cpp").read_text()
args.output.mkdir(parents=True, exist_ok=True)
(args.output / "snapshot-result.json").write_text(json.dumps(result))
'''
REPORT = '''import argparse
import json
from pathlib import Path
import snapshot_dependency
parser = argparse.ArgumentParser()
parser.add_argument("output", type=Path)
parser.add_argument("--config", type=Path)
args, rest = parser.parse_known_args()
(args.output / "snapshot-report.json").write_text(json.dumps({"report": "v1", "dependency": snapshot_dependency.VERSION, "config": args.config.read_text()}))
'''


def executable(path: Path, body: str) -> None:
    path.write_text(f"#!{sys.executable}\n" + body)
    path.chmod(0o755)


@pytest.fixture
def remote(tmp_path):
    binary_dir = tmp_path / "bin"
    binary_dir.mkdir()
    home = tmp_path / "home"
    home.mkdir()
    executable(binary_dir / "ssh", 'import os, sys\nos.execlp("bash", "bash", "-c", sys.argv[-1])\n')
    executable(binary_dir / "rsync", "pass\n")
    executable(binary_dir / "df", 'print("Filesystem 1024-blocks Used Available Capacity Mounted")\nprint("fake 20000000 0 20000000 0% /")\n')
    executable(binary_dir / "pgrep", 'import os, pathlib, sys\n(pathlib.Path(os.environ["FAKE_REMOTE"]) / "pgrep-called").touch()\nsys.exit(int(os.environ.get("FAKE_PGREP_EXIT", "0")))\n')
    # Exercise the workspace lock on macOS too, where util-linux flock is absent.
    executable(binary_dir / "flock", 'import fcntl, sys\ntry:\n fcntl.flock(int(sys.argv[-1]), fcntl.LOCK_UN if "-u" in sys.argv else fcntl.LOCK_EX | fcntl.LOCK_NB)\nexcept BlockingIOError:\n sys.exit(1)\n')
    executable(binary_dir / "gq", FAKE_GQ)
    executable(binary_dir / "systemd-run", '''import json
import os
from pathlib import Path
import subprocess
import sys
args = sys.argv[1:]
root = Path(os.environ["FAKE_REMOTE"])
(root / "legacy-command.json").write_text(json.dumps(args))
cwd = next(value.split("=", 1)[1] for value in args if value.startswith("--working-directory="))
env = dict(os.environ)
for value in args:
    if value.startswith("--setenv="):
        key, setting = value[len("--setenv="):].split("=", 1)
        env[key] = setting
index = args.index("/usr/bin/time")
assert args[index + 1:index + 3] == ["-v", "-o"]
sys.exit(subprocess.run(args[index + 4:], cwd=cwd, env=env).returncode)
''')
    env = {**os.environ, "HOME": str(home), "GQ_BIN": str(binary_dir / "gq"),
           "GQ_OWNER": "snapshot-test", "FAKE_REMOTE": str(tmp_path),
           # Match Linux tar: macOS otherwise inserts AppleDouble metadata
           # pseudo-files that its own extractor restores as extended attrs.
           "COPYFILE_DISABLE": "1",
           "PATH": str(binary_dir) + os.pathsep + os.environ["PATH"]}
    env.pop("GQ_JOB_ID_FILE", None)
    return {"root": tmp_path, "home": home, "env": env}


@pytest.mark.parametrize("script,args,output", [
    ("physics-e2e-remote.sh", ["run", "bullet", "missing-converter"], "runs"),
    ("physics-e2e-front-diagnostic.sh", ["missing-converter"], "diagnostics"),
])
def test_physics_requires_converter_before_run_setup(remote, script, args, output):
    binary = remote["home"] / "gothic-ai/physics-stage1/bullet/reference/opengothic/Gothic2Notr"
    binary.parent.mkdir(parents=True)
    executable(binary, "raise SystemExit('unexpected game launch')\n")
    executable(remote["root"] / "bin/flock", "raise SystemExit(97)\n")
    result = subprocess.run(["bash", str(SCRIPTS / script), *args],
                            env=remote["env"], stdin=subprocess.DEVNULL,
                            text=True, capture_output=True, timeout=5)
    assert result.returncode == 1
    assert f"Missing converter: {binary.with_name('content-convert')}" in result.stderr
    assert "Rebuild the engine" in result.stderr
    assert not (remote["home"] / "gothic-ai/physics-stage1/e2e" / output / "missing-converter").exists()
    assert not (remote["home"] / "gothic-ai/body-simulation.lock").exists()


def prepare(remote, task_name, suite):
    if task_name in ("body-arena", "body"):
        prepare(remote, "body-api", "body_api_suite.py")
    task = remote["home"] / "gothic-ai" / task_name
    source = task / "source"
    for directory in ("OpenGothic", "scripts/bench", "evals/npc-body"):
        (source / directory).mkdir(parents=True)
    (source / "scripts" / suite).write_text(SUITE)
    (source / "scripts/snapshot_dependency.py").write_text('VERSION = "v1"\n')
    shutil.copy2(SCRIPTS / "base-ensure.sh", source / "scripts/base-ensure.sh")
    (source / "scripts/body_trajectory_report.py").write_text(REPORT)
    (source / "OpenGothic/native.cpp").write_text("v1")
    for name in ("body-api.bench", "body-observe.bench", "body-melee.bench", "body-headless-senses.bench", "body-arena.bench"):
        (source / "scripts/bench" / name).write_text("v1")
    for name in ("body-api-v1.json", "body-observe-v1.json", "melee-v1.json", "body-arena-v1.json", "body-trajectories-v1.json"):
        (source / "evals/npc-body" / name).write_text("v1")
    provenance = task / "provenance"
    provenance.mkdir()
    for name in ("source-head.txt", "source.patch", "configure.log", "build.log", "resolver-tests.json"):
        (provenance / name).write_text("v1")
    build = task / "build"
    (build / "opengothic").mkdir(parents=True)
    for name in ("gothic-body-runner", "Gothic2Notr", "gothic-body-resolver-test"):
        path = build / "opengothic" / name
        path.write_text("v1")
        path.chmod(0o755)
    executable(build / "opengothic/content-convert", '''import json
import os
from pathlib import Path
import sys
(Path(os.environ["FAKE_REMOTE"]) / "base-ensure-call.json").write_text(json.dumps({
    "version": "v1", "args": sys.argv[1:], "queued": os.environ.get("FAKE_GQ_JOB")}))
''')
    (build / "libOpenGothic.a").write_text("v1")
    return task


def engine_task(task):
    return task.parent / "body-api" if task.name in ("body-arena", "body") else task


def run_output(task, name):
    return task / ("data" if task.name == "body" else "runs") / name


def wait_for_queue(process, remote):
    deadline = time.monotonic() + 5
    while not (remote["root"] / "waiting").exists():
        assert process.poll() is None, process.communicate(timeout=1)
        assert time.monotonic() < deadline, "launcher did not reach fake queue"
        time.sleep(0.01)


@pytest.mark.parametrize("script,action,task_name,suite,config", CASES)
def test_queued_body_suite_runs_archived_snapshot_after_sync(remote, script, action, task_name, suite, config):
    task = prepare(remote, task_name, suite)
    process = subprocess.Popen(["bash", str(SCRIPTS / script), action, "snapshot-test"],
                               env=remote["env"], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        wait_for_queue(process, remote)
        out = run_output(task, "snapshot-test")
        with tarfile.open(out / "source.tar.gz") as archive:
            assert archive.extractfile("scripts/" + suite).read().decode() == SUITE
            assert archive.extractfile("evals/npc-body/" + config).read() == b"v1"
        # The real sync heredoc must permit another run in this workspace;
        # executing this queued run is safe because its inputs are frozen.
        sync = subprocess.run(["bash", str(SCRIPTS / script), "sync"], env=remote["env"],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
        assert sync.returncode == 0, sync.stderr
        assert not (remote["root"] / "pgrep-called").exists()
        # Snapshotting must release the workspace lock before entering wait.
        with (engine_task(task) / "build.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for workspace in {task, engine_task(task)}:
            for path in (workspace / "source").rglob("*"):
                if path.is_file():
                    if path.name == suite:
                        path.write_text(SUITE.replace('"suite": "v1"', '"suite": "v2"'))
                    elif path.name == "snapshot_dependency.py":
                        path.write_text('VERSION = "v2"\n')
                    elif path.name == "body_trajectory_report.py":
                        path.write_text(REPORT.replace('"report": "v1"', '"report": "v2"'))
                    else:
                        path.write_text("v2")
        # A subsequent build must not change the queued run's native inputs.
        for path in (engine_task(task) / "build").rglob("*"):
            if path.is_file():
                path.write_text("v2")
        (remote["root"] / "release").write_text("1")
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stdout + stderr + (out / "launcher.log").read_text()
        result = json.loads((out / "snapshot-result.json").read_text())
        assert result and set(result.values()) == {"v1"}
        if task_name == "body":
            assert set(json.loads((out / "snapshot-report.json").read_text()).values()) == {"v1"}
        with tarfile.open(out / "source.tar.gz") as archive:
            for member in archive.getmembers():
                if member.isfile():
                    assert archive.extractfile(member).read() == (out / "reference/source" / member.name).read_bytes()
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


@pytest.mark.parametrize("script,action,task_name,suite,config", CASES)
def test_legacy_body_launch_keeps_shared_paths_and_unit_properties(remote, script, action, task_name, suite, config):
    task = prepare(remote, task_name, suite)
    env = {**remote["env"], "GQ_BIN": str(remote["root"] / "absent-gq"), "FAKE_PGREP_EXIT": "1"}
    result = subprocess.run(["bash", str(SCRIPTS / script), action, "legacy-test"],
                            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
    args = json.loads((remote["root"] / "legacy-command.json").read_text())
    assert str(task / "source/scripts" / suite) in args
    assert args[args.index("--source") + 1] == str(engine_task(task) / "source/OpenGothic")
    assert args[args.index("--config") + 1] == str(task / "source/evals/npc-body" / config)
    assert args[args.index("--engine-library") + 1] == str(engine_task(task) / "build/libOpenGothic.a")
    assert "--property=CPUQuota=200%" in args
    assert "--property=MemoryMax=12G" in args
    assert (remote["root"] / "pgrep-called").exists()
    assert not (engine_task(task) / "build.lock").exists()
    assert (remote["home"] / "gothic-ai/body-simulation.lock").exists()
    assert not (run_output(task, "legacy-test") / "reference/source").exists()


def test_body_snapshot_refuses_concurrent_workspace_build(remote):
    task = prepare(remote, "body-api", "body_api_suite.py")
    with (task / "build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = subprocess.run(["bash", str(SCRIPTS / "body-api-remote.sh"), "run", "blocked-build"],
                                env=remote["env"], stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, result.stdout + result.stderr
    assert "Another build owns workspace" in result.stderr
    assert not (remote["root"] / "calls.jsonl").exists()
    assert not (task / "runs/blocked-build").exists()


@pytest.mark.parametrize("queued,action,collector_code,reporter_code,expected,stages", [
    (True, "run", 0, 0, 0, ["collector", "reporter"]),
    (True, "run", 31, 0, 31, ["collector"]),
    (True, "run", 0, 32, 32, ["collector", "reporter"]),
    (True, "smoke", 0, 32, 0, ["collector"]),
    (False, "run", 0, 0, 0, ["collector", "reporter"]),
    (False, "run", 31, 0, 31, ["collector"]),
    (False, "run", 0, 32, 2, ["collector", "reporter"]),
])
def test_trajectory_collection_and_report_share_queue_job(
        remote, queued, action, collector_code, reporter_code, expected, stages):
    task = prepare(remote, "body", "body_trajectory_suite.py")
    scripts = task / "source/scripts"
    (scripts / "body_trajectory_suite.py").write_text('''import argparse
import json
import os
from pathlib import Path
import sys
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
args, rest = parser.parse_known_args()
with (args.output / "stages.jsonl").open("a") as log:
    log.write(json.dumps({"stage": "collector", "in_job": os.environ.get("FAKE_GQ_JOB") == "1"}) + "\\n")
sys.exit(int(os.environ["COLLECTOR_CODE"]))
''')
    (scripts / "body_trajectory_report.py").write_text('''import json
import os
from pathlib import Path
import sys
out = Path(sys.argv[1])
with (out / "stages.jsonl").open("a") as log:
    log.write(json.dumps({"stage": "reporter", "in_job": os.environ.get("FAKE_GQ_JOB") == "1"}) + "\\n")
sys.exit(int(os.environ["REPORTER_CODE"]))
''')
    env = {**remote["env"], "COLLECTOR_CODE": str(collector_code),
           "REPORTER_CODE": str(reporter_code)}
    if not queued:
        env.update(GQ_BIN=str(remote["root"] / "absent-gq"), FAKE_PGREP_EXIT="1")
    (remote["root"] / "release").touch()
    result = subprocess.run(["bash", str(SCRIPTS / "body-trajectories-remote.sh"), action, "stages-test"],
                            env=env, stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=5)
    out = task / "data/stages-test"
    assert result.returncode == expected, result.stdout + result.stderr + (out / "launcher.log").read_text()
    recorded = [json.loads(line) for line in (out / "stages.jsonl").read_text().splitlines()]
    assert recorded == [{"stage": stage, "in_job": queued} for stage in stages]
    if queued:
        calls = [json.loads(line) for line in (remote["root"] / "calls.jsonl").read_text().splitlines()]
        assert [call[0] for call in calls] == ["submit", "wait"]
        assert calls[0][calls[0].index("--class") + 1] == "batch"
    else:
        assert not (remote["root"] / "calls.jsonl").exists()


def test_queued_baseline_keeps_archived_scenario_binary_and_report(remote):
    import hashlib

    task = prepare(remote, "body-baseline", "body_baseline_report.py")
    source = task / "source"
    reporter = '''import json
from pathlib import Path
import sys
import snapshot_dependency
out = Path(sys.argv[1])
root = Path(__file__).resolve().parents[1]
(out / "report-result.json").write_text(json.dumps({
    "report": "v1", "dependency": snapshot_dependency.VERSION,
    "config": (root / "evals/npc-body/melee-v1.json").read_text()}))
'''
    (source / "scripts/body_baseline_report.py").write_text(reporter)
    binary = task / "build/opengothic/Gothic2Notr"
    executable(binary, '''import json
import os
from pathlib import Path
out = Path(os.environ["OPENGOTHIC_BENCH_OUT"])
(out / "baseline-result.json").write_text(json.dumps({
    "binary": "v1", "scenario": Path(os.environ["OPENGOTHIC_BENCH"]).read_text()}))
(out / "bench.log").write_text("done\\n")
Path("log.txt").write_text("test runner\\n")
''')
    (task / "build/CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n")
    (task / "assets").mkdir()
    (task / "assets/Data").mkdir()
    (task / "assets/_work").mkdir()
    (task / "reference").mkdir()
    for name in ("save_slot_1.sav", "source-head.txt", "source.patch"):
        (task / "reference" / name).write_text("v1")
    remote["env"].update(GOTHIC_DIR=str(task / "assets"),
                         BASELINE_SAVE=str(task / "reference/save_slot_1.sav"))
    toolchain = task / "toolchain/vulkan-1.4.341.0"
    toolchain.mkdir(parents=True)
    (toolchain / "manifest.json").write_text("{}")
    binary_dir = Path(remote["env"]["GQ_BIN"]).parent
    executable(binary_dir / "nvidia-smi", 'print("fake GPU")\n')
    executable(binary_dir / "systemctl", 'print("ActiveState=inactive")\n')
    process = subprocess.Popen(["bash", str(SCRIPTS / "body-baseline-remote.sh"), "run", "snapshot-test"],
                               env=remote["env"], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        wait_for_queue(process, remote)
        out = task / "runs/snapshot-test"
        sync = subprocess.run(["bash", str(SCRIPTS / "body-baseline-remote.sh"), "sync"],
                              env=remote["env"], stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=5)
        assert sync.returncode == 0, sync.stderr
        with (task / "build.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for path in source.rglob("*"):
            if path.is_file():
                path.write_text("v2")
        binary.write_text("v2")
        (remote["root"] / "release").write_text("1")
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stdout + stderr + (out / "launcher.log").read_text()
        assert set(json.loads((out / "baseline-result.json").read_text()).values()) == {"v1"}
        assert json.loads((remote["root"] / "base-ensure-call.json").read_text()) == {
            "version": "v1", "args": ["resolve", str(task / "assets")], "queued": "1"}
        assert set(json.loads((out / "report-result.json").read_text()).values()) == {"v1"}
        manifest = json.loads((out / "manifest.json").read_text())
        assert manifest["scenario_sha256"] == manifest["config_sha256"] == hashlib.sha256(b"v1").hexdigest()
        assert manifest["binary_sha256"] == hashlib.sha256((out / "reference/Gothic2Notr").read_bytes()).hexdigest()
        with tarfile.open(out / "source.tar.gz") as archive:
            assert archive.extractfile("scripts/body_baseline_report.py").read().decode() == reporter
            for member in archive.getmembers():
                if member.isfile():
                    assert archive.extractfile(member).read() == (out / "reference/source" / member.name).read_bytes()
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


def prepare_bc_run(remote):
    task = prepare(remote, "body", "body_setup_probe.py")
    source = task / "source"
    (source / "body-service/src").mkdir(parents=True)
    (source / "evals/npc-body/body-bc-v1.json").write_text(json.dumps({
        "resources": {"required_services": []}}))
    (source / "evals/npc-body/body-stage8-v1.json").write_text(json.dumps({"runtime": {}}))
    out = task / "data/bc-snapshot-test"
    shutil.copytree(source, out / "code")
    (out / "reference").mkdir()
    runner = out / "reference/gothic-body-runner"
    runner.write_text("v1")
    runner.chmod(0o755)
    (out / "environment.json").write_text("{}")
    with tarfile.open(out / "source.tar.gz", "w:gz") as archive:
        archive.add(engine_task(task) / "source/OpenGothic", arcname="OpenGothic")
    (task / ".venv/bin").mkdir(parents=True)
    (task / ".venv/bin/python").symlink_to(sys.executable)
    binary_dir = Path(remote["env"]["GQ_BIN"]).parent
    executable(binary_dir / "nvidia-smi", 'print("fake GPU")\n')
    return task, out


@pytest.mark.parametrize("existing_snapshot", [False, True])
def test_queued_bc_phase_keeps_frozen_native_inputs(remote, existing_snapshot):
    task, out = prepare_bc_run(remote)
    engine = engine_task(task)
    if existing_snapshot:
        shutil.copytree(engine / "source/OpenGothic", out / "reference/source/OpenGothic")
        shutil.copyfile(engine / "build/libOpenGothic.a", out / "reference/libOpenGothic.a")
    # Native source belongs to the prepared archive, including older runs that
    # do not yet have an extracted reference. Existing snapshots stay intact.
    (engine / "source/OpenGothic/native.cpp").write_text("v2")
    if existing_snapshot:
        (engine / "build/libOpenGothic.a").write_text("v2")
    process = subprocess.Popen(["bash", str(SCRIPTS / "body-bc-remote.sh"), "setup", out.name],
                               env=remote["env"], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        wait_for_queue(process, remote)
        with (engine / "build.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        command = json.loads((remote["root"] / "command.json").read_text())["command"]
        assert command[command.index("--source") + 1] == str(out / "reference/source/OpenGothic")
        assert command[command.index("--engine-library") + 1] == str(out / "reference/libOpenGothic.a")
        (engine / "source/OpenGothic/native.cpp").write_text("v3")
        (engine / "build/libOpenGothic.a").write_text("v3")
        (remote["root"] / "release").touch()
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stdout + stderr + (out / "launcher-setup.log").read_text()
        result = json.loads((out / "setup-probe/snapshot-result.json").read_text())
        assert {key: result[key] for key in ("suite", "dependency", "source", "engine_library", "runner")} == {
            key: "v1" for key in ("suite", "dependency", "source", "engine_library", "runner")}
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


def test_legacy_bc_phase_keeps_shared_native_paths(remote):
    task, out = prepare_bc_run(remote)
    env = {**remote["env"], "GQ_BIN": str(remote["root"] / "absent-gq"), "FAKE_PGREP_EXIT": "1"}
    result = subprocess.run(["bash", str(SCRIPTS / "body-bc-remote.sh"), "setup", out.name],
                            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
    args = json.loads((remote["root"] / "legacy-command.json").read_text())
    assert args[args.index("--source") + 1] == str(engine_task(task) / "source/OpenGothic")
    assert args[args.index("--engine-library") + 1] == str(engine_task(task) / "build/libOpenGothic.a")
    assert not (out / "reference/source").exists()
    assert not (out / "reference/libOpenGothic.a").exists()


@pytest.mark.parametrize("id_published", [False, True], ids=["submit-pending", "wait-pending"])
def test_queued_bc_phase_rejects_duplicate_without_changing_evidence(remote, id_published):
    task, out = prepare_bc_run(remote)
    env = {**remote["env"], "FAKE_WAIT_SKIP_COMMAND": "1"}
    if not id_published:
        env["FAKE_SUBMIT_BLOCK"] = "1"
    command = ["bash", str(SCRIPTS / "body-bc-remote.sh"), "tests", out.name]
    process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        if id_published:
            wait_for_queue(process, remote)
            assert (out / "gq-tests.id").read_text().strip() == "snapshot123"
        else:
            deadline = time.monotonic() + 5
            while not (remote["root"] / "submitting").exists():
                assert process.poll() is None, process.communicate(timeout=1)
                assert time.monotonic() < deadline, "submit did not reach the ID barrier"
                time.sleep(0.01)
            assert not (out / "gq-tests.id").exists()
            # A broken second launcher can finish without executing pytest;
            # the first submit remains paused until its own barrier opens.
            (remote["root"] / "release").touch()
        evidence = {name: (out / name).read_bytes() for name in (
            "launcher-tests.log", "services-tests-before.json")}
        duplicate = subprocess.run(command, env=env, stdin=subprocess.DEVNULL,
                                   capture_output=True, text=True, timeout=5)
        assert duplicate.returncode == 1, duplicate.stdout + duplicate.stderr
        assert "Another launcher owns this run/phase" in duplicate.stderr
        calls = [json.loads(line)[0] for line in (remote["root"] / "calls.jsonl").read_text().splitlines()]
        assert calls.count("submit") == 1
        for name, original in evidence.items():
            assert (out / name).read_bytes() == original, name
        (remote["root"] / "release-submit").touch()
        (remote["root"] / "release").touch()
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stdout + stderr + (out / "launcher-tests.log").read_text()
        calls = [json.loads(line)[0] for line in (remote["root"] / "calls.jsonl").read_text().splitlines()]
        assert calls == ["submit", "wait"]
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


def test_queued_bc_phase_existing_id_preserves_evidence_before_submit(remote):
    task, out = prepare_bc_run(remote)
    (out / "gq-tests.id").write_text("previous-job\n")
    evidence = {"launcher-tests.log": b"previous launcher output\n",
                "services-tests-before.json": b'{"previous": true}\n',
                "services-tests-after.json": b'{"previous": true}\n',
                "resources-tests.txt": b"previous resources\n",
                "tests.xml": b"<previous-tests/>\n"}
    for name, content in evidence.items():
        (out / name).write_bytes(content)
    result = subprocess.run(["bash", str(SCRIPTS / "body-bc-remote.sh"), "tests", out.name],
                            env=remote["env"], stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 1, result.stdout + result.stderr
    assert "Queue id evidence exists" in result.stderr
    assert not (remote["root"] / "calls.jsonl").exists()
    assert (out / "gq-tests.id").read_text() == "previous-job\n"
    for name, original in evidence.items():
        assert (out / name).read_bytes() == original, name


@pytest.mark.parametrize("first,second", [
    ("train-cuda", "defence-train-cuda"),
    ("defence-train-cuda", "train-cuda"),
])
def test_queued_bc_phase_aliases_share_reservation_and_id(remote, first, second):
    task, out = prepare_bc_run(remote)
    # The real GPU monitor is unrelated to reserving a phase. Keep its lifetime
    # tied to job-done without starting CUDA or querying a real GPU.
    (task / "source/scripts/body_training_gpu_watch.py").write_text('''import argparse
from pathlib import Path
import time
parser = argparse.ArgumentParser()
parser.add_argument("--job-done", type=Path)
args, rest = parser.parse_known_args()
deadline = time.monotonic() + 10
while not args.job_done.exists():
    assert time.monotonic() < deadline, "launcher did not finish its job"
    time.sleep(0.01)
''')
    env = {**remote["env"], "FAKE_SUBMIT_BLOCK": "1", "FAKE_WAIT_SKIP_COMMAND": "1"}
    launcher = ["bash", str(SCRIPTS / "body-bc-remote.sh")]
    process = subprocess.Popen([*launcher, first, out.name], env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        deadline = time.monotonic() + 5
        while not (remote["root"] / "submitting").exists():
            assert process.poll() is None, process.communicate(timeout=1)
            assert time.monotonic() < deadline, "submit did not reach the ID barrier"
            time.sleep(0.01)
        assert (out / "gq-defence-train-cuda.lock").exists()
        assert not (out / "gq-train-cuda.lock").exists()
        assert not (out / "gq-defence-train-cuda.id").exists()
        saved = json.loads((remote["root"] / "command.json").read_text())["command"]
        assert saved[-4:] == [str(out / "code/scripts/body_defence_suite.py"),
                              "train-cuda", "--run", str(out)]
        duplicate = subprocess.run([*launcher, second, out.name], env=env,
                                   stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
        assert duplicate.returncode == 1, duplicate.stdout + duplicate.stderr
        assert "Another launcher owns this run/phase" in duplicate.stderr
        (remote["root"] / "release-submit").touch()
        (remote["root"] / "release").touch()
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stdout + stderr + (out / f"launcher-{first}.log").read_text()
        assert (out / "gq-defence-train-cuda.id").read_text() == "snapshot123\n"
        assert not (out / "gq-train-cuda.id").exists()
        completed = subprocess.run([*launcher, second, out.name], env=env,
                                   stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
        assert completed.returncode == 1, completed.stdout + completed.stderr
        assert "Queue id evidence exists" in completed.stderr
        calls = [json.loads(line)[0] for line in (remote["root"] / "calls.jsonl").read_text().splitlines()]
        assert calls == ["submit", "wait"]
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


def test_bc_phase_repeated_interrupt_during_cancel_cancels_job(remote):
    task, out = prepare_bc_run(remote)
    env = {**remote["env"], "FAKE_CANCEL_BLOCK": "1"}
    launcher = (SCRIPTS / "body-bc-remote.sh").read_text()
    # Execute exactly the remote run_phase body. An SSH client otherwise adds
    # a second process group whose own terminal handling is outside this test.
    freezer = launcher.split("freeze_engine_reference() {", 1)[1].split("\nremote_script() {", 1)[0]
    body = launcher.split("run_phase() {", 1)[1].split("<<'REMOTE_SCRIPT'\n", 1)[1].split("\nREMOTE_SCRIPT", 1)[0]
    payload = (SCRIPTS / "gq-job.sh").read_text() + "\nfreeze_engine_reference() {" + freezer + "\n" + body
    process = subprocess.Popen(["bash", "-s", "--", "gothic-ai/body", "gothic-ai/body-api",
                                out.name, "setup", "", "0"], env=env,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    process.stdin.write(payload)
    process.stdin.close()
    process.stdin = None
    try:
        wait_for_queue(process, remote)
        os.killpg(process.pid, signal.SIGINT)
        deadline = time.monotonic() + 5
        while not (remote["root"] / "cancelling").exists():
            assert process.poll() is None, process.communicate(timeout=1)
            assert time.monotonic() < deadline, "cancel did not start"
            time.sleep(0.01)
        os.killpg(process.pid, signal.SIGINT)
        time.sleep(0.05)
        assert process.poll() is None
        assert not (remote["root"] / "cancelled").exists()
        (remote["root"] / "release-cancel").touch()
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 73, stdout + stderr + (out / "launcher-setup.log").read_text()
        assert (remote["root"] / "cancelled").read_text() == "snapshot123"
        calls = [json.loads(line)[0] for line in (remote["root"] / "calls.jsonl").read_text().splitlines()]
        assert calls == ["submit", "wait", "cancel"]
        assert "could not cancel" not in (out / "launcher-setup.log").read_text()
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
