# The clip of the brows on the accents of the voice (scripts/bench/face-brows-clip.bench): from the bench's output
# makes clip.mp4 (its video and sound, "Реплика N" over each line), key.txt (for each line its text and voice, each
# accent with its time, the word it falls on, the rise of the tone and whether the brows went up, down or stayed) and
# sheet.jpg (for several moves the frame before and the frame at its apex). With --off only the clip, as clip-off.mp4
# (the same bench with OPENGOTHIC_SPEECH_BROWS=0).
#   uv run --with pillow python brows_clip.py <bench out dir> <out dir> [--off]
# The lines are the watched npc's "output" events; the text of an AI line comes from its npc_line event, of an
# original one from OU.BIN of GOTHIC_DIR. The accents come from the engine's "[face] accent" lines of game.log (ms of
# the line's sound), their place in the clip from lip_t of metrics.csv, the brows as shown from br_w. The word of an
# accent is an estimate by the share of characters over the line's voice (marked ≈).
import csv
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from ingame_clip import FPS, caption, font, frame_at, t
from PIL import Image, ImageDraw

NPC = 'BAU_952_VINO'
LEAD_MS = 100  # speech_brows lead_ms of the profile: the brows start this much before the syllable
SHOWN = 0.02
YELLOW, BG = (255, 214, 120), (22, 22, 22)
GOTHIC_DIR = Path(os.environ.get('GOTHIC_DIR', '/Volumes/DevSSD/Dev/games/Gothic II'))


def original_texts():
    """name.WAV -> text of the original lines (OU.BIN, Windows-1251)."""
    ou = GOTHIC_DIR/'_work/Data/Scripts/CONTENT/CUTSCENE/OU.BIN'
    if not ou.exists():
        return {}
    data = ou.read_bytes()
    out = {}
    for m in re.finditer(rb'\x01..([^\x00-\x08\x0e-\x1f]{2,600}?)\x12\x06\x00\x00\x00\x01..([A-Za-z0-9_]+\.WAV)', data,
                         re.S):
        out[m[2].decode().upper()] = m[1].decode('cp1251', 'replace')
    return out


def lines_of(src, seg):
    events = [json.loads(e) for e in (src/'events.jsonl').read_text().splitlines()]
    texts = {e['frame']: e['text'] for e in events if e['kind'] == 'npc_line' and e['seg'] == seg and e['npc'] == NPC}
    ou = original_texts()
    out = []
    for e in events:
        if e['kind'] != 'output' or e['seg'] != seg or e['npc'] != NPC:
            continue
        name = e['name']
        original = name.upper().startswith('DIA_')
        text = ou.get(name.upper()+'.WAV', '?') if original else texts.get(e['frame'], '?')
        out.append(dict(name=name, frame=e['frame'], text=text, voice='оригинал' if original else 'Inworld'))
    return out


def accents_of(src):
    """Per voice of the watched npc in the order they started: its name and accents (ms, rise, loud, move)."""
    voices = []
    for row in (src/'game.log').read_text(errors='replace').splitlines():
        if m := re.search(r'\[face\] voice [^:]+: (\S*) ', row):
            voices.append(dict(name=m[1], accents=[]))
        elif voices and (m := re.search(r'\[face\] accent [^:]+: (-?\d+) ms, rise ([\d.]+) st, loud ([\d.]+): (\w+)', row)):
            voices[-1]['accents'].append(dict(ms=int(m[1]), rise=float(m[2]), loud=float(m[3]), move=m[4]))
    return voices


def word_at(text, share):
    words = [(m.start(), m.group()) for m in re.finditer(r'\S+', text)]
    if not words:
        return '?'
    at = share*len(text)
    best = words[0][1]
    for start, w in words:
        if start <= at:
            best = w
    return best.strip('.,!?…«»"')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    off = '--off' in sys.argv
    src, out = Path(args[0]), Path(args[1])
    seg = args[2] if len(args) > 2 else 'clip'
    out.mkdir(parents=True, exist_ok=True)
    video, wav = src/f'{seg}.mp4', src/f'{seg}.wav'
    with open(src/'metrics.csv') as f:
        rows = {int(r['frame']): r for r in csv.DictReader(f) if r['seg'] == seg}
    last = max(rows)
    lines = lines_of(src, seg)
    voices = accents_of(src)
    key, shots = [], []
    for i, line in enumerate(lines):
        f0 = line['frame']
        nxt = lines[i+1]['frame'] if i+1 < len(lines) else last+1
        line['next'] = nxt
        frames = [f for f in range(f0, nxt) if f in rows and rows[f]['lip_on'] == '1']
        dur = max((int(rows[f]['lip_t']) for f in frames), default=0)
        voice = next((v for v in voices if v['name'].upper().startswith(line['name'].upper())), None)
        if voice is not None:
            voices.remove(voice)
        f1 = frames[-1] if frames else f0
        key.append(f'Реплика {i+1} ({line["name"]}, голос: {line["voice"]}), {t(f0)} – {t(f1)}')
        key.append(f'  «{line["text"]}»')
        if voice is None or not voice['accents']:
            key.append('  акцентов нет' if voice is not None else '  голос не найден в журнале')
            key.append('')
            continue

        def frame_of(ms):
            return next((f for f in frames if int(rows[f]['lip_t']) >= ms), None)

        for a in voice['accents']:
            fs = frame_of(a['ms'])
            word = word_at(line['text'], a['ms']/dur) if dur else '?'
            what = {'up': 'брови вверх', 'down': 'брови вниз', 'skip': 'без движения'}[a['move']]
            if fs is None:
                key.append(f'  акцент ≈«{word}» на {a["ms"]} мс звука: вне записи')
                continue
            row = f'  акцент ≈«{word}» {t(fs)} (кадр {fs}), подъём тона {a["rise"]:.1f} пт, громкость {a["loud"]:.2f}: {what}'
            if a['move'] != 'skip':
                start = frame_of(a['ms']-LEAD_MS) or fs
                window = [f for f in range(start, min(start+25, nxt)) if f in rows]
                apex = max(window, key=lambda f: abs(float(rows[f]['br_w'])))
                weight = float(rows[apex]['br_w'])
                began = next((f for f in window if abs(float(rows[f]['br_w'])) > SHOWN), None)
                row += (f'; начало {t(began)} (кадр {began}), пик {t(apex)} (кадр {apex}), br_w = {weight:+.2f}'
                        if began is not None else '; не показан')
                if began is not None:
                    shots.append((f'{i+1}. «{word}» {what}', max(0, began-4), apex, weight))
            key.append(row)
        moves = sum(a['move'] != 'skip' for a in voice['accents'])
        key.append(f'  итого: акцентов {len(voice["accents"])}, движений бровей {moves}, звук {dur/1000:.2f} с')
        key.append('')
    key.insert(0, f'Ключ клипа {NPC}: брови по тону голоса{" (слой выключен)" if off else ""}, реплик: {len(lines)}. '
                  f'Время — от начала клипа, {FPS} кадров/с; слово акцента — оценка по доле символов (≈); '
                  f'пт — полутоны; br_w — брови слоя: + вверх, − вниз.\n')
    if not off:
        (out/'key.txt').write_text('\n'.join(key))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        size = frame_at(video, 0, tmp/'size.png').size
        cmd = ['ffmpeg', '-v', 'error', '-y', '-i', str(video)]
        if wav.exists():
            cmd += ['-i', str(wav)]
        chain, prev = [], '0:v'
        for i, line in enumerate(lines):
            png = tmp/f'cap{i}.png'
            caption(f'Реплика {i+1}: {line["voice"]}' + (', слой бровей выключен' if off else ''), png, size)
            cmd += ['-i', str(png)]
            n = i + (2 if wav.exists() else 1)
            a, b = line['frame']/FPS, line['next']/FPS
            chain.append(f"[{prev}][{n}:v]overlay=0:0:enable='between(t,{a:.3f},{b:.3f})'[v{i}]")
            prev = f'v{i}'
        cmd += ['-filter_complex', ';'.join(chain) or '[0:v]null[v0]', '-map', f'[{prev if chain else "v0"}]']
        if wav.exists():
            cmd += ['-map', '1:a', '-c:a', 'aac', '-b:a', '192k', '-shortest']
        clip = out/('clip-off.mp4' if off else 'clip.mp4')
        cmd += ['-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-r', str(FPS), str(clip)]
        subprocess.run(cmd, check=True)
        if off:
            print(clip)
            return

        # the sheet: before | apex of the moves, the strongest first, at most 8, cropped to the face
        shots = sorted(shots, key=lambda s: -abs(s[3]))[:8]
        shots.sort(key=lambda s: s[1])
        tiles = []
        for label, rest, apex, w in shots:
            pair = []
            for n in (rest, apex):
                im = frame_at(video, n, tmp/f'f{n}.png')
                W, H = im.size
                pair.append(im.crop((W//4, 0, 3*W//4, H)))
            tiles.append((f'{label}, br_w {w:+.2f}', pair))
    if tiles:
        tw, th = tiles[0][1][0].size
        scale = min(1.0, 360/th)
        tw, th = int(tw*scale), int(th*scale)
        cols, head = 2, 28
        rowsn = (len(tiles)+cols-1)//cols
        sheet = Image.new('RGB', (cols*(2*tw+12)+(cols-1)*16, rowsn*(th+head+8)), (60, 60, 60))
        d = ImageDraw.Draw(sheet)
        for k, (label, pair) in enumerate(tiles):
            x0, y0 = (k % cols)*(2*tw+12+16), (k//cols)*(th+head+8)
            d.rectangle((x0, y0, x0+2*tw+12, y0+head), fill=BG)
            d.text((x0+8, y0+head//2), f'{label} | до · пик', fill=YELLOW, font=font(17), anchor='lm')
            for j, im in enumerate(pair):
                sheet.paste(im.resize((tw, th), Image.LANCZOS), (x0+j*(tw+12), y0+head))
        sheet.save(out/'sheet.jpg', quality=90)
    print('\n'.join(key))
    print(out/'clip.mp4', out/'key.txt', out/'sheet.jpg' if tiles else '(no sheet)', len(tiles), 'tiles')


if __name__ == '__main__':
    main()
