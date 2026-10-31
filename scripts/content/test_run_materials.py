"""Exercise the GPU window and listener checks without SSH, models or services."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


SCRIPT = Path(__file__).with_name("run-materials.sh")


def run_mock(tmp_path, *args, free=1000, missing_decider=False):
    repo = tmp_path / "repo"
    script = repo / "scripts/content/run-materials.sh"
    script.parent.mkdir(parents=True)
    shutil.copy2(SCRIPT, script)
    home = tmp_path / "home"
    task = home / "gothic-ai/materials"
    (task / ".venv/bin").mkdir(parents=True)
    state = tmp_path / "state.json"
    state.write_text(json.dumps({"stopped": [], "events": []}))
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    mock = fake_bin / "mock"
    mock.write_text(f"#!{sys.executable}\n" + '''
import json
import os
from pathlib import Path
import subprocess
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
state_path = Path(os.environ['TEST_STATE'])
state = json.loads(state_path.read_text())
def event(kind, value):
    state['events'].append([kind, value])
    state_path.write_text(json.dumps(state))
if name == 'pgrep':
    raise SystemExit(1)
elif name in ('scp', 'flock', 'sleep'):
    pass
elif name == 'du':
    print('1024\\t.')
elif name == 'ssh':
    raise SystemExit(subprocess.call(args[2:]))
elif name == 'timeout':
    if args[0] == '-k': args = args[2:]
    os.execvp(args[1], args[1:])
elif name == 'nvidia-smi':
    if args:
        print(int(os.environ['TEST_FREE']) + 7500 * len(state['stopped']))
elif name == 'systemctl':
    action = args[1]
    unit = args[-1]
    if action == 'stop':
        state['stopped'].append(unit)
        event('stop', unit)
    elif action == 'start':
        state['stopped'].remove(unit)
        event('start', unit)
    elif action == 'is-active':
        if unit in state['stopped']: raise SystemExit(3)
        if '--quiet' not in args: print('active')
    else: raise SystemExit(93)
elif name == 'ss':
    port = args[-1].rsplit(':', 1)[-1]
    if port == '47832' and os.environ['TEST_MISSING_DECIDER'] == '1':
        raise SystemExit(0)
    bind = '192.0.2.55' if port == '47832' else '0.0.0.0'
    print(f'LISTEN 0 128 {bind}:{port} 0.0.0.0:*')
elif name == 'curl':
    url = args[-1]
    event('curl', url)
    if ':47832/' in url and url != 'http://192.0.2.55:47832/':
        raise SystemExit(7)
    if '--write-out' in args: print('400', end='')
elif name == 'python':
    event('python', {'args': args, 'implicit': os.environ.get('HF_HUB_DISABLE_IMPLICIT_TOKEN')})
else:
    raise SystemExit(f'unexpected mock command: {name}')
''')
    mock.chmod(0o755)
    for name in ("pgrep", "scp", "flock", "sleep", "du", "ssh", "timeout", "nvidia-smi", "systemctl", "ss", "curl"):
        (fake_bin / name).symlink_to(mock)
    (task / ".venv/bin/python").symlink_to(mock)
    env = dict(os.environ, HOME=str(home), PATH=f"{fake_bin}:{os.environ['PATH']}",
               TEST_STATE=str(state), TEST_FREE=str(free),
               TEST_MISSING_DECIDER=str(int(missing_decider)))
    env.pop("GPU_MIN_FREE_MIB", None)
    result = subprocess.run(["bash", str(script), *args], env=env, capture_output=True, text=True, timeout=10)
    return result, json.loads(state.read_text()), task


def test_flux_checks_actual_listener_before_stop_and_after_restore(tmp_path):
    result, state, task = run_mock(tmp_path, "flux")
    assert result.returncode == 0, result.stdout + result.stderr
    events = state["events"]
    stops = [value for kind, value in events if kind == "stop"]
    assert stops == ["gothic-gemma", "gothic-decider"]
    for unit, url in (("gothic-gemma", "http://127.0.0.1:47834/v1/models"),
                      ("gothic-decider", "http://192.0.2.55:47832/")):
        assert events.index(["curl", url]) < events.index(["stop", unit])
        assert events[events.index(["start", unit]) + 1:].count(["curl", url]) == 1
    assert state["stopped"] == []
    invocation = next(value for kind, value in events if kind == "python")
    assert invocation["implicit"] == "1"
    assert invocation["args"] == ["-u", "bakeoff.py", "flux", "--registry", "registry.jsonl", "--root", str(task)]


def test_unresolved_decider_listener_prevents_stop_and_restores_gemma(tmp_path):
    result, state, _ = run_mock(tmp_path, "flux", missing_decider=True)
    assert result.returncode == 1
    assert "Cannot identify listener for gothic-decider" in result.stderr
    assert [value for kind, value in state["events"] if kind == "stop"] == ["gothic-gemma"]
    assert state["stopped"] == []
    assert not any(kind == "python" for kind, _ in state["events"])


def test_flux_maps_pass_base_without_stopping_services(tmp_path):
    result, state, task = run_mock(tmp_path, "normals", "--base", "flux", free=22000)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not any(kind == "stop" for kind, _ in state["events"])
    invocation = next(value for kind, value in state["events"] if kind == "python")
    assert invocation["implicit"] == "1"
    assert invocation["args"] == ["-u", "bakeoff.py", "normals", "--registry", "registry.jsonl", "--root", str(task), "--base", "flux"]


def test_city2_uses_its_own_registry_and_log_with_the_shared_downtime_budget(tmp_path):
    result, state, task = run_mock(tmp_path, "city", "--set", "city-2", free=22000)
    assert result.returncode == 0, result.stdout + result.stderr
    invocations = [value for kind, value in state["events"] if kind == "python"]
    assert invocations[0]["args"] == ["generate.py", "prepare", "--registry", "city-2.jsonl",
        "--mapping", "city-materials.csv", "--bible", "content-style.md", "--set", "city-2"]
    assert invocations[1]["args"] == ["-u", "generate.py", "run", "--registry", "city-2.jsonl",
        "--root", str(task), "--set", "city-2"]
    assert (task/"city-2.log").is_file()
    assert not (task/"city-1.log").exists()
