#!/usr/bin/env bash
T=/Volumes/DevSSD/Dev/tmp/taa-mac-cost
spec() { case $1 in A) echo "A:";; M) echo "M:";; C) echo "C:OPENGOTHIC_PROBE_CYCLE=250";; D) echo "C2:OPENGOTHIC_PROBE_CYCLE=250";; esac; }
items=(); n=1
for round in "D C A M" "M A D C" "C D M A" "A M C D" "D C A M" "M A D C" "C D M A"; do
  for m in $round; do items+=("$m.$n:$(spec $m)"); done; n=$((n+1))
done
mkdir -p $T/runs/s4
printf '%s\n' "${items[@]}" > $T/runs/s4.items
deadline=$(( $(date +%s) + 90*60 ))
cd $T && nohup ./series3.sh $T/runs/s4 $deadline "${items[@]}" > $T/runs/s4.out 2>&1 &
