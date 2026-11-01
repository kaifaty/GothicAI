"""Exercise the live runner without starting a game or spending model quota."""

import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "e2e-live.sh"


def runner(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (tmp_path / "server").mkdir()
    script = scripts / "e2e-live.sh"
    script.write_text(SCRIPT.read_text())
    bins = tmp_path / "bin"
    bins.mkdir()
    # uv run python -c ... uses this interpreter and the actual source under test.
    (bins / "uv").write_text(f'#!/bin/bash\nshift 2\nexec {shlex.quote(sys.executable)} "$@"\n')
    (bins / "pgrep").write_text("#!/bin/sh\nexit 1\n")
    for binary in bins.iterdir():
        binary.chmod(0o755)
    env = dict(
        os.environ,
        PATH=f"{bins}:{os.environ['PATH']}",
        PYTHONPATH=str(SCRIPT.parents[1] / "server/src"),
        SUITE="haggle",
        RUNS="2",
        LLM="chatgpt",
        OUT=str(tmp_path / "out"),
    )
    env.pop("CASES", None)
    return script, scripts, env


def test_two_fake_runs_are_summarized_and_used_for_quota_estimate(tmp_path):
    script, scripts, env = runner(tmp_path)
    fake = scripts / "e2e.sh"
    fake.write_text(f"""#!/bin/bash
{shlex.quote(sys.executable)} - <<'INNER'
import json
import os
from pathlib import Path
root = Path(os.environ["LOG_DIR"])
index = int(root.name.split("-")[-1])
(root / "traces").mkdir()
trace = root / "traces/turns.jsonl"
trace.write_text("transcript")
(root / "server.log").write_text("server output")
(root / "live.json").write_text(json.dumps({{"suite": "haggle", "cases": [{{
    "name": "greedy_price", "expected": "Canthar offers less than Lutero for the same silver cup",
    "outcome": "success" if index == 1 else "failure", "evidence": {{"prices": [10, 20]}},
    "requests": index * 2, "transcripts": [{{"path": str(trace), "turns": ["turn"]}}]
}}]}}))
INNER
""")
    fake.chmod(0o755)
    done = subprocess.run(["bash", str(script)], env=env, capture_output=True, text=True, timeout=20)
    assert done.returncode == 0, done.stderr + done.stdout
    report = json.loads((tmp_path / "out/report.json").read_text())
    [case] = report["cases"]
    assert (case["successes"], case["attempts"], case["success_rate"], case["failures"]) == (1, 2, 0.5, 1)
    assert (case["requests"], case["mean_requests"], report["requests"]) == (6, 3, 6)
    assert len(case["transcripts"]) == 2
    markdown = (tmp_path / "out/report.md").read_text()
    assert "1/2" in markdown and "50.0%" in markdown and "6 / 3.00" in markdown
    assert str(tmp_path / "out/run-2/traces/turns.jsonl") in markdown
    assert "12.0" in done.stdout  # declared estimate, 6 per case × 2 runs
    # A failed restart must never report stale outcomes from an earlier invocation.
    repeated = subprocess.run(
        ["bash", str(script)], env=env, capture_output=True, text=True, timeout=5
    )
    assert repeated.returncode == 2
    assert "choose an empty directory" in repeated.stderr
    assert json.loads((tmp_path / "out/report.json").read_text()) == report
    history = tmp_path / ".live/old"
    history.mkdir(parents=True)
    (history / "report.json").write_text(json.dumps(report))
    done = subprocess.run(
        ["bash", str(script)],
        env={**env, "OUT": str(tmp_path / "again")},
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert done.returncode == 0, done.stderr + done.stdout
    assert "= 6.0" in done.stdout  # measured mean of 3 replaces the declared 6


def test_non_live_llm_is_rejected_before_starting_a_run(tmp_path):
    script, _, env = runner(tmp_path)
    for llm in ("", "off", "fake"):
        done = subprocess.run(
            ["bash", str(script)], env={**env, "LLM": llm}, capture_output=True, text=True, timeout=5
        )
        assert done.returncode == 2
        assert "requires a live LLM" in done.stderr
        assert not (tmp_path / "out").exists()


def regression_runner(tmp_path):
    _, scripts, env = runner(tmp_path)
    script = scripts / "e2e-regression.sh"
    script.write_text((SCRIPT.parent / script.name).read_text())
    docs = tmp_path / "docs"
    docs.mkdir()
    (docs / "e2e.md").write_text(
        "## Известные дефекты\n\n| Сценарий | Дефект main |\n|---|---|\n| `lie` | Known defect |\n"
    )
    save = tmp_path / "slot1.sav"
    save.write_text("save")
    env.update(HOST="mac", GAME_BIN="/game", SAVE=str(save), TIMEOUT="1", RYZEN_WORKTREE="test")
    stub = f"""#!{sys.executable}
import fcntl
import json
import os
from pathlib import Path
import time
out = Path(os.environ["LOG_DIR"])
root = out.parent
name = os.environ["SCENARIO"]
with (root / "lock").open("w") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    state = json.loads((root / "active.json").read_text()) if (root / "active.json").exists() else {{"active": 0, "peak": 0}}
    state["active"] += 1
    state["peak"] = max(state["peak"], state["active"])
    (root / "active.json").write_text(json.dumps(state))
time.sleep(0.3)
(out / "env.json").write_text(json.dumps(dict(os.environ)))
if os.environ.get("RETRY") == name:
    (out / "server.log").write_text(f"scenario {{name}}: model_off: failed interrupted\\n")
    out = out / "run-2"
    out.mkdir()
failed = int(os.environ.get("FAIL_CHECK") == name)
(out / "server.log").write_text(
    f"scenario {{name}}: 3 checks, {{failed}} failed\\n"
    f"scenario {{name}}: model_off: done\\nscenario {{name}}: givens_used: done\\n"
)
with (root / "lock").open("w") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    state = json.loads((root / "active.json").read_text())
    state["active"] -= 1
    (root / "active.json").write_text(json.dumps(state))
"""
    for name in ("e2e.sh", "ryzen.sh"):
        (scripts / name).write_text(stub)
        (scripts / name).chmod(0o755)
    return script, env


def test_regression_mac_uses_run_settings_and_writes_three_done_rows(tmp_path):
    from gothic_ai.scenarios.groups import GIVEN, OFF, RUN

    assert RUN.keys() == OFF | GIVEN
    script, env = regression_runner(tmp_path)
    done = subprocess.run(
        ["bash", str(script), "graph", "lie", "deal"], env=env,
        capture_output=True, text=True, timeout=10,
    )
    assert done.returncode == 0, done.stderr + done.stdout
    report = json.loads((tmp_path / "out/regression.json").read_text())
    assert [row["result"] for row in report["scenarios"]] == ["done"] * 3
    assert [row["group"] for row in report["scenarios"]] == ["OFF", "GIVEN", "GIVEN"]
    assert json.loads((tmp_path / "out/active.json").read_text())["peak"] == 1
    for name in ("graph", "lie", "deal"):
        observed = json.loads((tmp_path / f"out/{name}/env.json").read_text())
        assert observed["LLM"] == "auto" and observed["GAME_BIN"] == "/game"
        assert observed["TIMEOUT"] == str(RUN[name]["timeout"])
        assert ("SAVE" in observed) == RUN[name]["save"]
    markdown = (tmp_path / "out/regression.md").read_text()
    rows = [line.split(" | ") for line in markdown.splitlines() if line.startswith(("| graph |", "| lie |", "| deal |"))]
    assert len(rows) == 3
    assert all(row[2] == row[4] == row[5] == "done" for row in rows)
    assert "Known defect" in markdown


def test_regression_ryzen_runs_three_slots_and_keeps_known_defects_failed(tmp_path):
    script, env = regression_runner(tmp_path)
    env.update(HOST="ryzen", FAIL_CHECK="lie", RETRY="graph")
    done = subprocess.run(
        ["bash", str(script), "graph", "lie", "deal"], env=env,
        capture_output=True, text=True, timeout=10,
    )
    assert done.returncode == 1, done.stderr + done.stdout
    report = json.loads((tmp_path / "out/regression.json").read_text())
    assert [row["result"] for row in report["scenarios"]] == ["done", "failed", "done"]
    assert report["scenarios"][1]["known_defect"] == "Known defect"
    assert report["scenarios"][1]["exit_code"] == 0  # Failed checks still fail the row.
    assert report["scenarios"][0]["log"] == str(tmp_path / "out/graph/run-2/server.log")
    assert json.loads((tmp_path / "out/active.json").read_text())["peak"] == 3
    slots = {
        json.loads((tmp_path / f"out/{name}/env.json").read_text())["RYZEN_WORKTREE"]
        for name in ("graph", "lie", "deal")
    }
    assert slots == {"test-regression-1", "test-regression-2", "test-regression-3"}
