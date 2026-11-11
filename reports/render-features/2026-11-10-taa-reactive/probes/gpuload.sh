#!/usr/bin/env bash
# mean "Device Utilization %" of the Apple GPU over N one-second samples
n=${1:-5}; s=0
for ((i=0;i<n;i++)); do
  v=$(ioreg -r -c IOAccelerator -d 1 -w 0 | grep -o '"Device Utilization %"=[0-9]*' | head -1 | cut -d= -f2)
  s=$((s+v)); sleep 1
done
echo $((s/n))
