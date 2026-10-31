#!/usr/bin/env bash
# One bounded original-Bullet front scene; read-only snapshots, never an E2E pass verdict.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${GOTHIC_BODY_SSH:-dev-ubuntu}"
RUN="${1:-bullet-front-snapshot-v1}"
[[ "$RUN" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]*$ ]] || exit 2
printf -v quoted_run '%q' "$RUN"
ssh -o BatchMode=yes "$HOST" 'mkdir -p "$HOME/gothic-ai/physics-stage1/e2e/source/scripts"'
rsync -a "$ROOT/scripts/base-ensure.sh" "$HOST:gothic-ai/physics-stage1/e2e/source/scripts/"
ssh -o BatchMode=yes "$HOST" "bash -s -- $quoted_run" <<'REMOTE'
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
root="$HOME/gothic-ai/physics-stage1/e2e"
binary="$HOME/gothic-ai/physics-stage1/bullet/reference/opengothic/Gothic2Notr"
converter="$(dirname "$binary")/content-convert"
[[ -x "$converter" ]] || { echo "Missing converter: $converter. Rebuild the engine before running physics e2e." >&2; exit 1; }
out="$root/diagnostics/$1"
base="$root/fixtures/eef46cfc3b5d3ca24c75033d3aefed80387407212cfbabb9047bba0b0dc290ff/source"
[[ -d "$base" ]] || base="$root/source"
[[ ! -e "$out" ]] || { echo 'Choose a fresh diagnostic name' >&2; exit 1; }
mkdir -p "$out/source/server" "$out/source/libs/chatgpt-auth" "$out/source/scripts" "$out/see" "$out/reference"
cp "$base/server/pyproject.toml" "$base/server/uv.lock" "$out/source/server/"
rsync -a --exclude=__pycache__/ "$base/server/src" "$out/source/server/"
cp "$base/libs/chatgpt-auth/pyproject.toml" "$out/source/libs/chatgpt-auth/"
rsync -a --exclude=__pycache__/ "$base/libs/chatgpt-auth/src" "$out/source/libs/chatgpt-auth/"
cp "$base/scripts/e2e.sh" "$out/source/scripts/"
cp "$root/source/scripts/base-ensure.sh" "$out/base-ensure.sh"
python3 - "$out/source" "$out" <<'PY'
import hashlib, json, sys
from pathlib import Path
source, out = map(Path, sys.argv[1:])
def source_hash():
    digest = hashlib.sha256()
    files = [source / "scripts/e2e.sh", *sorted((source / "server/src").rglob("*")),
             *sorted((source / "libs/chatgpt-auth/src").rglob("*"))]
    for path in files:
        if path.is_file() and "__pycache__" not in path.parts:
            digest.update(path.relative_to(source).as_posix().encode() + b"\0")
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()
base_hash = source_hash()
assert base_hash == "eef46cfc3b5d3ca24c75033d3aefed80387407212cfbabb9047bba0b0dc290ff", base_hash
path = source / "server/src/gothic_ai/scenarios.py"
text = path.read_text()
start = text.index("async def see_perception(")
end = text.index("\nTOWNSFOLK =", start)
section = text[start:end]
anchor = '    await link.debug("teleport", near=npc, distance=300)\n'
assert section.count(anchor) == 1
snapshot = '''    async def snapshot_front(phase: str) -> None:
        observed = await link.snapshot(npc, fresh=True, timeout=5)
        log.info("physics_e2e_snapshot %s", json.dumps({
            "phase": phase, "snapshot": observed.model_dump(mode="json"),
        }, ensure_ascii=False))

'''
section = section.replace(anchor, snapshot + '    await snapshot_front("before_teleport")\n' + anchor
                          + '    await snapshot_front("after_teleport_ack")\n', 1)
anchor = '\n    view = await perception.fresh(npc)\n'
assert section.count(anchor) == 1
section = section.replace(anchor, '\n    await snapshot_front("after_front_ready")' + anchor, 1)
cut = section.index('    spots = []\n')
section = section[:cut] + '    return _logged(results)\n\n'
path.write_text(text[:start] + section + text[end:])
provenance = {"kind": "physics_front_read_only_diagnostic", "final_e2e_verdict": False,
              "base_server_source_sha256": base_hash, "diagnostic_source_sha256": source_hash(),
              "changes": "Read-only snapshots before/after original teleport and after fresh-push readiness; stop after original front check; no position/action/assertion changes."}
(out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
PY
(cd "$out/source/server" && uv sync --frozen --no-dev --python 3.12 && uv run --frozen --no-dev gothic-ai serve --help >/dev/null)
exec 9>"$HOME/gothic-ai/body-simulation.lock"
flock -n 9 || { echo 'Another simulation owns the shared lock' >&2; exit 1; }
if pgrep -f '[/]Gothic2Notr([[:space:]]|$)|[/]gothic-body-runner([[:space:]]|$)|(^|/)(c\+\+|g\+\+|clang\+\+|ninja)([[:space:]]|$)' >/dev/null; then
  echo 'Another game/compiler is active' >&2; exit 1
fi
python3 - <<'PY'
import socket
with socket.socket() as probe:
    probe.bind(("0.0.0.0", 47927))
PY
cp "$binary" "$out/reference/Gothic2Notr"
cp "$converter" "$out/"
binary="$out/reference/Gothic2Notr"
sha256sum "$binary" > "$out/binary.sha256"
sha256sum "$HOME/gothic-ai/body-baseline/reference/save_slot_1.sav" > "$out/save.sha256"
sha256sum "$out/source/server/uv.lock" > "$out/server-lock.sha256"
cat > "$out/game-wrapper.sh" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
GAME_BIN="$0" bash "$(dirname "$0")/base-ensure.sh" "$GOTHIC_DIR"
exec "$PHYSICS_E2E_BINARY" "$@" -g2 -window -aa 0 -gi 0 -rt 0
WRAPPER
chmod +x "$out/game-wrapper.sh"
cat > "$out/see/Gothic.ini" <<'INI'
[INTERNAL]
vidResIndex=2
[SOUND]
soundVolume=0
musicVolume=0
INI
date -u +%Y-%m-%dT%H:%M:%SZ > "$out/started-at.txt"
unit="gothic-physics-front-$1"
trap 'systemctl --user stop "$unit.service" >/dev/null 2>&1 || true; exit 130' INT TERM
systemd-run --user --wait --pipe --collect --unit="$unit" \
  --property=Nice=10 --property=CPUQuota=400% --property=MemoryMax=12G \
  --property=TasksMax=128 --property=RuntimeMaxSec=120 \
  --setenv="PATH=$HOME/.local/bin:$PATH" \
  --setenv=VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  --setenv=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  --setenv=LP_NUM_THREADS=4 --setenv=ALSOFT_DRIVERS=null --setenv=GOTHIC_AI=1 \
  --setenv=OPENGOTHIC_TEXTURES= --setenv=DECIDER_URL= \
  --setenv=LLM=fake --setenv=ASR=fake --setenv=TTS=fake --setenv=UV_NO_DEV=1 --setenv=UV_FROZEN=1 \
  --setenv="PHYSICS_E2E_BINARY=$binary" --setenv="GAME_BIN=$out/game-wrapper.sh" \
  --setenv="GOTHIC_DIR=$HOME/gothic-ai/body-baseline/assets" \
  --setenv="SAVE=$HOME/gothic-ai/body-baseline/reference/save_slot_1.sav" \
  --setenv="LOG_DIR=$out/see" --setenv=SCENARIO=see --setenv=PORT=47927 --setenv=TIMEOUT=100 \
  /usr/bin/xvfb-run -a -s '-screen 0 320x240x24' \
  /bin/bash "$out/source/scripts/e2e.sh" > "$out/see/launcher.log" 2>&1 && rc=0 || rc=$?
printf '%s\n' "$rc" > "$out/see/exit-code.txt"
systemctl --user stop "$unit.service" >/dev/null 2>&1 || true
trap - INT TERM
date -u +%Y-%m-%dT%H:%M:%SZ > "$out/completed-at.txt"
python3 - "$out" <<'PY'
import json, math, re, sys
from pathlib import Path
out = Path(sys.argv[1])
log = (out / "see/server.log").read_text()
snapshots = [json.loads(row) for row in re.findall(r"physics_e2e_snapshot ([^\n]+)", log)]
for row in snapshots:
    snap = row["snapshot"]
    player, npc = snap["player"]["pos"], snap["npc"]["pos"]
    delta = [p - n for p, n in zip(player, npc)]
    yaw = math.radians(snap["npc"]["rot"])
    row["geometry"] = {"player_minus_npc_cm": delta, "horizontal_distance_cm": math.hypot(delta[0], delta[2]),
                       "distance_cm": math.sqrt(sum(d*d for d in delta)),
                       "body_forward_expected_300_cm": [300*math.cos(yaw), 0, 300*math.sin(yaw)]}
report = json.loads((out / "provenance.json").read_text())
report["snapshots"] = snapshots
report["snapshot_coverage_valid"] = len(snapshots) == 3 and {r["phase"] for r in snapshots} == {
    "before_teleport", "after_teleport_ack", "after_front_ready"}
report["readiness"] = [json.loads(row) for row in re.findall(r"physics_e2e_ready ([^\n]+)", log)]
report["front_check"] = re.findall(r"gothic_ai.scenarios: sees_in_front\s+(done|failed)([^\n]*)", log)
report["renderer_verified"] = "GPU = llvmpipe" in (out / "see/game.log").read_text()
report["started_at_utc"] = (out / "started-at.txt").read_text().strip()
report["completed_at_utc"] = (out / "completed-at.txt").read_text().strip()
for name in ("binary", "save", "server-lock"):
    report[name.replace("-", "_") + "_sha256"] = (out / f"{name}.sha256").read_text().split()[0]
(out / "front-diagnostic.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({"snapshot_coverage_valid": report["snapshot_coverage_valid"],
                  "front_check": report["front_check"], "geometry": [r["geometry"] for r in snapshots]}))
PY
REMOTE
mkdir -p "$ROOT/data/physics/stage1/e2e/$RUN"
rsync -a --exclude=source/ --exclude=reference/ --exclude='*.sav' --exclude='*.sqlite*' --exclude=game-wrapper.sh \
  "$HOST:gothic-ai/physics-stage1/e2e/diagnostics/$RUN/" "$ROOT/data/physics/stage1/e2e/$RUN/"
