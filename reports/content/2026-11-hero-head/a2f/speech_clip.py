# The clip of the face from the voice in the game (scripts/bench/speech-face-clip.bench, engine-plan.md step 10):
#   uv run --with pillow python speech_clip.py <neutral run> <anger run> <off run> <tracks dir> <head.glb> <out dir>
# The runs are bench outputs of the same bench: the content with neutral tracks, with tracks of anger:0.8 for the two
# lines over S_ANGRY, and OPENGOTHIC_SPEECH_FACE=0. <tracks dir> holds neutral/ and anger/ <sound>.face.json.
# Writes into <out dir>:
#   clip.mp4      the neutral run, then the lines over S_ANGRY of the anger run ("стало"), "Реплика N" over each line;
#   clip-off.mp4  the off run ("было");
#   sheet.jpg     moments picked on the tracks - the lips shut (м, п, б), the most rounded lips (у), the apex of the
#                 brows - each "стало | было" at the same time of the voice;
#   key.txt       the lines, their tracks and emotions, where the moments are, the blinks, vf_w, the lip gap
#                 (lip_gap_game.py on each run's face.csv).
import csv
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent/'cues'))
import base64  # noqa: E402

from ingame_clip import FPS, caption, font, frame_at  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

GOTHIC_DIR = Path('/Volumes/DevSSD/Dev/games/Gothic II')
NPC = 'BAU_952_VINO'
ANGRY_LINES = ('DIA_VINO_HALLO_05_02', 'DIA_VINO_PERM_05_04')  # said over S_ANGRY
YELLOW, BG = (255, 214, 120), (22, 22, 22)


def texts():
    ou = GOTHIC_DIR/'_work/Data/Scripts/CONTENT/CUTSCENE/OU.BIN'
    data = ou.read_bytes() if ou.exists() else b''
    return {m[2].decode().upper(): m[1].decode('cp1251', 'replace') for m in re.finditer(
        rb'\x01..([^\x00-\x08\x0e-\x1f]{2,600}?)\x12\x06\x00\x00\x00\x01..([A-Za-z0-9_]+\.WAV)', data, re.S)}


def track(path):
    d = json.loads(Path(path).read_text())
    raw = base64.b64decode(d['data'])
    n = len(d['shapes'])
    frames = [[b/255 for b in raw[i:i+n]] for i in range(0, len(raw), n)]
    return {s: [f[k] for f in frames] for k, s in enumerate(d['shapes'])}, d.get('emotion', '')


def run(src):
    with open(src/'metrics.csv') as f:
        rows = {int(r['frame']): r for r in csv.DictReader(f) if r['seg'] == 'clip'}
    lines = [e for e in map(json.loads, (src/'events.jsonl').read_text().splitlines())
             if e['kind'] == 'output' and e['seg'] == 'clip' and e['npc'] == NPC]
    last = max(rows)
    for i, ln in enumerate(lines):
        ln['next'] = lines[i+1]['frame'] if i+1 < len(lines) else last+1
        ln['voiced'] = [f for f in range(ln['frame'], ln['next']) if f in rows and rows[f]['lip_on'] == '1']
    return dict(src=src, rows=rows, lines=lines)


def frame_of(r, line, ms):
    """The frame of the recording where the voice of the line has played `ms`."""
    return next((f for f in line['voiced'] if int(r['rows'][f]['lip_t']) >= ms), None)


def peaks(values, score, n, gap):
    """Up to n frames of the track with the highest score, at least `gap` frames apart."""
    order = sorted(range(len(values)), key=lambda i: -score(i))
    out = []
    for i in order:
        if score(i) <= 0 or any(abs(i-j) < gap for j in out):
            continue
        out.append(i)
        if len(out) == n:
            break
    return sorted(out)


def moments(sh, angry):
    """Frames of the track: lips shut (jaw nearly closed, the lips pressed or rolled), rounded lips, the brows' apex."""
    n = len(sh['jawOpen'])
    talk = [i for i in range(n) if max(sh['jawOpen'][max(0, i-6):i+7]) > 0.2]  # in the speech, not in a pause
    shut = lambda i: (i in talk and sh['jawOpen'][i] < 0.08) * (sh['mouthPressLeft'][i]+sh['mouthPressRight'][i] +  # noqa: E731
                                                               sh['mouthRollLower'][i]+sh['mouthClose'][i])
    round_ = lambda i: (i in talk) * (sh['mouthPucker'][i]+sh['mouthFunnel'][i])  # noqa: E731
    if angry:
        brow = lambda i: sh['browDownLeft'][i]+sh['browDownRight'][i]  # noqa: E731
    else:
        brow = lambda i: sh['browInnerUp'][i]+sh['browOuterUpLeft'][i]+sh['browOuterUpRight'][i]  # noqa: E731
    return [('губы сомкнуты (м/п/б)', i) for i in peaks(sh['jawOpen'], shut, 2, 9)] + \
           [('губы трубочкой (у)', i) for i in peaks(sh['jawOpen'], round_, 1, 9)] + \
           [('пик бровей' + (' вниз' if angry else ''), i) for i in peaks(sh['jawOpen'], brow, 1, 9)]


def blinks(r, line):
    rows = r['rows']
    fs = [f for f in range(line['frame'], line['next']) if f in rows]
    return int(rows[fs[-1]]['bk_n'])-int(rows[fs[0]]['bk_n']) if fs else 0


def labelled(video, wav, lines, out, tmp, labels):
    size = frame_at(video, 0, tmp/'size.png').size
    args = ['ffmpeg', '-v', 'error', '-y', '-i', str(video), '-i', str(wav)]
    chain, prev = [], '0:v'
    for i, (ln, text) in enumerate(zip(lines, labels)):
        png = tmp/f'cap{i}-{out.stem}.png'
        caption(text, png, size)
        args += ['-i', str(png)]
        a, b = ln['frame']/FPS, ln['next']/FPS
        chain.append(f"[{prev}][{i+2}:v]overlay=0:0:enable='between(t,{a:.3f},{b:.3f})'[v{i}]")
        prev = f'v{i}'
    args += ['-filter_complex', ';'.join(chain), '-map', f'[{prev}]', '-map', '1:a', '-c:a', 'aac', '-b:a', '192k',
             '-shortest', '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-r', str(FPS), str(out)]
    subprocess.run(args, check=True)


def cut(video, a, b, out):
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{a:.3f}', '-to', f'{b:.3f}', '-i', str(video),
                    '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', str(out)],
                   check=True)


def main():
    on, angry, off = (run(Path(p)) for p in sys.argv[1:4])
    tracks, head, out = Path(sys.argv[4]), sys.argv[5], Path(sys.argv[6])
    out.mkdir(parents=True, exist_ok=True)
    ou = texts()
    key = [f'Ключ клипа {NPC}: лицо целиком по звуку (Audio2Face v3.0, дорожки рядом с головой). Время — от начала '
           f'ролика, {FPS} кадров/с. clip.mp4 — «стало»: прогон с нейтральными дорожками, затем реплики 3–4 прогона с '
           f'дорожками anger:0.8; clip-off.mp4 — «было» (OPENGOTHIC_SPEECH_FACE=0). Реплики 3–4 идут поверх S_ANGRY '
           f'(faceanim): без эмоции в дорожке выражение ложится сверху, с эмоцией гаснет.', '']
    shots = []
    for take, r, label in ((on, on, 'нейтрально'), (angry, angry, 'anger:0.8')):
        for i, ln in enumerate(r['lines']):
            name = ln['name'].upper()
            over = name in ANGRY_LINES
            if take is angry and not over:
                continue
            sub = 'anger' if take is angry else 'neutral'
            sh, emo = track(next((tracks/sub).glob(f'{name}.face.json')))
            vf = [float(r['rows'][f]['vf_w']) for f in ln['voiced']]
            key.append(f'Реплика {i+1} ({ln["name"]}), прогон «{label}», {ln["frame"]/FPS:.2f} с'
                       + (' в прогоне' if take is angry else ''))
            key.append(f'  «{ou.get(name+".WAV", "?")}»')
            key.append(f'  дорожка: {len(sh["jawOpen"])} кадров, эмоция: {emo or "нет"}'
                       + ('; поверх S_ANGRY' if over else '')
                       + f'; vf_w: доля кадров голоса с полным слоем {sum(v >= 0.99 for v in vf)/max(len(vf), 1):.0%}'
                       + f'; S_ANGRY в середине реплики: {float(r["rows"][ln["voiced"][len(vf)//2]]["face_angry"]) if vf else 0:.2f}'
                       + f'; морганий: {blinks(r, ln)}')
            for what, k in moments(sh, take is angry):
                ms = round(k*1000/30)
                f_on = frame_of(r, ln, ms)
                ln_off = next((x for x in off['lines'] if x['name'].upper() == name), None)
                f_off = frame_of(off, ln_off, ms) if ln_off else None
                if f_on is None:
                    continue
                vals = {s: sh[s][k] for s in ('jawOpen', 'mouthClose', 'mouthPressLeft', 'mouthRollLower', 'mouthPucker',
                                             'mouthFunnel', 'browInnerUp', 'browDownLeft')}
                key.append(f'  {what}: {ms/1000:.2f} с звука, кадр прогона {f_on}'
                           + (f', было — кадр {f_off}' if f_off is not None else '')
                           + '; веса дорожки до правил: ' + ', '.join(f'{s} {v:.2f}' for s, v in vals.items() if v >= 0.05))
                shots.append((f'{i+1}{"*" if take is angry else ""}. {what}, {ms/1000:.2f} с', r, f_on, f_off))
            key.append('')
    key.append('* — прогон с дорожками anger:0.8.')
    key.append('')
    # the lip guard rail on the weights of each run
    for name, r in (('нейтрально', on), ('anger:0.8', angry), ('было', off)):
        res = subprocess.run([sys.executable, str(Path(__file__).parent/'lip_gap_game.py'), head, str(r['src']/'face.csv')],
                             capture_output=True, text=True)
        key.append(f'Провал губы, прогон «{name}»: ' + ' | '.join(res.stdout.strip().splitlines()[1:]))
    (out/'key.txt').write_text('\n'.join(key)+'\n')

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        caps = lambda r: [f'Реплика {i+1}' + (', злость 0.8 + S_ANGRY' if r is angry else ', поверх S_ANGRY' if  # noqa: E731
                          x['name'].upper() in ANGRY_LINES else '') for i, x in enumerate(r['lines'])]
        labelled(on['src']/'clip.mp4', on['src']/'clip.wav', on['lines'], tmp/'on.mp4', tmp, caps(on))
        labelled(angry['src']/'clip.mp4', angry['src']/'clip.wav', angry['lines'], tmp/'angry.mp4', tmp, caps(angry))
        labelled(off['src']/'clip.mp4', off['src']/'clip.wav', off['lines'], out/'clip-off.mp4', tmp,
                 [f'Было: реплика {i+1}' for i in range(len(off['lines']))])
        first = next(x for x in angry['lines'] if x['name'].upper() in ANGRY_LINES)
        cut(tmp/'angry.mp4', first['frame']/FPS, max(angry['rows'])/FPS, tmp/'angry-cut.mp4')
        (tmp/'list.txt').write_text(f"file '{tmp/'on.mp4'}'\nfile '{tmp/'angry-cut.mp4'}'\n")
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(tmp/'list.txt'), '-c:v',
                        'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', str(out/'clip.mp4')],
                       check=True)

        tiles = []
        for label, r, f_on, f_off in shots:
            pair = []
            for src, n in ((r['src'], f_on), (off['src'], f_off)):
                if n is None:
                    pair.append(Image.new('RGB', pair[0].size, BG))
                    continue
                im = frame_at(src/'clip.mp4', n, tmp/f'f{n}.png')
                W, H = im.size
                pair.append(im.crop((int(W*0.3), int(H*0.1), int(W*0.7), int(H*0.9))))
            w, h = pair[0].size
            tile = Image.new('RGB', (2*w, h+40), BG)
            tile.paste(pair[0], (0, 40))
            tile.paste(pair[1], (w, 40))
            ImageDraw.Draw(tile).text((8, 8), label + '  (стало | было)', fill=YELLOW, font=font(22))
            tiles.append(tile)
        cols = 3
        tw, th = tiles[0].size
        sheet = Image.new('RGB', (cols*tw, (len(tiles)+cols-1)//cols*th), BG)
        for i, tile in enumerate(tiles):
            sheet.paste(tile, (i % cols*tw, i//cols*th))
        sheet.save(out/'sheet.jpg', quality=88)


if __name__ == '__main__':
    main()
