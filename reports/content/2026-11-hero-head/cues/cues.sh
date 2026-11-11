#!/bin/bash
# The gesture and leak probe of face-cues.md (step 1): weights from cues.py, rendered on the head with Mark's shapes
# (render_cues.py: render_a2f.py plus turning eyes), two clips with sound and two contact sheets for the self-check.
#   cues.sh [<out dir>]      default ~/.local/share/gothic-ai/remaster/hero-head/cues-1
# Clip A cues-dictionary.mp4: orig_hero, plain and each of the 11 gestures twice, 3 x 4 cells of 480 px.
# Clip B cues-blind.mp4: ten 4.5 s segments at 720 px, numbered only; the key is answers.txt, never in the clip.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
D=$HOME/.local/share/gothic-ai
O=${1:-$D/remaster/hero-head/cues-1}
HEAD=$D/mpfb/work/shapes/head-mark.glb
CL=$D/face-sources/clips
PY=$D/mpfb/venv/bin/python
BL=/Applications/Blender.app/Contents/MacOS/Blender
F=$O/frames
mkdir -p "$F"
"$PY" "$H/cues.py" "$O" | tee "$O/cues.log" | grep '^A'   # the B lines carry the key: only in the log

render() {  # <csv> <frames dir> <size>: again only when the csv changed
  local sum; sum=$(md5 -q "$1")
  [ "$(cat "$2/done" 2>/dev/null)" = "$sum" ] && return 0
  rm -rf "$2"
  "$BL" -b --factory-startup -P "$H/render_cues.py" -- "$HEAD" "$1" "$2/" "$3" >"$2.log" 2>&1
  echo "$sum" > "$2/done"
}
export -f render; export H HEAD BL
A=(plain flash doubt smirk press disgust squint frown chin wince glance roll)
RU=("без жестов" "вспышка бровей" "сомнение" "ухмылка" "поджатые губы" "брезгливость" "прищур подозрения"
    "хмурится" "подбородок" "поморщился" "взгляд в сторону" "закатил глаза")
{ for k in "${A[@]}"; do echo "$O/a/$k.csv $F/a/$k 480"; done
  for i in $(seq -w 1 10); do echo "$O/b/seg$i.csv $F/b/seg$i 720"; done; } |
  xargs -P 4 -L 1 bash -c 'mkdir -p "$(dirname "$1")"; render "$0" "$1" "$2"'

# clip A: a header strip over each row of four, the line's own sound
LABEL="$H/../shapes/label.py"
PIL=(uv run -q --with pillow python)
for r in 0 1 2; do "${PIL[@]}" "$LABEL" "$F/a/h$r.png" "$([ $r = 0 ] && echo orig_hero)" "${RU[@]:$((r*4)):4}"; done
ins=(); for k in "${A[@]}"; do ins+=(-framerate 30 -i "$F/a/$k/f%04d.jpg"); done
ffmpeg -y -loglevel error "${ins[@]}" -loop 1 -i "$F/a/h0.png" -loop 1 -i "$F/a/h1.png" -loop 1 -i "$F/a/h2.png" \
  -i "$CL/orig_hero.wav" -filter_complex \
  "[0:v][1:v][2:v][3:v]hstack=inputs=4[r0];[4:v][5:v][6:v][7:v]hstack=inputs=4[r1];[8:v][9:v][10:v][11:v]hstack=inputs=4[r2];[12:v][r0][13:v][r1][14:v][r2]vstack=inputs=6:shortest=1,format=yuv420p[o]" \
  -map "[o]" -map 15:a -af "afade=t=in:d=0.03,areverse,afade=t=in:d=0.05,areverse" \
  -c:v libx264 -crf 20 -preset fast -c:a aac -b:a 160k -ar 48000 -shortest "$O/cues-dictionary.mp4"

# clip B: each segment under its number, held 0.5 s after it ends; the sound once from the wav halves in one pass
SEGS=(); while read -r x; do SEGS+=("$x"); done < <("$PY" -c "
import json; p=json.load(open('$O/plan.json'))['b']
for s in p['segments']: print(s['line'], 0 if s['half']=='first' else p['seg_len'], p['seg_len'])")
list=$F/b/list.txt; : > "$list"; ains=(); af=""
for i in $(seq 0 9); do
  n=$(printf %02d $((i+1))); read -r line start len <<< "${SEGS[$i]}"
  "${PIL[@]}" "$H/sheet.py" strip "$F/b/n$n.png" 720 "$((i+1))"
  ffmpeg -y -loglevel error -framerate 30 -i "$F/b/seg$n/f%04d.jpg" -loop 1 -i "$F/b/n$n.png" -filter_complex \
    "[1:v][0:v]vstack=inputs=2:shortest=1,tpad=stop_mode=clone:stop_duration=0.5,format=yuv420p[o]" \
    -map "[o]" -c:v libx264 -crf 20 -preset fast -r 30 "$F/b/seg$n.mp4"
  echo "file '$F/b/seg$n.mp4'" >> "$list"
  d=$(ffprobe -v error -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$F/b/seg$n.mp4")
  ains+=(-i "$CL/$line.wav")
  af+="[$i:a]atrim=start=$start:duration=$len,asetpts=PTS-STARTPTS,aresample=48000,afade=t=in:d=0.03,areverse,afade=t=in:d=0.05,areverse,apad,atrim=0:$d[a$i];"
done
for j in $(seq 0 9); do af+="[a$j]"; done
ffmpeg -y -loglevel error -f concat -safe 0 -i "$list" -an -c:v copy "$F/b/video.mp4"
ffmpeg -y -loglevel error "${ains[@]}" -filter_complex "${af}concat=n=10:v=0:a=1[a]" -map "[a]" -c:a pcm_s16le "$F/b/audio.wav"
ffmpeg -y -loglevel error -i "$F/b/video.mp4" -i "$F/b/audio.wav" -c:v copy -c:a aac -b:a 160k -shortest "$O/cues-blind.mp4"

# self-check stills
"${PIL[@]}" "$H/sheet.py" a "$O"
"${PIL[@]}" "$H/sheet.py" b "$O"
ls -la "$O"/*.mp4 "$O"/sheet-*.jpg "$O/answers.txt"
