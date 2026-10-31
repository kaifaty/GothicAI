#!/usr/bin/env bash
# Visibility regressions against the real AI bridge, CPU lavapipe, and scripted LLM.
# Usage: scripts/physics-e2e-remote.sh prepare|run|collect [bullet|jolt] [fresh-run-name]
# Preparation patches only the isolated server snapshot: daylight and bounded fresh-push waits.
# PHYSICS_E2E_SOURCE_BINARY selects an immutable remote candidate; each run copies its binary.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${GOTHIC_BODY_SSH:-dev-ubuntu}"
REMOTE="gothic-ai/physics-stage1/e2e"
ACTION="${1:-prepare}"
ENGINE="${2:-jolt}"
RUN="${3:-$ENGINE-$(date -u +%Y%m%dT%H%M%SZ)}"
CPU_QUOTA="${PHYSICS_E2E_CPU_QUOTA:-400}"
RENDER_THREADS="${PHYSICS_E2E_RENDER_THREADS:-4}"
LOCK_WAIT="${PHYSICS_E2E_LOCK_WAIT:-0}"
SOURCE_BINARY="${PHYSICS_E2E_SOURCE_BINARY:-}"
[[ "$ENGINE" == bullet || "$ENGINE" == jolt ]] || exit 2
[[ "$RUN" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || exit 2
[[ "$CPU_QUOTA" =~ ^[1-8]00$ && "$RENDER_THREADS" =~ ^[1-8]$ ]] || exit 2
[[ "$LOCK_WAIT" =~ ^[0-9]+$ && "$LOCK_WAIT" -le 1200 ]] || exit 2

remote_script() {
  local escaped="" arg
  for arg in "$@"; do printf -v escaped '%s %q' "$escaped" "$arg"; done
  ssh -o BatchMode=yes "$HOST" "bash -s --$escaped"
}

case "$ACTION" in
prepare)
  remote_script "$REMOTE" <<'REMOTE'
set -euo pipefail
mkdir -p "$HOME/$1/source/server" "$HOME/$1/source/libs/chatgpt-auth" "$HOME/$1/source/scripts"
REMOTE
  rsync -a --exclude=.venv/ --exclude=__pycache__/ --exclude=.pytest_cache/ --exclude=.ruff_cache/ \
    --exclude='.env*' --exclude='*.sqlite*' --exclude='*.log' \
    "$ROOT/server/pyproject.toml" "$ROOT/server/uv.lock" "$ROOT/server/src" \
    "$HOST:$REMOTE/source/server/"
  rsync -a --exclude=__pycache__/ "$ROOT/libs/chatgpt-auth/pyproject.toml" \
    "$ROOT/libs/chatgpt-auth/src" "$HOST:$REMOTE/source/libs/chatgpt-auth/"
  rsync -a "$ROOT/scripts/e2e.sh" "$ROOT/scripts/base-ensure.sh" "$HOST:$REMOTE/source/scripts/"
  remote_script "$REMOTE" <<'REMOTE'
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
python3 - "$HOME/$1/source/server/src/gothic_ai/scenarios/perception.py" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
text = path.read_text()
start = text.index("async def see_perception(")
end = text.index("\nTOWNSFOLK =", start)
section = text[start:end]
anchor = "    await link.wait_world()\n"
if section.count(anchor) != 1:
    raise SystemExit("Unexpected see scenario; world-wait shape check refused")
# Daylight is applied only to the farm phase, after the original Xardas front/wall commands.
collector = '''    # Passive diagnostic: preserve the exact pushed actors, clock and native tick.
    import json
    from ..protocol import NpcViews
    recent_push = {"tick": -1, "views": [], "received_wall": 0.0}

    def capture_push(message: GameMessage) -> None:
        if isinstance(message, NpcViews):
            recent_push.update(tick=message.t, views=message.views, received_wall=time.monotonic())
            record = message.model_dump(mode="json")
            record["received_wall_seconds"] = recent_push["received_wall"]
            record["known_human_pairs"] = [
                {"viewer": v.npc, "other": o.id, "game_minutes": v.time,
                 "viewer_state": v.state, "other_state": o.state, "distance_cm": o.distance}
                for v in message.views for o in v.sees
                if o.kind == "human" and perception.knows(v.npc, o)
            ]
            log.info("physics_e2e_view %s", json.dumps(record, ensure_ascii=False))

    link.listeners.append(capture_push)

    async def wait_pushed(label: str, target: str, predicate: Callable[[NpcView], bool]) -> None:
        before_tick = recent_push["tick"]
        before_wall = time.monotonic()
        deadline = before_wall + 12
        matched = False
        observed = None
        while time.monotonic() < deadline:
            if recent_push["tick"] > before_tick and recent_push["received_wall"] <= deadline:
                observed = next((v for v in recent_push["views"] if v.npc.upper() == target.upper()), None)
                if observed is not None and predicate(observed):
                    matched = True
                    break
            await asyncio.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
        log.info("physics_e2e_ready %s", json.dumps({
            "phase": label, "target": target, "before_tick": before_tick,
            "after_tick": recent_push["tick"],
            "elapsed_wall_seconds": round(time.monotonic() - before_wall, 3),
            "fresh_push_wall_seconds": round(recent_push["received_wall"] - before_wall, 3),
            "max_wait_seconds": 12, "matched": matched,
            "view": observed.model_dump(mode="json") if observed is not None else None,
        }, ensure_ascii=False))
'''
collector_anchor = "    results: dict[str, str] = {}\n"
if section.count(collector_anchor) != 1:
    raise SystemExit("Unexpected see scenario; push collector patch refused")
section = section.replace(collector_anchor, collector_anchor + collector, 1)
front_query = '    await link.debug("teleport", near=npc, distance=300)\n    await asyncio.sleep(1)\n    view = await perception.fresh(npc)\n'
front_replace = '    await link.debug("teleport", near=npc, distance=300)\n    await wait_pushed("front_hero", npc, lambda v: v.hero is not None)\n    view = await perception.fresh(npc)\n'
if section.count(front_query) != 1:
    raise SystemExit("Unexpected see scenario; front push readiness patch refused")
section = section.replace(front_query, front_replace, 1)
farm_anchor = '    await link.debug("teleport", near=farmer, distance=300)\n'
if section.count(farm_anchor) != 2:
    raise SystemExit("Unexpected see scenario; scoped farm daylight patch refused")
farm_daylight = ('    # Isolated CPU fixture: daylight belongs to the farm acquaintance phase.\n'
                 '    await link.debug("time", hour=8, minute=0)\n'
                 '    await asyncio.sleep(3)\n')
section = section.replace(farm_anchor, farm_daylight + farm_anchor, 1)
old_wait = "        await asyncio.sleep(1.5)  # a push or two\n        scan()\n"
new_wait = '''        # CPU rendering advances game time more slowly than wall time.
        # Wait for a fresh pushed scene and apply the original acquaintance predicate.
        before_tick = recent_push["tick"]
        before_wall = time.monotonic()
        deadline = before_wall + 12

        def fresh_acquaintance() -> bool:
            if recent_push["tick"] <= before_tick or recent_push["received_wall"] > deadline:
                return False
            scan()
            return bool(named)

        matched = False
        while time.monotonic() < deadline:
            if fresh_acquaintance():
                matched = True
                break
            await asyncio.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
        log.info("physics_e2e_wait %s", json.dumps({
            "near": tie, "before_tick": before_tick, "after_tick": recent_push["tick"],
            "elapsed_wall_seconds": round(time.monotonic() - before_wall, 3),
            "fresh_push_wall_seconds": round(recent_push["received_wall"] - before_wall, 3),
            "max_wait_seconds": 12,
            "matched": matched, "names": sorted(set(named)),
            "game_minutes": sorted({v.time for v in recent_push["views"] if v.time is not None}),
        }, ensure_ascii=False))
'''
if section.count(old_wait) != 1:
    raise SystemExit("Unexpected see scenario; fresh-push wait patch refused")
section = section.replace(old_wait, new_wait, 1)
teleport_anchor = '        try:\n            await link.debug("teleport", near=tie, distance=300)\n'
if section.count(teleport_anchor) != 1:
    raise SystemExit("Unexpected see scenario; teleport tick gate patch refused")
text = text[:start] + section + text[end:]
start = text.index("async def seen_memory(")
end = text.index("\nasync def ", start + 1)
section = text[start:end]
desk_anchor = "    desk = sightings.desk\n"
if section.count(desk_anchor) != 1:
    raise SystemExit("Unexpected seen scenario; collector patch refused")
section = section.replace(desk_anchor, desk_anchor + collector.replace("perception.knows", "desk.knows"), 1)
urban_anchor = '    # check_claim\'s sight is the engine\'s: story_state `seen` agrees with what that one\'s view sees\n'
urban_ready = '    await wait_pushed("urban_humans", smith, lambda v: bool([o.id for o in v.sees if o.kind == "human"][:3]))\n'
if section.count(urban_anchor) != 1:
    raise SystemExit("Unexpected seen scenario; urban push readiness patch refused")
section = section.replace(urban_anchor, urban_anchor + urban_ready, 1)
path.write_text(text[:start] + section + text[end:])
PY
cd "$HOME/$1/source/server"
uv sync --frozen --no-dev --python 3.12
uv run --frozen --no-dev gothic-ai serve --help >/dev/null
REMOTE
  ;;
run)
  remote_script "$REMOTE" "$ENGINE" "$RUN" "$CPU_QUOTA" "$RENDER_THREADS" "$LOCK_WAIT" "$SOURCE_BINARY" <<'REMOTE'
set -euo pipefail
root="$HOME/$1"
engine="$2"
run="$3"
cpu_quota="$4"
renderer_threads="$5"
lock_wait="$6"
binary="$HOME/gothic-ai/physics-stage1/$engine/build/opengothic/Gothic2Notr"
if [[ "$engine" == bullet ]]; then
  binary="$HOME/gothic-ai/physics-stage1/bullet/reference/opengothic/Gothic2Notr"
fi
[[ -z "$7" ]] || binary="$7"
out="$root/runs/$run"
[[ -x "$binary" ]] || { echo "Build $binary first" >&2; exit 1; }
converter="$(dirname "$binary")/content-convert"
[[ -x "$converter" ]] || { echo "Missing converter: $converter. Rebuild the engine before running physics e2e." >&2; exit 1; }
[[ ! -e "$out" ]] || { echo 'Choose a fresh run name' >&2; exit 1; }
exec 9>"$HOME/gothic-ai/body-simulation.lock"
flock -w "$lock_wait" 9 || { echo 'Another simulation owns the shared lock' >&2; exit 1; }
echo "Shared simulation lock acquired for $run"
if python3 - <<'PY'
from pathlib import Path
for process in Path("/proc").iterdir():
    if not process.name.isdecimal():
        continue
    try:
        executable = (process / "exe").readlink().name.removesuffix(" (deleted)")
    except OSError:
        continue
    if executable in {"Gothic2Notr", "gothic-body-runner"}:
        print(f"Active executable: pid={process.name} {executable}")
        raise SystemExit(0)
raise SystemExit(1)
PY
then
  echo 'Another game/runner is active' >&2; exit 1
fi
mkdir -p "$out"
printf '%s\n' "$binary" > "$out/source-binary.txt"
mkdir -p "$out/reference"
cp "$binary" "$out/reference/Gothic2Notr"
cp "$converter" "$out/"
binary="$out/reference/Gothic2Notr"
cat > "$out/game-wrapper.sh" <<'WRAPPER'
#!/usr/bin/env bash
exec "$PHYSICS_E2E_BINARY" "$@" -g2 -window -aa 0 -gi 0 -rt 0
WRAPPER
chmod +x "$out/game-wrapper.sh"
printf '%s\n' "$engine" > "$out/engine.txt"
sha256sum "$binary" > "$out/binary.sha256"
sha256sum "$HOME/gothic-ai/body-baseline/reference/save_slot_1.sav" > "$out/save.sha256"
sha256sum "$root/source/server/uv.lock" > "$out/server-lock.sha256"
python3 - "$root/source" "$out" "$HOME/gothic-ai/body-baseline" "$cpu_quota" "$renderer_threads" <<'PY'
import hashlib, json, sys
from pathlib import Path
source, out, baseline = map(Path, sys.argv[1:4])
files = [source / "scripts/e2e.sh"]
files += sorted((source / "server/src").rglob("*"))
files += sorted((source / "libs/chatgpt-auth/src").rglob("*"))
digest = hashlib.sha256()
count = 0
for path in files:
    if path.is_file() and "__pycache__" not in path.parts:
        digest.update(path.relative_to(source).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
        count += 1
reference = baseline / "runs/melee-reference-stage1-v4/manifest.json"
provenance = {"server_source_sha256": digest.hexdigest(), "server_source_files": count,
              "assets_directory": str(baseline / "assets"),
              "assets_reference_manifest": str(reference) if reference.exists() else None,
              "assets_reference_manifest_sha256": hashlib.sha256(reference.read_bytes()).hexdigest()
                  if reference.exists() else None,
              "runtime_limits": {"cpu_quota_percent": int(sys.argv[4]), "renderer_threads": int(sys.argv[5]),
                                 "memory_max_gib": 12, "scenario_timeout_seconds": 240},
              "isolated_scenario_patch": "see: original Xardas front ordering, blind 1s replaced by exact-target fresh native push with hero view present, then original single fresh sight assertion; 08:00 + 3s settling only before first farm teleport; acquaintance push waits; seen: fresh urban human push readiness; each wait bounded to 12s; original actor positions/actions/selection/assertions unchanged apart scoped daylight; passive view/tick diagnostics"}
(out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
PY
date -u +%Y-%m-%dT%H:%M:%SZ > "$out/started-at.txt"
failed=0
unit=""
trap '[[ -z "$unit" ]] || systemctl --user stop "$unit.service" >/dev/null 2>&1 || true; exit 130' INT TERM
for scenario in see seen lie; do
  scenario_out="$out/$scenario"
  mkdir -p "$scenario_out"
  cat > "$scenario_out/Gothic.ini" <<'INI'
[INTERNAL]
vidResIndex=2
[SOUND]
soundVolume=0
musicVolume=0
INI
  python3 - <<'PY'
import socket
with socket.socket() as probe:
    try:
        probe.bind(("0.0.0.0", 47927))
    except OSError as error:
        raise SystemExit(f"Isolated e2e port 47927 is occupied: {error}")
PY
  unit="gothic-physics-e2e-$run-$scenario"
  systemd-run --user --wait --pipe --collect --unit="$unit" \
    --property=Nice=10 --property="CPUQuota=$cpu_quota%" --property=MemoryMax=12G \
    --property=TasksMax=128 --property=RuntimeMaxSec=300 \
    --setenv="PATH=$HOME/.local/bin:$PATH" \
    --setenv="VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json" \
    --setenv="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" \
    --setenv="LP_NUM_THREADS=$renderer_threads" --setenv=ALSOFT_DRIVERS=null --setenv=GOTHIC_AI=1 \
    --setenv="OPENGOTHIC_TEXTURES=" --setenv="DECIDER_URL=" \
    --setenv=LLM=fake --setenv=ASR=fake --setenv=TTS=fake \
    --setenv=UV_NO_DEV=1 --setenv=UV_FROZEN=1 \
    --setenv="PHYSICS_E2E_BINARY=$binary" --setenv="GAME_BIN=$out/game-wrapper.sh" \
    --setenv="GOTHIC_DIR=$HOME/gothic-ai/body-baseline/assets" \
    --setenv="SAVE=$HOME/gothic-ai/body-baseline/reference/save_slot_1.sav" \
    --setenv="LOG_DIR=$scenario_out" --setenv="SCENARIO=$scenario" \
    --setenv=PORT=47927 --setenv=TIMEOUT=240 \
    /usr/bin/time -v -o "$scenario_out/resources.txt" \
    /usr/bin/xvfb-run -a -s '-screen 0 320x240x24' \
    /bin/bash "$root/source/scripts/e2e.sh" > "$scenario_out/launcher.log" 2>&1 \
    && rc=0 || rc=$?
  printf '%s\n' "$rc" > "$scenario_out/exit-code.txt"
  [[ "$rc" == 0 ]] || failed=1
  rg 'scenario (see|seen|lie):|gothic_ai.scenarios:.* (done|failed)' \
    "$scenario_out/server.log" || true
  # Wait for the e2e launcher's owned process group to close before the next game.
  systemctl --user stop "$unit.service" >/dev/null 2>&1 || true
done
trap - INT TERM
date -u +%Y-%m-%dT%H:%M:%SZ > "$out/completed-at.txt"
python3 - "$out" <<'PY'
import json, re, sys
from pathlib import Path
out = Path(sys.argv[1])
scenarios = {}
expected_names = {
    "see": {"sees_in_front", "not_through_walls", "hero_by_look", "acquaintances",
            "fake_guild", "in_the_turn", "pushed", "places"},
    "seen": {"time_in_view", "sighting", "elsewhere", "story_state_sight", "back",
             "in_the_turn", "caught", "wary", "trust", "follows_the_save"},
    "lie": {"quest", "paid_for_a_lie", "found_out", "hostile", "attack", "rumor", "knows_in_talk"},
}
for scenario, names in expected_names.items():
    expected = len(names)
    path = out / scenario
    log = (path / "server.log").read_text() if (path / "server.log").exists() else ""
    matches = re.findall(r"gothic_ai.scenarios: (\w+)\s+(done|failed)([^\n]*)", log)
    checks = {name: {"status": status, "detail": detail.strip()} for name, status, detail in matches}
    summaries = re.findall(rf"scenario {scenario}: (\d+) checks, (\d+) failed", log)
    game_log = (path / "game.log").read_text() if (path / "game.log").exists() else ""
    device = re.search(r"GPU = ([^\n]+)", game_log)
    renderer_verified = device is not None and "llvmpipe" in device[1].lower()
    rc = int((path / "exit-code.txt").read_text())
    waits = [json.loads(row) for row in re.findall(r"gothic_ai.scenarios: physics_e2e_wait ([^\n]+)", log)]
    views = [json.loads(row) for row in re.findall(r"gothic_ai.scenarios: physics_e2e_view ([^\n]+)", log)]
    readiness = [json.loads(row) for row in re.findall(r"gothic_ai.scenarios: physics_e2e_ready ([^\n]+)", log)]
    expected_phases = {"see": {"front_hero"}, "seen": {"urban_humans"}, "lie": set()}
    readiness_valid = len(readiness) == len(expected_phases[scenario]) and {r["phase"] for r in readiness} == expected_phases[scenario]
    readiness_valid = readiness_valid and all(
        row["matched"] is True and (row["after_tick"] > row["before_tick"] and
                               0 <= row["fresh_push_wall_seconds"] <= row["max_wait_seconds"] == 12 and
                               row["view"] is not None and row["view"]["npc"].upper() == row["target"].upper())
        for row in readiness)
    waits_valid = len(waits) <= 6 and all(
        not row["matched"] or (row["after_tick"] > row["before_tick"] and
                               0 <= row["fresh_push_wall_seconds"] <= row["max_wait_seconds"] == 12)
        for row in waits)
    passed = rc == 0 and renderer_verified and len(summaries) == 1
    passed = passed and set(checks) == names and len(matches) == expected
    passed = passed and int(summaries[0][0]) == expected and int(summaries[0][1]) == 0
    passed = passed and all(item["status"] == "done" for item in checks.values())
    passed = passed and waits_valid and readiness_valid
    scenarios[scenario] = {"status": "passed" if passed else "failed", "exit_code": rc,
                           "expected_checks": expected, "cpu_renderer_verified": renderer_verified,
                           "graphics_device": device[1] if device is not None else None, "checks": checks,
                           "acquaintance_waits": waits, "readiness_waits": readiness,
                           "fresh_push_waits_valid": waits_valid and readiness_valid,
                           "known_human_pairs": [{"native_tick": v["t"], **pair}
                                                 for v in views for pair in v["known_human_pairs"]]}
report = {"schema_version": 1, "kind": "physics_stage1_e2e", "engine": (out / "engine.txt").read_text().strip(),
          "renderer": "lavapipe", "screen": "320x240", "llm": "fake", "brain": False,
          "started_at_utc": (out / "started-at.txt").read_text().strip(),
          "completed_at_utc": (out / "completed-at.txt").read_text().strip(), "scenarios": scenarios,
          "status": "passed" if all(s["status"] == "passed" for s in scenarios.values()) else "failed"}
report["provenance"] = json.loads((out / "provenance.json").read_text())
for name in ("binary", "save", "server-lock"):
    report["provenance"][name.replace("-", "_") + "_sha256"] = (out / f"{name}.sha256").read_text().split()[0]
(out / "e2e-summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({s: v["status"] for s, v in scenarios.items()}))
sys.exit(0 if report["status"] == "passed" else 1)
PY
echo "Evidence: $out"
exit "$failed"
REMOTE
  ;;
collect)
  local_out="$ROOT/data/physics/stage1/e2e/$RUN"
  mkdir -p "$local_out"
  rsync -a --exclude='*.sav' --exclude='*.sqlite*' --exclude=game-wrapper.sh --exclude=reference/ \
    "$HOST:$REMOTE/runs/$RUN/" "$local_out/"
  echo "$local_out"
  ;;
*) exit 2 ;;
esac
