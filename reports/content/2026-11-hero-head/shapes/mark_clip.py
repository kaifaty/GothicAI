# The in-game comparison of face-shapes.md, "Формы Mark в игре": MPFB's shapes (the clip speech-face-1) and Mark's on
# the same bench, lines and tracks, side by side; the hero's gestures and moods the same way.
#   uv run --with pillow --with numpy python mark_clip.py <MPFB clip.mp4> <MPFB neutral run> <MPFB anger run> \
#       <Mark clip.mp4> <Mark neutral run> <Mark anger run> <tracks dir> <cues MPFB> <cues Mark> <head MPFB> \
#       <head Mark> <out dir>
# The clips are a2f/speech_clip.py's of speech-face-clip.bench (the neutral take, then lines 3-4 of the anger take);
# the runs are its bench outputs (metrics.csv, events.jsonl). cues and head are outputs of face-cues.bench and
# hero-head.bench on the hero's head with each set. Writes into <out dir>:
#   clip.mp4   per line the two clips from the start of the voice, cut to the shorter, face close up, MPFB | Mark,
#              the sound of Mark's take;
#   sheet.jpg  pairs MPFB | Mark: per line the moments of a2f/speech_clip.py on the tracks (lips shut on м/п/б, rounded
#              on у, the apex of the brows, up or down with anger), the moods of hero-head.bench at their middle, the
#              apex of every gesture of face-cues.bench;
#   key.txt    where each tile and segment comes from.
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent/'a2f'))
sys.path.insert(0, str(HERE.parent/'cues'))
from speech_clip import ANGRY_LINES, FPS, frame_at, frame_of, moments, run, track  # noqa: E402
from engine_sheet import ORDER, RU  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

FONT = '/System/Library/Fonts/Supplemental/Arial.ttf'
YELLOW, BG = (255, 214, 120), (22, 22, 22)
FACE = (0.3, 0.05, 0.7, 0.95)          # the face in speech-face-clip.bench's frame
HERO = (0.36, 0.06, 0.64, 0.78)        # the hero's face in face-cues.bench and hero-head.bench


def duration(video):
    return float(subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0',
                                 str(video)], capture_output=True, text=True, check=True).stdout)


def timeline(clip, on, angry):
    """Clip time of a run frame: the clip is the neutral take, then the anger take from its first line over S_ANGRY."""
    first = next(x for x in angry['lines'] if x['name'].upper() in ANGRY_LINES)['frame']
    cut = (max(angry['rows'])-first)/FPS
    on_len = duration(clip)-cut
    return lambda take, f: f/FPS if take == 'on' else on_len+(f-first)/FPS


def segments(on, angry):
    """(take, line) in the order of the clip."""
    return [('on', ln) for ln in on['lines']]+[('angry', ln) for ln in angry['lines'] if ln['name'].upper() in ANGRY_LINES]


def crop(im, box):
    w, h = im.size
    return im.crop((int(box[0]*w), int(box[1]*h), int(box[2]*w), int(box[3]*h)))


def tile(label, a, b, height=420):
    s = height/a.height
    a, b = (x.resize((int(x.width*s), height)) for x in (a, b))
    t = Image.new('RGB', (a.width+b.width+6, height+34), BG)
    t.paste(a, (0, 34))
    t.paste(b, (a.width+6, 34))
    ImageDraw.Draw(t).text((6, 6), label, fill=YELLOW, font=ImageFont.truetype(FONT, 18))
    return t


def main(a):
    mp_clip, mp_on, mp_angry, mk_clip, mk_on, mk_angry = a[0], run(Path(a[1])), run(Path(a[2])), a[3], run(Path(a[4])), \
        run(Path(a[5]))
    tracks, cues, heads, out = Path(a[6]), (Path(a[7]), Path(a[8])), (Path(a[9]), Path(a[10])), Path(a[11])
    out.mkdir(parents=True, exist_ok=True)
    sets = [('MPFB', mp_clip, mp_on, mp_angry), ('Mark', mk_clip, mk_on, mk_angry)]
    tl = [timeline(c, on, an) for _, c, on, an in sets]
    key = ['Ключ сравнения «MPFB | Mark» (face-shapes.md, «Формы Mark в игре»). Слева формы MPFB с поправкой губ '
           '(клип speech-face-1), справа формы Mark, профиль Mark; тот же стенд speech-face-clip.bench, те же реплики '
           'и дорожки Audio2Face. Время — от начала clip.mp4, 30 кадров/с.', '']
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        # 1. the clip, line by line from the start of each voice
        parts, t = [], 0.0
        for i, (take, _) in enumerate(segments(mp_on, mp_angry)):
            spans = []
            for k, (_, clip, on, an) in enumerate(sets):
                ln = segments(on, an)[i][1]
                spans.append((tl[k](take, ln['frame']), tl[k](take, ln['next'])))
            d_ = min(b-a for a, b in spans)
            name = segments(mk_on, mk_angry)[i][1]['name']
            label = f'{name}' + (', anger 0.8 + S_ANGRY' if take == 'angry' else ', поверх S_ANGRY' if
                                 name.upper() in ANGRY_LINES else '')
            seg = tmp/f's{i}.mp4'
            cx = f'crop=iw*{FACE[2]-FACE[0]:.3f}:ih*{FACE[3]-FACE[1]:.3f}:iw*{FACE[0]}:ih*{FACE[1]},scale=512:648'
            strip = tmp/f'h{i}.png'                    # this ffmpeg has no drawtext: the captions are a picture
            im = Image.new('RGB', (1024, 64), BG)
            d = ImageDraw.Draw(im)
            d.text((10, 6), label, fill=(255, 255, 255), font=ImageFont.truetype(FONT, 20))
            d.text((10, 36), 'MPFB, с поправкой губ (сейчас)', fill=YELLOW, font=ImageFont.truetype(FONT, 20))
            d.text((522, 36), 'Mark', fill=YELLOW, font=ImageFont.truetype(FONT, 20))
            im.save(strip)
            subprocess.run([
                'ffmpeg', '-v', 'error', '-y', '-ss', f'{spans[0][0]:.3f}', '-t', f'{d_:.3f}', '-i', str(mp_clip),
                '-ss', f'{spans[1][0]:.3f}', '-t', f'{d_:.3f}', '-i', str(mk_clip), '-loop', '1', '-i', str(strip),
                '-filter_complex', f'[0:v]{cx}[l];[1:v]{cx}[r];[l][r]hstack=inputs=2[f];[2:v][f]vstack=shortest=1[v]',
                '-map', '[v]', '-map', '1:a', '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-r', str(FPS),
                '-c:a', 'aac', '-b:a', '192k', '-ar', '48000', '-t', f'{d_:.3f}', str(seg)], check=True)
            parts.append(seg)
            key.append(f'{t:6.2f} с — {label}: MPFB с {spans[0][0]:.2f} с своего клипа, Mark с {spans[1][0]:.2f} с, '
                       f'{d_:.2f} с')
            t += d_
        (tmp/'list.txt').write_text(''.join(f"file '{p}'\n" for p in parts))
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(tmp/'list.txt'),
                        '-c', 'copy', str(out/'clip.mp4')], check=True)
        key.append('')
        # 2. the sheet
        tiles = []
        for i, (take, ln_mk) in enumerate(segments(mk_on, mk_angry)):
            name = ln_mk['name'].upper()
            sh, emo = track(next((tracks/('anger' if take == 'angry' else 'neutral')).glob(f'{name}.face.json')))
            for what, k in moments(sh, take == 'angry'):
                ms = round(k*1000/30)
                ims = []
                for s, (_, clip, on, an) in enumerate(sets):
                    r = an if take == 'angry' else on
                    ln = segments(on, an)[i][1]
                    f = frame_of(r, ln, ms)
                    if f is None:
                        break
                    n = round(tl[s](take, f)*FPS)
                    ims.append(crop(frame_at(clip, n, tmp/f'{s}-{n}.png'), FACE))
                if len(ims) < 2:
                    continue
                lab = f'{name}{" (anger 0.8)" if take == "angry" else ""}: {what}, {ms/1000:.2f} с'
                tiles.append(tile(lab, *ims))
                key.append(f'лист: {lab}')
        for mood in ('angry', 'hostile', 'frightened', 'friendly', 'hurt', 'quiet', 'lookdown', 'lookup'):
            ims = []
            for d in heads:
                v = d/f'{mood}.mp4'
                if not v.exists():
                    break
                n = int(duration(v)*FPS*0.6)
                ims.append(crop(frame_at(v, n, tmp/f'{d.name}-{mood}.png'), HERO))
            if len(ims) == 2:
                tiles.append(tile(f'герой: {mood} (hero-head.bench, 60 % записи)', *ims))
                key.append(f'лист: герой, {mood}')
        for g in ORDER:
            ims = [crop(Image.open(d/f'{g}-apex.png').convert('RGB'), HERO) for d in cues if (d/f'{g}-apex.png').exists()]
            if len(ims) == 2:
                tiles.append(tile(f'жест: {RU.get(g, g)} (пик)', *ims))
                key.append(f'лист: жест {g}')
    cols = 3
    w = max(x.width for x in tiles)
    h = max(x.height for x in tiles)
    rows = (len(tiles)+cols-1)//cols
    sheet = Image.new('RGB', (cols*w, rows*h+40), BG)
    ImageDraw.Draw(sheet).text((8, 8), 'Каждая пара: слева MPFB (сейчас), справа Mark', fill=(255, 255, 255),
                               font=ImageFont.truetype(FONT, 22))
    for k, x in enumerate(tiles):
        sheet.paste(x, ((k % cols)*w, 40+(k//cols)*h))
    sheet.save(out/'sheet.jpg', quality=86)
    (out/'key.txt').write_text('\n'.join(key)+'\n')
    print(out/'clip.mp4', out/'sheet.jpg', len(tiles), 'tiles')


if __name__ == '__main__':
    main(sys.argv[1:])
