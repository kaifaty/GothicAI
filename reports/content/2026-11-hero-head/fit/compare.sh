#!/usr/bin/env bash
# compare.sh <round dir>: overlay (clay at 55% over the concept) and side by side, front and profile
set -euo pipefail
W=$(cd "$(dirname "$0")" && pwd); R="$W/$1"
for v in front profile; do
  ffmpeg -loglevel error -y -i "$W/${v}_ref.png" -i "$R/fit_$v.png" -filter_complex "[1]format=rgba,colorchannelmixer=aa=0.55[o];[0][o]overlay" "$R/over_$v.png"
done
ffmpeg -loglevel error -y -i "$W/front_ref.png" -i "$R/over_front.png" -i "$R/fit_front.png" -i "$W/profile_ref.png" -i "$R/over_profile.png" -i "$R/fit_profile.png" \
  -filter_complex "[2]format=rgba[a];color=0x7a7c80:s=887x887[g1];[g1][a]overlay[f];[5]format=rgba[b];color=0x7a7c80:s=887x887[g2];[g2][b]overlay[p];[0][1][f]hstack=3[top];[3][4][p]hstack=3[bot];[top][bot]vstack=2,scale=1500:-1" -frames:v 1 "$R/compare.jpg"
