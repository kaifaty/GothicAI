#!/usr/bin/env bash
# Automatic OFF/GIVEN regression: [HOST=mac GAME_BIN=...] [OUT=/path] scripts/e2e-regression.sh [scenarios...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/server"
exec uv run python - "$ROOT" "$@" <<'PY'
import concurrent.futures
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import time

from gothic_ai.scenarios.groups import GIVEN, OFF, RUN

root = Path(sys.argv[1])
names = sys.argv[2:] or sorted(OFF | GIVEN)
unknown = sorted(set(names) - (OFF | GIVEN))
if unknown:
    raise SystemExit("Automatic regression requires OFF/GIVEN scenarios: " + ", ".join(unknown))
if len(names) != len(set(names)):
    raise SystemExit("Each scenario must be listed once")
host = os.environ.get("HOST", "ryzen")
if host not in {"ryzen", "mac"}:
    raise SystemExit("HOST must be ryzen or mac")
if host == "mac" and not os.environ.get("GAME_BIN"):
    raise SystemExit("HOST=mac requires GAME_BIN")
out = Path(os.environ.get("OUT", str(root / ".ryzen" / time.strftime("regression-%Y%m%d-%H%M%S")))).resolve()
if out.exists() and any(out.iterdir()):
    raise SystemExit("OUT already contains results; choose an empty directory")
save = None
if any(RUN[name]["save"] for name in names):
    save = os.environ.get("SAVE")
    if not save:
        common = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "--path-format=absolute", "--git-common-dir"], text=True
        ).strip()
        save = str(Path(common).parent / "save_slot_1.sav")
    save = str(Path(save).expanduser().resolve())
    if not Path(save).is_file():
        raise SystemExit(f"Missing SAVE: {save} (set SAVE to slot 1)")
known = {}
document = (root / "docs/e2e.md").read_text()
section = re.search(r"^## Известные дефекты\s*\n(.*?)(?=^## |\Z)", document, re.M | re.S)
if section:
    for line in section.group(1).splitlines():
        cells = [cell.strip().strip("`") for cell in line.strip().strip("|").split("|")]
        if line.startswith("|") and len(cells) >= 2 and cells[0] in RUN:
            known[cells[0]] = cells[1]
out.mkdir(parents=True, exist_ok=True)
worktree = os.environ.get("RYZEN_WORKTREE") or (root.parent.name if "worktrees" in root.parts else root.name)
slots = queue.Queue()
for slot in range(1, 4):
    slots.put(slot)


def run(name):
    slot = slots.get()
    directory = out / name
    directory.mkdir()
    log = directory / "runner.log"
    env = dict(os.environ, SCENARIO=name, LLM="auto", ASR="fake", TTS="fake", LOG_DIR=str(directory),
               TIMEOUT=str(RUN[name]["timeout"]))
    env.pop("SAVE", None)
    if RUN[name]["save"]:
        env["SAVE"] = save
    for key, value in RUN[name].get("env", {}).items():
        env[key] = str(Path(value).expanduser()) if value.startswith("~/") else value
    command = [str(root / "scripts/e2e.sh")]
    if host == "ryzen":
        env["RYZEN_WORKTREE"] = f"{worktree}-regression-{slot}"
        command = [str(root / "scripts/ryzen.sh"), "e2e"]
    started = time.monotonic()
    print(f"Regression {name} ({'OFF' if name in OFF else 'GIVEN'}): {directory}", flush=True)
    try:
        if host == "mac":
            while subprocess.run(
                ["pgrep", "-f", r"^(/[^ ]*/)?Gothic2Notr( |$)"], env=dict(env, LC_ALL="C"),
                stdout=subprocess.DEVNULL, check=False,
            ).returncode == 0:
                time.sleep(5)
        with log.open("w") as output:
            code = subprocess.run(command, env=env, cwd=root, stdout=output, stderr=subprocess.STDOUT).returncode
        attempts = [path for path in directory.glob("run-*")
                    if path.is_dir() and re.fullmatch(r"run-[0-9]+", path.name)]
        final = max(attempts, key=lambda path: int(path.name[4:])) if attempts else directory
        server_log = final / "server.log"
        text = server_log.read_text() if server_log.exists() else ""
        guards = {}
        for guard in ("model_off", "givens_used"):
            matches = re.findall(rf"scenario {re.escape(name)}: {guard}: ([^\n]+)", text)
            guards[guard] = matches[-1].strip() if matches else "missing"
        failed_checks = any(int(count) for count in re.findall(
            rf"scenario {re.escape(name)}: [0-9]+ (?:checks|steps), ([0-9]+) failed", text
        ))
        result = ("done" if code == 0 and not failed_checks
                  and all(value == "done" for value in guards.values()) else "failed")
        row = dict(scenario=name, group="OFF" if name in OFF else "GIVEN", result=result,
                   seconds=round(time.monotonic() - started, 2), exit_code=code, **guards,
                   log=str(server_log if server_log.exists() else log), known_defect=known.get(name, ""))
        print(f"Regression {name}: {result} ({row['seconds']}s)", flush=True)
        return row
    finally:
        slots.put(slot)


with concurrent.futures.ThreadPoolExecutor(max_workers=1 if host == "mac" else 3) as pool:
    rows = list(pool.map(run, names))
(out / "regression.json").write_text(json.dumps({"host": host, "scenarios": rows}, ensure_ascii=False, indent=2) + "\n")
lines = ["# E2E regression", "", f"Host: {host}", "",
         "| Scenario | Group | Result | Seconds | model_off | givens_used | Log | Known defect in main |",
         "|---|---|---|---:|---|---|---|---|"]
for row in rows:
    values = [row["scenario"], row["group"], row["result"], str(row["seconds"]), row["model_off"],
              row["givens_used"], f"[log]({row['log']})", row["known_defect"]]
    lines.append("| " + " | ".join(value.replace("|", r"\|") for value in values) + " |")
(out / "regression.md").write_text("\n".join(lines) + "\n")
print(f"Regression: {sum(row['result'] == 'done' for row in rows)}/{len(rows)} done; {out / 'regression.md'}")
raise SystemExit(0 if all(row["result"] == "done" for row in rows) else 1)
PY
