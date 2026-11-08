"""Side-by-side wood fracture clip from two own-wood-fracture-clip bench outputs.

uv run --with pillow python3 make_clip.py OUT.mp4 ITEMS LEFT_DIR "left label" RIGHT_DIR "right label"

ITEMS is barrel, crate or barrel,crate. Both takes of an item are aligned on the frame of
their break and cut to the same length. One item: a 12 s take, then the close
"intact | pieces" pair of each side. Several items: 9.5 s takes one after another.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = '/System/Library/Fonts/Supplemental/Arial.ttf'
W, H = 960, 540
STILL = 2.5  # seconds of each close pair


def break_frame(directory, item):
    for line in (Path(directory) / 'events.jsonl').open():
        event = json.loads(line)
        if event.get('kind') == 'destruction_break' and event.get('seg') == f'{item}-side':
            return event['frame']
    raise SystemExit(f'{directory}: no break in {item}-side')


def label(path, text):
    image = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(FONT, 26)
    box = draw.textbbox((0, 0), text, font=font)
    draw.rectangle((0, 0, box[2] + 28, box[3] + 20), fill=(0, 0, 0, 170))
    draw.text((14, 9), text, font=font, fill=(255, 255, 255, 255))
    image.save(path)


def main():
    out, items, left, left_text, right, right_text = sys.argv[1:7]
    items = items.split(',')
    single = len(items) == 1
    lead, tail = (45, 320) if single else (30, 255)  # frames before and after the break, 30 fps
    sides = ((left, left_text), (right, right_text))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        inputs, filters, parts = [], [], []

        def add(*arguments):
            inputs.extend(arguments)
            return sum(1 for value in inputs if value == '-i') - 1

        def labelled(stream, text, name):
            path = tmp / f'{name}.png'
            label(path, text)
            filters.append(f'[{stream}][{add("-i", str(path))}:v]overlay=0:0[{name}]')
            return name

        for item in items:
            halves = []
            for i, (directory, text) in enumerate(sides):
                start = break_frame(directory, item) - lead
                n = add('-i', str(Path(directory) / f'{item}-side.mp4'))
                filters.append(f'[{n}:v]trim=start_frame={start}:end_frame={start + lead + tail},'
                               f'setpts=PTS-STARTPTS,scale={W}:{H},setsar=1,format=yuv420p[t{item}{i}]')
                halves.append(labelled(f't{item}{i}', text if single else f'{item}: {text}', f'l{item}{i}'))
            filters.append(f'[{halves[0]}][{halves[1]}]hstack[take{item}]')
            parts.append(f'take{item}')
        if single:
            for i, (directory, text) in enumerate(sides):
                shots = []
                for shot in ('before', 'after'):
                    n = add('-loop', '1', '-t', str(STILL), '-framerate', '30',
                            '-i', str(Path(directory) / f'{items[0]}-close-{shot}.png'))
                    filters.append(f'[{n}:v]scale={W}:{H},setsar=1,format=yuv420p[s{i}{shot}]')
                    shots.append(f's{i}{shot}')
                filters.append(f'[{shots[0]}][{shots[1]}]hstack[p{i}]')
                parts.append(labelled(f'p{i}', f'{text}: целая | куски', f'q{i}'))
        filters.append(''.join(f'[{part}]' for part in parts) + f'concat=n={len(parts)}:v=1:a=0,format=yuv420p[out]')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', *inputs, '-filter_complex', ';'.join(filters),
                        '-map', '[out]', '-r', '30', '-c:v', 'libx264', '-crf', '23', '-preset', 'slow',
                        '-movflags', '+faststart', out], check=True)


main()
