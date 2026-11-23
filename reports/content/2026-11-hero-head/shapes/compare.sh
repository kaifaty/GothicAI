#!/bin/bash
# The comparison clip of face-shapes.md: six Audio2Face v3.0 lines, four columns (by default MPFB with the lip fix,
# MPFB raw, ICT with the fix, Mark raw), with their sound. Frames by ../a2f/render_a2f.py, labels by label.py.
#   [COLS="mpfb v3fix,mark v3,mpfb smpfb,ict sict"] compare.sh <work dir with head-{mpfb,ict,mark}.glb> <weights dir> <clips dir> <out.mp4>
# A column is "<head> <weights>": <weights dir>/<line>.<weights>.csv played on <work dir>/head-<head>.glb.
set -euo pipefail
W=$1; WT=$2; CL=$3; OUT=$4
H=$(cd "$(dirname "$0")" && pwd)
BL=/Applications/Blender.app/Contents/MacOS/Blender
IFS=, read -r -a COLS <<< "${COLS:-mpfb v3fix,mpfb v3,ict v3fix,mark v3}"
LINES=(inworld_hero inworld_xardas orig_hero orig_sylvio orig_biff player_mic)
render() {  # head weights line
  local d="$W/frames/$3/$1-$2"
  [ -f "$d/done" ] && return 0
  "$BL" -b --factory-startup -P "$H/../a2f/render_a2f.py" -- "$W/head-$1.glb" "$WT/$3.$2.csv" "$d/" >/dev/null 2>&1
  touch "$d/done"
}
export -f render; export W WT H BL
for l in "${LINES[@]}"; do for c in "${COLS[@]}"; do echo "$c $l"; done; done | \
  xargs -P 3 -L 1 bash -c 'render $0 $1 $2'
SEG=()
for l in "${LINES[@]}"; do
  uv run -q --with pillow python "$H/label.py" "$W/frames/$l/head.png" "$l" "${COLS[@]}"
  ins=(); flt=""; i=0
  for c in "${COLS[@]}"; do set -- $c; ins+=(-framerate 30 -i "$W/frames/$l/$1-$2/f%04d.jpg"); flt+="[$i:v]"; i=$((i+1)); done
  ffmpeg -y -loglevel error "${ins[@]}" -loop 1 -i "$W/frames/$l/head.png" -i "$CL/$l.wav" \
    -filter_complex "${flt}hstack=inputs=4[v];[4:v][v]vstack=shortest=1,format=yuv420p[o]" \
    -map "[o]" -map 5:a -af "afade=t=in:d=0.03,areverse,afade=t=in:d=0.05,areverse" -c:v libx264 -crf 20 -preset fast -c:a aac -b:a 160k -ar 48000 -shortest "$W/frames/$l/seg.mp4"
  SEG+=("$W/frames/$l/seg.mp4")
done
# the picture is joined as is; the sound once more from the wavs in one pass, so the joins do not click
printf "file '%s'\n" "${SEG[@]}" > "$W/frames/list.txt"
ffmpeg -y -loglevel error -f concat -safe 0 -i "$W/frames/list.txt" -an -c:v copy "$W/frames/video.mp4"
ains=(); af=""; i=0
for l in "${LINES[@]}"; do
  d=$(ffprobe -v error -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$W/frames/$l/seg.mp4")
  ains+=(-i "$CL/$l.wav")
  af+="[$i:a]aresample=48000,afade=t=in:d=0.03,areverse,afade=t=in:d=0.05,areverse,apad,atrim=0:$d[a$i];"
  i=$((i+1))
done
for j in $(seq 0 $((i-1))); do af+="[a$j]"; done
ffmpeg -y -loglevel error "${ains[@]}" -filter_complex "${af}concat=n=$i:v=0:a=1[a]" -map "[a]" -c:a pcm_s16le "$W/frames/audio.wav"
ffmpeg -y -loglevel error -i "$W/frames/video.mp4" -i "$W/frames/audio.wav" -c:v copy -c:a aac -b:a 160k -shortest "$OUT"
echo "$OUT"
