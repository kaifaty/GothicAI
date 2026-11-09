#!/bin/bash
# The emotion probe of audio2face.md: Audio2Face v3.0 lines with one emotion label each (a2f-weights' last argument),
# played on the head with Mark's shapes; per line a 4 x 2 grid (neutral and seven emotions) with its sound.
#   emotions.sh <head.glb> <weights dir with <line>.<emotion>.csv> <clips dir> <out.mp4>
set -euo pipefail
HEAD=$1; WT=$2; CL=$3; OUT=$4
H=$(cd "$(dirname "$0")" && pwd)
F=$(dirname "$OUT")/frames
BL=/Applications/Blender.app/Contents/MacOS/Blender
EMO=(neutral anger joy fear sadness pain cheekiness disgust)
RU=("нейтрально" "злость" "радость" "страх" "грусть" "боль" "озорство" "отвращение")
LINES=(inworld_xardas orig_hero orig_biff)
render() {  # line emotion
  local d="$F/$1/$2"
  [ -f "$d/done" ] && return 0
  "$BL" -b --factory-startup -P "$H/render_a2f.py" -- "$HEAD" "$WT/$1.$2.csv" "$d/" >/dev/null 2>&1
  touch "$d/done"
}
export -f render; export F WT H BL HEAD
for l in "${LINES[@]}"; do for e in "${EMO[@]}"; do echo "$l $e"; done; done | xargs -P 3 -L 1 bash -c 'render $0 $1'
LABEL="$H/../shapes/label.py"
SEG=()
for l in "${LINES[@]}"; do
  uv run -q --with pillow python "$LABEL" "$F/$l/h1.png" "$l" "${RU[@]:0:4}"
  uv run -q --with pillow python "$LABEL" "$F/$l/h2.png" "" "${RU[@]:4:4}"
  ins=(); i=0
  for e in "${EMO[@]}"; do ins+=(-framerate 30 -i "$F/$l/$e/f%04d.jpg"); i=$((i+1)); done
  ffmpeg -y -loglevel error "${ins[@]}" -loop 1 -i "$F/$l/h1.png" -loop 1 -i "$F/$l/h2.png" -i "$CL/$l.wav" \
    -filter_complex "[0:v][1:v][2:v][3:v]hstack=inputs=4[r1];[4:v][5:v][6:v][7:v]hstack=inputs=4[r2];[8:v][r1][9:v][r2]vstack=inputs=4:shortest=1,format=yuv420p[o]" \
    -map "[o]" -map 10:a -af "afade=t=in:d=0.03,areverse,afade=t=in:d=0.05,areverse" -c:v libx264 -crf 20 -preset fast -c:a aac -b:a 160k -ar 48000 -shortest "$F/$l/seg.mp4"
  SEG+=("$F/$l/seg.mp4")
done
# the picture is joined as is; the sound once more from the wavs in one pass, so the joins do not click
printf "file '%s'\n" "${SEG[@]}" > "$F/list.txt"
ffmpeg -y -loglevel error -f concat -safe 0 -i "$F/list.txt" -an -c:v copy "$F/video.mp4"
ains=(); af=""; i=0
for l in "${LINES[@]}"; do
  d=$(ffprobe -v error -select_streams v:0 -show_entries stream=duration -of csv=p=0 "$F/$l/seg.mp4")
  ains+=(-i "$CL/$l.wav")
  af+="[$i:a]aresample=48000,afade=t=in:d=0.03,areverse,afade=t=in:d=0.05,areverse,apad,atrim=0:$d[a$i];"
  i=$((i+1))
done
for j in $(seq 0 $((i-1))); do af+="[a$j]"; done
ffmpeg -y -loglevel error "${ains[@]}" -filter_complex "${af}concat=n=$i:v=0:a=1[a]" -map "[a]" -c:a pcm_s16le "$F/audio.wav"
ffmpeg -y -loglevel error -i "$F/video.mp4" -i "$F/audio.wav" -c:v copy -c:a aac -b:a 160k -shortest "$OUT"
echo "$OUT"
