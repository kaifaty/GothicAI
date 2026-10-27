#!/usr/bin/env bash
# Submit one bounded phase and return immediately. No training or PPO paths.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
stage="${1:?Usage: nav-jumps-remote.sh build|bootstrap|pilot|recheck|candidates|discover|routes|record-pilot [1|2]|demo-pilot|record|analyze}"
attempt="${2:-1}"
export GQ_OWNER=nav-jumps
host=ryzen
task="$(ssh -o ConnectTimeout=10 "$host" 'printf "%s/gothic-ai/nav-jumps" "$HOME"')"
source_dir="$task/source"
write_record_script() {
    ssh "$host" "cat > '$task/record.sh'" <<'RECORD'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1
df -h "$HOME"
cd "$task/source"
"$task/.venv/bin/python" -I - "$HOME/gothic-ai/nav-demos-jumps" <<'PY'
from collections import Counter
import json
from pathlib import Path
import sys
root=Path(sys.argv[1])
pilot=root/"demo-check"
if not pilot.is_dir():
    raise RuntimeError("Run demo-pilot with the demonstration success criterion first")
episodes=[json.loads(path.read_text()) for path in (pilot/"episodes").glob("*/episode.json")]
if len(episodes)!=30 or Counter(row["type"] for row in episodes)!={"jump":10,"climb":10,"drop":10}:
    raise RuntimeError("Pilot must finish all original 30 routes")
original=[json.loads(path.read_text()) for path in (root/"episodes").glob("*/episode.json")]
if {row["route_index"] for row in episodes}!={row["route_index"] for row in original}:
    raise RuntimeError("Pilot route indices differ from the original stopped 30")
if any(row.get("success_criterion")!="demo_goal_action_landing_side_v1" for row in episodes):
    raise RuntimeError("Pilot used an obsolete link-probe success criterion")
for kind in ("jump","climb","drop"):
    if sum(row.get("transition_success") is True for row in episodes if row["type"]==kind)<7:
        raise RuntimeError(f"Pilot {kind} below 70%; full recording forbidden")
if any(not (pilot/"episodes"/f"{row['route_index']:04d}"/"transition-frames.jsonl").is_file() for row in episodes):
    raise RuntimeError("Pilot lacks physical-frame transition evidence")
print(f"Full recording admitted by {pilot}",flush=True)
PY
"$task/.venv/bin/python" -I "$task/source/scripts/nav_jump_record.py" \
  --runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game" \
  --save "$task/input/save.sav" --cache "$task/cache-transitions-cv" \
  --links "$task/accepted-links-cv.txt" --roads "$task/input/places.json" \
  --routes "$task/routes/routes.jsonl" --output "$HOME/gothic-ai/nav-demos-jumps/final" \
  --storage-root "$HOME/gothic-ai/nav-demos-jumps" --count 267
"$task/.venv/bin/python" -I "$task/source/scripts/nav_demos.py" check "$HOME/gothic-ai/nav-demos-jumps/final"
RECORD
}
case "$stage" in
  build|bootstrap|pilot|record-pilot|demo-pilot)
    if [[ "$stage" == record-pilot && "$attempt" != 1 && "$attempt" != 2 ]]; then
      echo 'record-pilot attempt must be 1 or 2' >&2; exit 2
    fi
    ssh "$host" "mkdir -p '$source_dir' '$task/input'"
    rsync -a --exclude=build --exclude='build-*' --exclude=__pycache__ \
      "$ROOT/OpenGothic" "$ROOT/scripts" "$ROOT/AGENTS.md" "$host:$source_dir/"
    rsync -a "$ROOT/reports/navigation/2026-10-16-teacher/raw/accepted-links.txt" \
      "$host:$task/input/accepted-links-legacy.txt"
    rsync -a "$ROOT/server/src/gothic_ai/gamedata/places.json" "$host:$task/input/places.json"
    # Shared game assets and the historical save are read-only inputs. Copy the save.
    ssh "$host" "test -f '$task/input/save.sav' || cp \"\$HOME/gothic-ai/nav-mesh/save_slot_1.sav\" '$task/input/save.sav'"
    ssh "$host" "cat > '$task/build.sh'" <<'BUILD'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
export VULKAN_SDK="$HOME/gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
export PATH="$HOME/.local/bin:$VULKAN_SDK/bin:$PATH"
export OMP_NUM_THREADS=4 OPENBLAS_NUM_THREADS=1
df -h "$HOME"
if [[ ! -x "$task/.venv/bin/python" ]]; then
  uv venv "$task/.venv"
fi
uv pip install --python "$task/.venv/bin/python" numpy pytest
cmake -S "$task/source/OpenGothic" -B "$task/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENGOTHIC_JOLT_X86=x86-64-v3 \
  -DCMAKE_CXX_FLAGS="-I$VULKAN_SDK/include" -DCMAKE_EXE_LINKER_FLAGS="-L$VULKAN_SDK/lib"
targets=(Gothic2Notr gothic-body-runner check-navmesh)
if [[ "${1:-}" == record-pilot ]]; then targets=(Gothic2Notr gothic-body-runner check-navkeyteacher); fi
cmake --build "$task/build" --target "${targets[@]}" --parallel 3
cd "$task/source"
tests=(scripts/tests/test_nav_jumps.py)
if [[ "${1:-}" == demo-pilot ]]; then
  tests=(scripts/tests/test_nav_jump_record.py)
elif [[ "${1:-}" == record-pilot ]]; then
  tests=(scripts/tests/test_nav_jump_record.py scripts/tests/test_nav_demos.py)
elif [[ "${1:-}" != pilot ]]; then
  tests+=(scripts/tests/test_nav_jump_routes.py scripts/tests/test_nav_jump_observability.py scripts/tests/test_nav_demos.py)
fi
"$task/.venv/bin/python" -m pytest -q "${tests[@]}"
if [[ "${1:-}" == bootstrap ]]; then
  for phase in recheck candidates; do
    args=(--runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game"
      --save "$task/input/save.sav" --cache "$task/cache-ground" --output "$task/$phase")
    if [[ "$phase" == recheck ]]; then
      args+=(--links "$task/input/accepted-links-legacy.txt")
    else
      args+=(--site-budget 1200)
    fi
    "$task/.venv/bin/python" -I "$task/source/scripts/nav_jumps.py" "$phase" "${args[@]}"
  done
elif [[ "${1:-}" == pilot ]]; then
  "$task/.venv/bin/python" -I "$task/source/scripts/nav_jumps.py" pilot \
    --runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game" \
    --save "$task/input/save.sav" --cache "$task/cache-ground" --output "$task/pilot-grounded" \
    --links "$task/candidates/candidates.txt" --exclude-links "$task/recheck/accepted-links-cv.txt"
elif [[ "${1:-}" == record-pilot || "${1:-}" == demo-pilot ]]; then
  pilot_output="$HOME/gothic-ai/nav-demos-jumps/repair-${2:?attempt}"
  if [[ "$1" == demo-pilot ]]; then pilot_output="$HOME/gothic-ai/nav-demos-jumps/demo-check"; fi
  pilot_indices="$("$task/.venv/bin/python" -I - "$HOME/gothic-ai/nav-demos-jumps" <<'PY'
from collections import Counter
import json
from pathlib import Path
import sys
episodes=[json.loads(path.read_text()) for path in (Path(sys.argv[1])/"episodes").glob("*/episode.json")]
if len(episodes)!=30 or Counter(row["type"] for row in episodes)!={"jump":10,"climb":10,"drop":10}:
    raise RuntimeError("Expected original stopped pilot: exactly 10 attempts per type")
print(",".join(str(row["route_index"]) for row in episodes))
PY
)"
  "$task/.venv/bin/python" -I "$task/source/scripts/nav_jump_record.py" \
    --runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game" \
    --save "$task/input/save.sav" --cache "$task/cache-transitions-cv" \
    --links "$task/accepted-links-cv.txt" --roads "$task/input/places.json" \
    --routes "$task/routes/routes.jsonl" --indices "$pilot_indices" --count 30 --require-70pct \
    --output "$pilot_output" --storage-root "$HOME/gothic-ai/nav-demos-jumps"
  "$task/.venv/bin/python" -I "$task/source/scripts/nav_demos.py" check "$pilot_output"

fi
BUILD
    "$ROOT/scripts/gq" submit --class batch --threads 4 --mem 6G --max 2h \
      --name "nav jumps $stage engine runner tests" -- bash "$task/build.sh" "$stage" "$attempt"
    ;;
  recheck|candidates|discover)
    ssh "$host" 'df -h "$HOME"'
    output="$task/$stage"
    [[ "$stage" != discover ]] || output="$task/discover-grounded"
    args=("$task/.venv/bin/python" -I "$source_dir/scripts/nav_jumps.py" "$stage"
      --runner "$task/build/opengothic/gothic-body-runner"
      --assets "${task%/nav-jumps}/game" --save "$task/input/save.sav"
      --cache "$task/cache-ground" --output "$output")
    if [[ "$stage" == recheck ]]; then
      args+=(--links "$task/input/accepted-links-legacy.txt")
    elif [[ "$stage" == discover ]]; then
      args+=(--links "$task/candidates/candidates.txt" --exclude-links "$task/recheck/accepted-links-cv.txt")
    else
      args+=(--site-budget 1200)
    fi
    "$ROOT/scripts/gq" submit --class batch --threads 2 --mem 6G --max 2h \
      --name "nav jumps $stage CV" -- env OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1 "${args[@]}"
    ;;
  routes)
    # This phase uses the verified runner; only the changed route selector is synced.
    ssh "$host" "mkdir -p '$source_dir/scripts/tests'"
    rsync -a "$ROOT/scripts/nav_jump_routes.py" "$host:$source_dir/scripts/"
    rsync -a "$ROOT/scripts/tests/test_nav_jump_routes.py" "$host:$source_dir/scripts/tests/"
    ssh "$host" "cat > '$task/routes.sh'" <<'ROUTES'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1
df -h "$HOME"
cd "$task/source"
"$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_jump_routes.py
"$task/.venv/bin/python" -I - "$task" <<'PY'
from collections import Counter
import importlib.util
import json
from pathlib import Path
import sys
task = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("nav_jumps_merge", task / "source/scripts/nav_jumps.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
old = probe.links(task / "recheck/accepted-links-cv.txt")
new = probe.links(task / "discover-grounded/accepted-links-cv.txt")
combined = old + new
if len(combined) != 121 or len({row["id"] for row in combined}) != len(combined):
    raise RuntimeError("Expected 121 verified links with unique IDs")
destination = task / "accepted-links-cv.txt"
if destination.exists() and probe.links(destination) != combined:
    raise RuntimeError("Refusing to replace the verified transition set")
probe.write_links(destination, combined)
print(json.dumps(dict(verified_links=len(combined), old=len(old), new=len(new),
                      by_type=dict(Counter(probe.TYPES[row["type"]] for row in combined)))), flush=True)
PY
"$task/.venv/bin/python" -I "$task/source/scripts/nav_jump_routes.py" run \
  --runner "$task/build/opengothic/gothic-body-runner" --assets "$HOME/gothic-ai/game" \
  --save "$task/input/save.sav" --cache "$task/cache-transitions-cv" \
  --links "$task/accepted-links-cv.txt" --roads "$task/input/places.json" --places "$task/input/places.json" \
  --output "$task/routes" --per-link 3
ROUTES
    "$ROOT/scripts/gq" submit --class batch --threads 2 --mem 6G --max 2h \
      --name "nav jumps routes v2 max3 per transition" -- bash "$task/routes.sh"
    ;;
  record)
    # Record the approved route set with the existing CV runner and private cache.
    ssh "$host" "mkdir -p '$source_dir/scripts/tests'"
    rsync -a "$ROOT/scripts/nav_jump_record.py" "$host:$source_dir/scripts/"
    rsync -a "$ROOT/scripts/tests/test_nav_jump_record.py" "$host:$source_dir/scripts/tests/"
    write_record_script
    "$ROOT/scripts/gq" submit --class batch --threads 2 --mem 6G --max 2h \
      --name "nav jumps record 267 teacher v2 CV 240s" -- bash "$task/record.sh"
    ;;
  analyze)
    # Offline only: verify every attempt, then explain successful demos from
    # learner_input. No engine, teacher rollout or model training is started.
    ssh "$host" "mkdir -p '$source_dir/scripts/tests' '$task/analysis'"
    rsync -a "$ROOT/scripts/nav_jump_observability.py" "$host:$source_dir/scripts/"
    rsync -a "$ROOT/scripts/tests/test_nav_jump_observability.py" "$host:$source_dir/scripts/tests/"
    ssh "$host" "cat > '$task/analysis.sh'" <<'ANALYSIS'
#!/usr/bin/env bash
set -euo pipefail
task="$(cd "$(dirname "$0")" && pwd)"
export OMP_NUM_THREADS=2 OPENBLAS_NUM_THREADS=1
cd "$task/source"
df -h "$HOME"
"$task/.venv/bin/python" -m pytest -q scripts/tests/test_nav_jump_observability.py
"$task/.venv/bin/python" -I "$task/source/scripts/nav_demos.py" check \
  "$HOME/gothic-ai/nav-demos-jumps/final" > "$task/analysis/navdemo-check.json"
"$task/.venv/bin/python" -I "$task/source/scripts/nav_jump_observability.py" \
  "$HOME/gothic-ai/nav-demos-jumps/final" --successful-only \
  --success-criterion demo_goal_action_landing_side_v1 \
  --output "$task/analysis/observability.json" --csv "$task/analysis/decisions.csv"
ANALYSIS
    "$ROOT/scripts/gq" submit --class batch --threads 2 --mem 6G --max 30m \
      --name "nav jumps final NAVDEMO check and learner observability" -- bash "$task/analysis.sh"
    ;;
  *) echo "Unknown phase: $stage" >&2; exit 2 ;;
esac
