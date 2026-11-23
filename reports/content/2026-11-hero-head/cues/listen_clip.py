# The clip of the face of the one listening (scripts/bench/face-listen-clip.bench): from the bench's output makes
# clip.mp4 (its video and sound, the part over each stretch of listening), key.txt (each part: whom Vino listens to,
# the lines said to him, the accents he heard, each nod with its time and the face that came with it or none, the
# apex of ls_w) and sheet.jpg (for several faces the frame before and the frame at its apex). With --off only the
# clip, as clip-off.mp4 (the same bench with OPENGOTHIC_LISTEN_FACE=0).
#   uv run --with pillow python listen_clip.py <bench out dir> <out dir> [--off]
# The parts are the stretches where the watched npc listens (ge_listen of metrics.csv): the first ones while Lobart's
# lines are said to it (his npc_line events), the last two the bench's `hear` (friendly, then angry). The nods are where
# ge_lnods grows; the face of each comes from the engine's "[face] listen" lines of game.log, one per nod in order;
# the accents it heard from the bridge's "[ai] ... hears accent" lines; the face as shown from ls_w.
import csv
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from ingame_clip import FPS, caption, font, frame_at, t
from PIL import Image, ImageDraw

NPC = 'BAU_952_VINO'
SPEAKER = 'BAU_950_LOBART'
SHOWN = 0.02
YELLOW, BG = (255, 214, 120), (22, 22, 22)
FACE = {'flash': 'вспышка бровей', 'smile': 'полуулыбка', 'smile+flash': 'полуулыбка и вспышка', 'frown': 'нахмуривание',
        'nod': 'без лица'}


def runs(rows, column):
    """[first, last] frames where the column is 1."""
    out, start = [], None
    frames = sorted(rows)
    for f in frames:
        on = rows[f][column] == '1'
        if on and start is None:
            start = f
        if not on and start is not None:
            out.append([start, f-1])
            start = None
    if start is not None:
        out.append([start, frames[-1]])
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    off = '--off' in sys.argv
    src, out = Path(args[0]), Path(args[1])
    seg = args[2] if len(args) > 2 else 'clip'
    out.mkdir(parents=True, exist_ok=True)
    video, wav = src/f'{seg}.mp4', src/f'{seg}.wav'
    with open(src/'metrics.csv') as f:
        rows = {int(r['frame']): r for r in csv.DictReader(f) if r['seg'] == seg}
    events = [json.loads(e) for e in (src/'events.jsonl').read_text().splitlines()]
    said = [e for e in events if e['kind'] == 'npc_line' and e['seg'] == seg and e['npc'] == SPEAKER]
    log = (src/'game.log').read_text(errors='replace').splitlines()
    faces = [m[1] for row in log if (m := re.search(r'\[face\] listen [^:]+: (\S+) \((\w+)\)', row))]
    moods = [m[2] for row in log if (m := re.search(r'\[face\] listen [^:]+: (\S+) \((\w+)\)', row))]
    heard = [row for row in log if re.search(rf'\[ai\] {NPC} hears accent', row)]

    # the parts: stretches of listening; those starting before the end of Lobart's lines are his
    stretches = runs(rows, 'ge_listen')
    said_end = max((e['frame'] + e['ms']*FPS//1000 for e in said), default=-1)
    lobart = [r for r in stretches if r[0] <= said_end]
    hero = [r for r in stretches if r[0] > said_end]
    parts = []
    if lobart:
        parts.append(dict(name=f'Вино слушает Лобарта ({len(said)} реплики голосом Inworld, обращены к Вино)',
                          short='слушает Лобарта', lo=lobart[0][0], hi=lobart[-1][1]))
    for (lo, hi), how in zip(hero, ('дружелюбно', 'зло')):
        parts.append(dict(name=f'Вино слушает героя {how} (речь героя — стенд, hear)', short=f'слушает героя {how}',
                          lo=lo, hi=hi))

    # the nods: frames where ge_lnods grows, each with its face from the log in order
    nods, prev = [], None
    for f in sorted(rows):
        n = int(rows[f]['ge_lnods'])
        if prev is not None and n > prev:
            nods.append(f)
        prev = n
    key, shots = [], []
    k = 0
    last = max(rows)
    for i, p in enumerate(parts):
        nxt = parts[i+1]['lo'] if i+1 < len(parts) else last+1
        p['next'] = nxt
        key.append(f'Часть {i+1}. {p["name"]}, {t(p["lo"])} – {t(p["hi"])}')
        if i == 0 and lobart:
            for e in said:
                key.append(f'  Лобарт {t(e["frame"])}: «{e["text"]}» (звук {e["ms"]/1000:.2f} с)')
            key.append(f'  акцентов Лобарта услышано: {len(heard)} (на каждом — повод для кивка: пауза и вероятность '
                       'прежние)')
        count = {}
        for f in [f for f in nods if p['lo'] <= f < nxt]:
            what = faces[k] if k < len(faces) else ('?' if not off else 'nod')
            mood = moods[k] if k < len(moods) else '?'
            k += 1
            count[what] = count.get(what, 0) + 1
            row = f'  кивок {t(f)} (кадр {f}): {FACE.get(what, what)}'
            if what != 'nod' and not off:
                window = [g for g in range(f, min(f+75, last+1)) if g in rows]
                apex = max(window, key=lambda g: float(rows[g]['ls_w']))
                began = next((g for g in window if float(rows[g]['ls_w']) > SHOWN), None)
                row += f' ({mood}); пик {t(apex)} (кадр {apex}), ls_w {float(rows[apex]["ls_w"]):.2f}'
                if began is None:
                    row += ', не показано'
                else:
                    shots.append((f'{i+1}. {FACE.get(what, what)}', max(0, f-3), apex))
            key.append(row)
        n = sum(count.values())
        key.append(f'  итого: кивков {n}, с лицом {n - count.get("nod", 0)}, без лица {count.get("nod", 0)}')
        key.append('')
    key.insert(0, f'Ключ клипа {NPC}: лицо слушающего, частей: {len(parts)}. Время — от начала клипа, {FPS} кадров/с; '
                  'ls_w — лицо слоя, 0..1 его пика.\n')
    if not off:
        (out/'key.txt').write_text('\n'.join(key))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        size = frame_at(video, 0, tmp/'size.png').size
        cmd = ['ffmpeg', '-v', 'error', '-y', '-i', str(video)]
        if wav.exists():
            cmd += ['-i', str(wav)]
        chain, prev = [], '0:v'
        for i, p in enumerate(parts):
            png = tmp/f'cap{i}.png'
            caption(f'Часть {i+1}: {p["short"]}' + (', слой выключен' if off else ''), png, size)
            cmd += ['-i', str(png)]
            n = i + (2 if wav.exists() else 1)
            a, b = p['lo']/FPS, p['next']/FPS
            chain.append(f"[{prev}][{n}:v]overlay=0:0:enable='between(t,{a:.3f},{b:.3f})'[v{i}]")
            prev = f'v{i}'
        cmd += ['-filter_complex', ';'.join(chain) or '[0:v]null[v0]', '-map', f'[{prev if chain else "v0"}]']
        if wav.exists():
            cmd += ['-map', '1:a', '-c:a', 'aac', '-b:a', '192k', '-shortest']
        clip = out/('clip-off.mp4' if off else 'clip.mp4')
        cmd += ['-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-r', str(FPS), str(clip)]
        subprocess.run(cmd, check=True)
        if off:
            print('\n'.join(key))
            print(clip)
            return

        # the sheet: before | apex of the faces, at most 8, cropped to the face
        tiles = []
        for label, rest, apex in shots[:8]:
            pair = []
            for n in (rest, apex):
                im = frame_at(video, n, tmp/f'f{n}.png')
                W, H = im.size
                pair.append(im.crop((W//4, 0, 3*W//4, H)))
            tiles.append((f'{label}, ls_w {float(rows[apex]["ls_w"]):.2f}', pair))
    if tiles:
        tw, th = tiles[0][1][0].size
        scale = min(1.0, 360/th)
        tw, th = int(tw*scale), int(th*scale)
        cols, head = 2, 28
        rowsn = (len(tiles)+cols-1)//cols
        sheet = Image.new('RGB', (cols*(2*tw+12)+(cols-1)*16, rowsn*(th+head+8)), (60, 60, 60))
        d = ImageDraw.Draw(sheet)
        for j, (label, pair) in enumerate(tiles):
            x0, y0 = (j % cols)*(2*tw+12+16), (j//cols)*(th+head+8)
            d.rectangle((x0, y0, x0+2*tw+12, y0+head), fill=BG)
            d.text((x0+8, y0+head//2), f'{label} | до · пик', fill=YELLOW, font=font(17), anchor='lm')
            for m, im in enumerate(pair):
                sheet.paste(im.resize((tw, th), Image.LANCZOS), (x0+m*(tw+12), y0+head))
        sheet.save(out/'sheet.jpg', quality=90)
    print('\n'.join(key))
    print(out/'clip.mp4', out/'key.txt', out/'sheet.jpg' if tiles else '(no sheet)', len(tiles), 'tiles')


if __name__ == '__main__':
    main()
