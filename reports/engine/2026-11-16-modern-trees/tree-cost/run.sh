#!/usr/bin/env bash
# See tree-cost.md for the content sets to stage on Ryzen.
# run.sh OUT [variant...] repeats the named variants from round 3; -vsmstats starts at 1.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
OUT="${1:?usage: run.sh /absolute/local/out [variant...]}"
shift
repeats=2
if (($#)); then repeats=1; fi

plan="$(python3 - "$@" <<'PY'
from collections import Counter
import sys

# Timing variants in the first round's order. The common runner reverses the second round.
sets = {"C": "0", "F64": "64", "F256": "256", "F1024": "1024",
        "F1024h": "1024-h", "F1024q": "1024-q", "F1024e": "1024-e"}
flags = {"F1024-rt0": "OPENGOTHIC_RT=0", "C-rt0": "OPENGOTHIC_RT=0",
         "F1024-wind0": "OPENGOTHIC_FOLIAGE=light", "F1024-crown0": "OPENGOTHIC_CROWN_LIGHT=0"}
counters = ["F1024", "F64", "F256", "F1024h", "F1024q", "F1024e"]
requested = sys.argv[1:]
names = requested or [*sets, *flags, *(name + "-vsmstats" for name in counters)]
seen = Counter()
for name in names:
    variant = name.removesuffix("-vsmstats")
    if variant not in sets and variant not in flags:
        sys.exit(f"tree-cost: unknown variant {name!r}")
    stats = variant != name
    label = name
    if requested or stats:
        first = 1 if stats else 3
        label = f"{name}/round-{first + seen[name]}"
        seen[name] += 1
    content = sets[variant.split("-")[0]]
    setting = "OPENGOTHIC_VSM_STATS=1" if stats else flags.get(variant, "")
    print(f"{label}|OPENGOTHIC_CONTENT=~/gothic-ai/tree-cost/content/tree-cost-{content} {setting}".rstrip())
PY
)"
mkdir -p "$OUT"
BENCH_HOST=ryzen PERF_WINDOW=2560x1440 PERF_REPEAT="$repeats" PERF_ENV=OPENGOTHIC_GPU_PASSES=1 \
  PERF_VID_RES=0 PERF_INI= PERF_WAIT=600 PERF_ARGS= RYZEN_LOCK_WAIT="${RYZEN_LOCK_WAIT:-7200}" \
  "$ROOT/scripts/perf.sh" --variants "$OUT" trees <<< "$plan" 2>&1 | tee "$OUT.log"
