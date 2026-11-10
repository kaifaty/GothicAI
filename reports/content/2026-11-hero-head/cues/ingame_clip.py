# The clip of the facial gestures and a lie in the game (scripts/bench/face-cues-clip.bench): from the bench's
# output makes clip.mp4 (its video and sound, "Реплика N" over each line), key.txt (for each line its text, the
# gestures with their words, start and apex, where the lie is, its felt mood and half of the face, the liar's
# blinks) and sheet.jpg (for each gesture and the leak the frame before it and the frame at its apex).
#   uv run --with pillow python ingame_clip.py <bench out dir> <out dir> [<segment, default clip>]
# The times come from what was shown: the columns cue_*, cue_leak, bk_w, bk_n of metrics.csv for the watched npc;
# the gestures of each line from its "[ai] cues" line in game.log, the line's start and length from events.jsonl.
import csv
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FPS = 30
SHOWN = 0.02      # a weight from which a gesture counts as begun
BLINK = 0.5       # R_EYESBLINK above it: the eyes shut
RU = dict(flash='вспышка бровей', doubt='сомнение', smirk='ухмылка', press='поджатые губы', disgust='брезгливость',
          squint='прищур', frown='хмурится', chin='подбородок', wince='поморщился')
FELT = dict(friendly='радость', angry='злость', hostile='неприязнь', afraid='страх', pain='боль')
HALF = dict(upper='верх лица', lower='низ лица')
MOOD = dict(S_NEUTRAL='neutral', S_FRIENDLY='friendly', S_ANGRY='angry', S_HOSTILE='hostile',
            S_FRIGHTENED='afraid', T_HURT='pain')
FONTS = ['/System/Library/Fonts/Supplemental/Arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf']
YELLOW, BG = (255, 214, 120), (22, 22, 22)


def font(size):
    for f in FONTS:
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def lines_of(src, seg):
    """The npc's lines in the recording, each with its gestures, leak and mood from game.log (in the same order:
    the bench's "npc line" goes out just before the bridge's "[ai] face" and "[ai] cues" of the same line)."""
    said = [e for e in map(json.loads, (src/'events.jsonl').read_text().splitlines())
            if e['kind'] == 'npc_line' and e['seg'] == seg and not e.get('filler')]
    out, current = [], None
    for row in (src/'game.log').read_text(errors='replace').splitlines():
        if m := re.search(r'\[bench\] npc line (\S+): (.*)$', row):
            current = dict(npc=m[1], text=m[2], cues=[], leak=None, mood='neutral')
            out.append(current)
        elif current and (m := re.search(r'\[ai\] face \S+ (\S+) for', row)):
            current['mood'] = MOOD.get(m[1], m[1])
        elif current and (m := re.search(r'\[ai\] cues \S+ (\S+):(.*)$', row)):
            current['id'] = m[1]
            for tok in m[2].split():
                if lm := re.fullmatch(r'leak:(\w+):(upper|lower)@(\d+)', tok):
                    current['leak'] = dict(felt=lm[1], half=lm[2], word=int(lm[3]))
                elif cm := re.fullmatch(r'(\w+)@(\d+)([LR]?)', tok):
                    current['cues'].append(dict(name=cm[1], word=int(cm[2]), side=cm[3]))
            current['posed'] = 'posed' in m[2].split()
    # the last lines of the log that were recorded (a line said before the recording has no frame)
    out = out[-len(said):] if said else []
    for line, e in zip(out, said):
        assert line['text'] == e['text'], (line['text'], e['text'])
        line['frame'], line['ms'] = e['frame'], e['ms']
    return out


def metrics(src, seg):
    with open(src/'metrics.csv') as f:
        rows = [r for r in csv.DictReader(f) if r['seg'] == seg]
    return {int(r['frame']): r for r in rows}


def course(rows, column, lo, hi):
    """The first frame of [lo, hi) where the column is shown, its apex frame and weight."""
    frames = [f for f in range(lo, hi) if f in rows]
    shown = [f for f in frames if float(rows[f][column]) > SHOWN]
    if not shown:
        return None
    apex = max(frames, key=lambda f: float(rows[f][column]))
    return dict(start=shown[0], apex=apex, weight=float(rows[apex][column]))


def blinks(rows, lo, hi):
    """Frames where the eyes shut in [lo, hi); ordinary (EyeBlink counted it within 6 frames) or the liar's."""
    out = []
    for f in range(max(lo, 1), hi):
        if f in rows and f-1 in rows and float(rows[f]['bk_w']) >= BLINK > float(rows[f-1]['bk_w']):
            near = [int(rows[g]['bk_n']) for g in range(f-6, f+2) if g in rows]
            out.append(dict(frame=f, liar=not near or near[-1] == near[0]))
    return out


def t(frame):
    return f'{frame/FPS:5.2f} с'


def caption(text, path, size):
    w, h = size
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = font(int(h*0.045))
    box = d.textbbox((0, 0), text, font=f)
    x, y = int(w*0.03), int(h*0.04)
    d.rectangle((x-10, y-6, x+box[2]-box[0]+10, y+box[3]-box[1]+12), fill=(0, 0, 0, 140))
    d.text((x, y-box[1]), text, fill=(255, 255, 255, 255), font=f)
    im.save(path)


def frame_at(video, n, path):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(video), '-vf', rf'select=eq(n\,{n})', '-vframes', '1',
                    str(path)], check=True)
    return Image.open(path).convert('RGB')


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    seg = sys.argv[3] if len(sys.argv) > 3 else 'clip'
    out.mkdir(parents=True, exist_ok=True)
    video, wav = src/f'{seg}.mp4', src/f'{seg}.wav'
    rows = metrics(src, seg)
    lines = lines_of(src, seg)
    last = max(rows)
    key, shots = [], []
    for i, line in enumerate(lines):
        f0 = line['frame']
        f1 = f0 + round(line['ms']*FPS/1000)
        nxt = lines[i+1]['frame'] if i+1 < len(lines) else last+1
        line['end'], line['next'] = f1, nxt
        words = line['text'].split()
        key.append(f'Реплика {i+1} ({line["id"] if "id" in line else "?"}), {t(f0)} – {t(f1)}, '
                   f'настроение {line["mood"]}{", наигранная улыбка" if line.get("posed") else ""}')
        key.append(f'  «{line["text"]}»')
        for c in line['cues']:
            col = f'cue_{c["name"]}'
            got = course(rows, col, max(0, f0-6), nxt) if col in next(iter(rows.values())) else None
            word = words[c['word']] if c['word'] < len(words) else '?'
            name = RU.get(c['name'], c['name']) + (f' {c["side"]}' if c['side'] else '')
            if got is None:
                key.append(f'  жест {name} [{c["name"]}] у слова «{word}» (№{c["word"]}): не показан')
                continue
            key.append(f'  жест {name} [{c["name"]}] у слова «{word}» (№{c["word"]}): начало {t(got["start"])}, '
                       f'пик {t(got["apex"])} (кадр {got["apex"]}), {col} = {got["weight"]:.2f}')
            shots.append((f'{i+1}. {name}: «{word}»', max(0, got['start']-3), got['apex'], col, got['weight']))
        if (lk := line['leak']) is not None:
            got = course(rows, 'cue_leak', max(0, f0-6), nxt)
            word = words[lk['word']] if lk['word'] < len(words) else '?'
            felt, half = FELT.get(lk['felt'], lk['felt']), HALF[lk['half']]
            key.append(f'  ЛОЖЬ: прорыв чувства «{felt}» ({lk["felt"]}), {half}, у слова «{word}» (№{lk["word"]})'
                       + (f': начало {t(got["start"])}, пик {t(got["apex"])} (кадр {got["apex"]}), '
                          f'cue_leak = {got["weight"]:.2f}' if got else ': не показан'))
            if got:
                shots.append((f'{i+1}. прорыв: {felt}, {half}', max(0, got['start']-3), got['apex'], 'cue_leak',
                              got['weight']))
            liar = [b for b in blinks(rows, f0, nxt) if b['liar']]
            key.append('  моргания лжеца: ' + (', '.join(f'{t(b["frame"])} (кадр {b["frame"]})' for b in liar)
                                               or 'не найдены'))
        else:
            key.append('  правда, без прорыва')
        ordinary = [b for b in blinks(rows, f0, nxt) if not b['liar']]
        if ordinary:
            key.append('  обычные моргания: ' + ', '.join(t(b['frame']) for b in ordinary))
        key.append('')
    gestures = sorted({c['name'] for line in lines for c in line['cues']})
    lies = [i+1 for i, line in enumerate(lines) if line['leak']]
    key.insert(0, f'Ключ клипа: {len(lines)} реплик {lines[0]["npc"] if lines else ""}, жесты: {", ".join(gestures)}; '
               f'ложь — реплика {", ".join(map(str, lies)) or "нет"}. Время — от начала клипа, {FPS} кадров/с.\n')
    (out/'key.txt').write_text('\n'.join(key))

    # the clip: the video, its sound, the number of the line from its start to the next one
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        size = frame_at(video, 0, tmp/'size.png').size
        args = ['ffmpeg', '-v', 'error', '-y', '-i', str(video)]
        if wav.exists():
            args += ['-i', str(wav)]
        chain, prev = [], '0:v'
        for i, line in enumerate(lines):
            png = tmp/f'cap{i}.png'
            caption(f'Реплика {i+1}', png, size)
            args += ['-i', str(png)]
            n = i + (2 if wav.exists() else 1)
            a, b = line['frame']/FPS, line['next']/FPS
            chain.append(f"[{prev}][{n}:v]overlay=0:0:enable='between(t,{a:.3f},{b:.3f})'[v{i}]")
            prev = f'v{i}'
        args += ['-filter_complex', ';'.join(chain) or '[0:v]null[v0]', '-map', f'[{prev if chain else "v0"}]']
        if wav.exists():
            args += ['-map', '1:a', '-c:a', 'aac', '-b:a', '192k', '-shortest']
        args += ['-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-r', str(FPS), str(out/'clip.mp4')]
        subprocess.run(args, check=True)

        # the sheet: before | apex for each gesture and the leak, cropped to the middle of the frame
        tiles = []
        for label, rest, apex, col, w in shots:
            pair = []
            for n in (rest, apex):
                im = frame_at(video, n, tmp/f'f{n}.png')
                W, H = im.size
                pair.append(im.crop((W//4, 0, 3*W//4, H)))
            tiles.append((f'{label} — {col} {w:.2f}', pair))
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
