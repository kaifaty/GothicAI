#!/usr/bin/env bash
# One headless lockstep npc-day-parity run (gq batch job, no display).
# usage: run.sh <name> <tier full|simplified|auto> [extra env assignments...]
set -uo pipefail
task="$HOME/gothic-ai/headless-bench"
name="$1"; tier="$2"; shift 2
out="$task/runs/$name"
rm -rf "$out"; mkdir -p "$out/run"
eval "$(python3 "$task/source/scripts/ryzen_game.py")"
cat >"$out/run/Gothic.ini" <<INI
[INTERNAL]
vidResIndex=0
[SOUND]
soundVolume=0
musicVolume=0
musicEnabled=0
worldSounds=1
INI
cd "$out/run"
started=$(date +%s.%N)
env OPENGOTHIC_NPC_SIM_TIER="$tier" OPENGOTHIC_BENCH="$task/inputs/${SCENARIO:-npc-day-parity}.bench" OPENGOTHIC_BENCH_OUT="$out" "$@" \
  /usr/bin/time -v "$task/build/opengothic/gothic-body-runner" -g "$GOTHIC_DIR" -g2 -devmode -nointro \
  --game-parity --world NEWWORLD.ZEN --scenario "$task/inputs/${SCENARIO:-npc-day-parity}.bench" --output "$out" --mode lockstep \
  >"$out/game.log" 2>&1
rc=$?
ended=$(date +%s.%N)
python3 -c "print(f'wall {float($ended)-float($started):.1f}s rc=$rc')" | tee "$out/wall.txt"
grep -c "bench\] done" "$out/game.log" || true
tail -25 "$out/game.log"
exit $rc
