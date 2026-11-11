#!/usr/bin/env bash
# Mac series of the fix: 8 runs of the cycle-fix.patch build (tag F), 60 min at most.
T=/Volumes/DevSSD/Dev/tmp/taa-mac-cost
items=(); for n in 1 2 3 4 5 6 7 8; do items+=("F.$n:F:OPENGOTHIC_PROBE_CYCLE=250"); done
mkdir -p $T/runs/fix
deadline=$(( $(date +%s) + 60*60 ))
cd $T && nohup ./series3.sh $T/runs/fix $deadline "${items[@]}" > $T/runs/fix.out 2>&1 &
