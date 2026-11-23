#!/usr/bin/env bash
# Stops gothic-gemma, runs the A/B on 127.0.0.1:47833 with the same flags as run.sh, always restarts the service.
set -uo pipefail
cd ~/gothic-ai/gemma
T=~/gothic-ai/gemma/mtp-test
PORT=47833
N=${N:-40}
restore() {
  [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
  systemctl --user start gothic-gemma
  echo "restored gothic-gemma: $(systemctl --user is-active gothic-gemma) at $(date -u +%T)"
  touch "$T/done"
}
trap restore EXIT
rm -f "$T/done"
echo "stop gothic-gemma at $(date -u +%T)"
systemctl --user stop gothic-gemma
sleep 3

run() {  # label, extra flags...
  local label=$1; shift
  llama.cpp/build/bin/llama-server -m models/gemma-4-12b-it-qat-q4_0.gguf --alias gemma-4-12b \
    --host 127.0.0.1 --port $PORT -ngl 99 -c 65536 -np 4 -fa on -ctk q8_0 -ctv q8_0 --no-cache-idle-slots \
    --jinja --reasoning-budget 0 --chat-template-kwargs '{"enable_thinking":false}' "$@" \
    > "$T/server-$label.log" 2>&1 &
  pid=$!
  for _ in $(seq 120); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 1; done
  nvidia-smi --query-gpu=memory.used --format=csv,noheader > "$T/vram-$label.txt"
  python3 "$T/bench.py" "http://127.0.0.1:$PORT" "$T/cases.jsonl" "$label" "$T/res-$label.jsonl" "$N"
  nvidia-smi --query-gpu=memory.used --format=csv,noheader >> "$T/vram-$label.txt"
  kill "$pid"; wait "$pid" 2>/dev/null; pid=
  sleep 2
}
MTP=(-md "$T/mtp-gemma-4-12b-it-qat-q8_0.gguf" --spec-type draft-mtp)
run base
run mtp3 "${MTP[@]}" --spec-draft-n-max 3
run mtp2 "${MTP[@]}" --spec-draft-n-max 2
run mtp4 "${MTP[@]}" --spec-draft-n-max 4
run base2
