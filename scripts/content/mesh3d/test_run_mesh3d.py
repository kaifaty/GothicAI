"""Exercise the remote GPU window on CPU with temporary cgroups and services."""

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPT = Path(__file__).with_name("run-mesh3d.sh")
RUN_ID = "cpu-batch"
AUTHORIZATION = "one-hour-pilot"
START_EPOCH = 100000


MOCK = r'''
import json
import os
from pathlib import Path
import subprocess
import sys
import time

name = Path(sys.argv[0]).name
args = sys.argv[1:]
state_path = Path(os.environ["TEST_STATE"])
state = json.loads(state_path.read_text())
with Path(os.environ["TEST_TRACE"]).open("a") as trace:
    trace.write(json.dumps({"command": name, "args": args, "epoch": state["epoch"]}) + "\n")

def save():
    temporary = state_path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state))
    temporary.replace(state_path)

if name == "date":
    print(state["epoch"] if args == ["+%s"] else "2026-10-20T00:00:00Z")
elif name in ("flock",):
    pass
elif name == "sleep":
    time.sleep(0.01)
elif name == "setsid":
    os.execvp(args[0], args)
elif name == "timeout":
    generation_timeout = any(arg.startswith("--signal=") for arg in args)
    while args[0].startswith("--"):
        args = args[1:]
    seconds, args = int(args[0]), args[1:]
    if generation_timeout:
        os.environ["TEST_GENERATION_TIMEOUT"] = str(seconds)
    os.execvp(args[0], args)
elif name == "systemd-run":
    while args[0].startswith("-"):
        args = args[2:] if args[0] == "-p" else args[1:]
    raise SystemExit(subprocess.call(args))
elif name == "systemctl":
    action = args[1]
    unit = args[-1]
    if action == "show":
        print("/user.slice/test")
    elif action == "is-active":
        if (unit == "gothic-gemma" and unit in state["started"]
                and state["gemma_restore_failures"]):
            # Give the ready Decider worker time to record its first response.
            time.sleep(0.15)
            state["gemma_restore_failures"] -= 1
            state["epoch"] += 3
            save()
            raise SystemExit(3)
        raise SystemExit(0 if unit in state["active"] else 3)
    elif action in ("start", "stop"):
        if not unit.endswith(".scope"):
            assert unit in ("gothic-gemma", "gothic-decider"), unit
            if action == "stop":
                state["active"].remove(unit)
                state["epoch"] += {"gothic-gemma": 3, "gothic-decider": 5}[unit]
                state["free_mib"] += 8000
            else:
                state["active"].append(unit)
                state["started"].append(unit)
                state["epoch"] += {"gothic-gemma": 2, "gothic-decider": 3}[unit]
            save()
    else:
        raise SystemExit("Unexpected systemctl action: " + action)
elif name == "nvidia-smi":
    if args:
        print(state["free_mib"])
elif name == "ss":
    port = args[-1].rsplit(":", 1)[-1]
    print(f"LISTEN 0 128 127.0.0.1:{port} 0.0.0.0:*")
elif name == "curl":
    if "--write-out" in args:
        print("400", end="")
elif name == "python":
    if args[0] == "-":
        # RGBA preflight is independently covered by read_rgba CPU tests.
        sys.stdin.read()
    else:
        model = args[args.index("--model") + 1]
        duration = state["durations"][model]
        budget = int(os.environ["TEST_GENERATION_TIMEOUT"])
        state["epoch"] += min(duration, budget)
        save()
        time.sleep(0.04)  # Let the scope memory monitor sample real files.
        if duration > budget:
            raise SystemExit(124)
        raise SystemExit(state["returncodes"].get(model, 0))
else:
    raise SystemExit("Unexpected mock command: " + name)
'''


def run_window(tmp_path, *, available_gib=24, memory_max=21474836480,
               active=("gothic-gemma", "gothic-decider"), free_mib=1000,
               consumed=False, durations=None, returncodes=None, gemma_restore_failures=0):
    """Execute the production heredoc; redirect only Linux kernel file paths."""
    home = tmp_path / "home"
    task = home / "gothic-ai/mesh3d"
    (home / "gothic-ai/materials").mkdir(parents=True)
    (task / "inputs" / RUN_ID).mkdir(parents=True)
    (task / "models/dinov3").mkdir(parents=True)
    for name in ("config.json", "model.safetensors"):
        (task / "models/dinov3" / name).write_text("stub")
    (task / "downtime.tsv").write_text("old-smoke\t999999\n")
    if consumed:
        (task / "authorizations").mkdir()
        (task / "authorizations" / f"{AUTHORIZATION}.tsv").write_text("start\tprevious\n")

    cgroup = tmp_path / "cgroup"
    cgdir = cgroup / "user.slice/test"
    cgdir.mkdir(parents=True)
    for name, content in {
        "cgroup.controllers": "memory", "cgroup.subtree_control": "memory",
        "memory.max": str(memory_max), "memory.swap.max": "0", "memory.oom.group": "0",
        "memory.current": "1048576", "memory.peak": "2097152", "memory.events": "oom 0\noom_kill 0\n",
    }.items():
        (cgdir / name).write_text(content + "\n")
    proc_cgroup = tmp_path / "proc-cgroup"
    proc_cgroup.write_text("0::/user.slice/test\n")
    meminfo = tmp_path / "meminfo"
    meminfo.write_text(f"MemAvailable: {available_gib * 1024 * 1024} kB\n")

    state = tmp_path / "state.json"
    state.write_text(json.dumps({
        "epoch": START_EPOCH, "active": list(active), "free_mib": free_mib,
        "durations": durations or {"trellis": 11, "trellis2": 13},
        "returncodes": returncodes or {},
        "started": [], "gemma_restore_failures": gemma_restore_failures,
    }))
    trace = tmp_path / "trace.jsonl"
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    mock = fake_bin / "mock"
    mock.write_text(f"#!{sys.executable}\n" + MOCK)
    mock.chmod(0o755)
    for name in ("date", "flock", "sleep", "setsid", "timeout", "systemd-run",
                 "systemctl", "nvidia-smi", "ss", "curl"):
        (fake_bin / name).symlink_to(mock)
    for model in ("trellis", "trellis2"):
        python = task / f".venv-{model}/bin/python"
        python.parent.mkdir(parents=True)
        python.symlink_to(mock)

    remote = SCRIPT.read_text().split("<<'REMOTE'\n", 1)[1].rsplit("\nREMOTE", 1)[0]
    remote = remote.replace("/sys/fs/cgroup", str(cgroup))
    remote = remote.replace("/proc/self/cgroup", str(proc_cgroup))
    remote = remote.replace("/proc/meminfo", str(meminfo))
    env = dict(os.environ, HOME=str(home), PATH=f"{fake_bin}:{os.environ['PATH']}",
               TEST_STATE=str(state), TEST_TRACE=str(trace))
    result = subprocess.run(
        ["bash", "-s", "--", RUN_ID, "1", "both", "1", "3600", AUTHORIZATION],
        input=remote, env=env, capture_output=True, text=True, timeout=20,
    )
    calls = [json.loads(line) for line in trace.read_text().splitlines()]
    return result, json.loads(state.read_text()), calls, task


def service_actions(calls, action):
    return [call["args"][-1] for call in calls if call["command"] == "systemctl"
            and call["args"][1] == action and not call["args"][-1].endswith(".scope")]


def generation_calls(calls):
    return [call for call in calls if call["command"] == "python" and call["args"][0] != "-"]


def test_batch_loads_v1_then_512_once_and_accounts_only_this_authorization(tmp_path):
    result, state, calls, task = run_window(tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert set(state["active"]) == {"gothic-gemma", "gothic-decider"}
    assert service_actions(calls, "stop") == service_actions(calls, "start") == [
        "gothic-gemma", "gothic-decider"]
    invocations = generation_calls(calls)
    assert [call["args"][call["args"].index("--model") + 1] for call in invocations] == [
        "trellis", "trellis2"]
    for call in invocations:
        args = call["args"]
        assert args[args.index("--seeds") + 1:args.index("--seeds") + 3] == ["1", "2"]
        assert args[args.index("--images-dir") + 1] == str(task / "inputs" / RUN_ID)
        assert args[args.index("--output") + 1] == str(task / "runs" / RUN_ID / "batch")
        # 30 s covers timeout's 10 s kill grace plus the 15 s scope stop.
        assert int(args[args.index("--deadline") + 1]) - START_EPOCH + 10 + 15 <= 3000
    assert invocations[1]["args"][-2:] == ["--pipeline-type", "512"]
    rows = [line.split("\t") for line in
            (task / "authorizations" / f"{AUTHORIZATION}.tsv").read_text().splitlines()]
    assert rows[0] == ["start", RUN_ID, str(START_EPOCH), "3600", "3000"]
    assert sorted(row[2:] for row in rows if row[0] == "service") == sorted([
        ["gothic-gemma", "100000", "100037", "37"],
        ["gothic-decider", "100003", "100037", "34"],
    ])
    assert rows[-1] == ["window", RUN_ID, "100000", "100037", "37"]
    assert (task / "downtime.tsv").read_text() == "old-smoke\t999999\n"


@pytest.mark.parametrize("delayed_checks,expected_rc", [(10, 0), (30, 1)])
def test_restore_records_each_service_readiness_independently(tmp_path, delayed_checks, expected_rc):
    result, _, calls, task = run_window(tmp_path, gemma_restore_failures=delayed_checks)
    assert result.returncode == expected_rc, result.stdout + result.stderr
    rows = [line.split("\t") for line in
            (task / "authorizations" / f"{AUTHORIZATION}.tsv").read_text().splitlines()]
    service_rows = [row[2:] for row in rows if row[0] == "service"]
    assert len(service_rows) == 2
    assert ["gothic-decider", "100003", "100037", "34"] in service_rows
    gemma_ready = 100037 + 3 * delayed_checks
    assert ["gothic-gemma", "100000", str(gemma_ready), str(gemma_ready - START_EPOCH)] in service_rows
    decider_responses = [call for call in calls if call["command"] == "curl"
                         and call["epoch"] >= 100037 and call["args"][-1].endswith(":47832/")]
    assert len(decider_responses) == 1
    if expected_rc:
        assert "SERVICE RESTORE FAILED: gothic-gemma" in result.stderr


def test_restores_only_the_unit_actually_stopped(tmp_path):
    result, state, calls, task = run_window(tmp_path, active=("gothic-gemma",), free_mib=9000)
    assert result.returncode == 0, result.stdout + result.stderr
    assert service_actions(calls, "stop") == service_actions(calls, "start") == ["gothic-gemma"]
    assert state["active"] == ["gothic-gemma"]
    ledger = (task / "authorizations" / f"{AUTHORIZATION}.tsv").read_text()
    assert "gothic-gemma" in ledger and "gothic-decider" not in ledger


@pytest.mark.parametrize("available_gib,memory_max,expected_rc", [
    (23, 21474836480, 75), (24, 10737418240, 1),
])
def test_memory_preflight_refuses_before_stopping_services(tmp_path, available_gib, memory_max, expected_rc):
    result, state, calls, task = run_window(tmp_path, available_gib=available_gib, memory_max=memory_max)
    assert result.returncode == expected_rc, result.stdout + result.stderr
    assert not service_actions(calls, "stop")
    assert not service_actions(calls, "start")
    assert not generation_calls(calls)
    assert not (task / "authorizations" / f"{AUTHORIZATION}.tsv").exists()
    scope = next(call for call in calls if call["command"] == "systemd-run")
    assert "MemoryMax=20G" in scope["args"] and "MemorySwapMax=0" in scope["args"]


def test_consumed_authorization_refuses_before_stop(tmp_path):
    result, _, calls, task = run_window(tmp_path, consumed=True)
    assert result.returncode == 75, result.stdout + result.stderr
    assert "Authorization already consumed" in result.stderr
    assert not service_actions(calls, "stop")
    assert not generation_calls(calls)
    assert (task / "authorizations" / f"{AUTHORIZATION}.tsv").read_text() == "start\tprevious\n"


@pytest.mark.parametrize("first_rc,models", [(86, ["trellis"]), (1, ["trellis", "trellis2"])])
def test_generator_failure_restores_services_and_obeys_stop_rule(tmp_path, first_rc, models):
    result, state, calls, _ = run_window(tmp_path, returncodes={"trellis": first_rc})
    assert result.returncode == first_rc, result.stdout + result.stderr
    invocations = generation_calls(calls)
    assert [call["args"][call["args"].index("--model") + 1] for call in invocations] == models
    assert service_actions(calls, "start") == ["gothic-gemma", "gothic-decider"]
    assert set(state["active"]) == {"gothic-gemma", "gothic-decider"}


def test_deadline_stops_generation_before_fiftieth_minute_and_restores(tmp_path):
    result, _, calls, task = run_window(tmp_path, durations={"trellis": 4000, "trellis2": 13})
    assert result.returncode == 124, result.stdout + result.stderr
    assert len(generation_calls(calls)) == 1
    generation_timeouts = [call for call in calls if call["command"] == "timeout"
                           and "--signal=TERM" in call["args"]]
    assert len(generation_timeouts) == 1
    timeout_call = generation_timeouts[0]
    assert timeout_call["epoch"] - START_EPOCH + int(timeout_call["args"][2]) + 10 + 15 <= 3000
    assert service_actions(calls, "start") == ["gothic-gemma", "gothic-decider"]
    rows = [line.split("\t") for line in
            (task / "authorizations" / f"{AUTHORIZATION}.tsv").read_text().splitlines()]
    assert int(rows[-1][-1]) == 2975 < 3600
