#!/usr/bin/env python3
"""The hero's nod while he speaks, before and after (player-voice-face.md, "Кивок головы"): the same bench
scripts/bench/player-voice-clip.bench, its segments "voice" (the four phrases of the microphone) and "own" (two lines
of the hero's recorded voice).

  nod_clip.py <before run> <after run> <out dir> [--start-ms X]

Writes into <out dir>:
  clip.mp4  "было | стало" side by side: the four phrases with the microphone's sound (each phrase from the time the
            server played its first chunk: --start-ms after the bench's `ptt`, default 5), then the two lines with
            the game's sound of the "after" run;
  nod.png   ge_pitch (degrees) over time, before and after, on phrase 1 and on the first line of the hero's voice,
            with the syllables the mouth found (lip_picks) and the nods (ge_nods);
  key.txt   per run and segment: nods, the largest step of a frame, the first frame of a nod (from the last still
            frame), the size of the nods, the time down (from that frame to the apex) and back (to within 0.1 degree).
"""
import csv
import json
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent.parent/'cues'))
from ingame_clip import font  # noqa: E402

LEAD, TAIL, RATE = 0.7, 1.2, 16000
YELLOW = (255, 214, 120)


def load(src, seg):
    src = Path(src)
    with open(src/'metrics.csv') as f:
        rows = [r for r in csv.DictReader(f) if r['seg'] == seg]
    ev = [json.loads(s) for s in (src/'events.jsonl').read_text().splitlines()]
    ev = [e for e in ev if e['seg'] == seg]
    said = [e for e in ev if e['kind'] == 'hero' or (e['kind'] == 'output' and e['npc'] == 'PC_HERO')]
    return {'src': src, 'seg': seg, 'frame': np.array([int(r['frame']) for r in rows]),
            't': np.array([float(r['t']) for r in rows]), 'pitch': np.array([float(r['ge_pitch']) for r in rows]),
            'nods': np.array([int(r['ge_nods']) for r in rows]), 'picks': np.array([int(r['lip_picks']) for r in rows]),
            'said': said, 'fps': next(e['fps'] for e in ev if e['kind'] == 'rec')}


def nods(r):
    """Each nod: its frame, the apex frame, its size (degrees), ms down and back (to within 0.1 degree of where it
    started, or the next nod)."""
    p, t = r['pitch'], r['t']
    starts = [i for i in range(1, len(p)) if r['nods'][i] > r['nods'][i-1]]
    out = []
    for k, i in enumerate(starts):
        end = starts[k+1] if k+1 < len(starts) else len(p)
        rest = p[i-1]
        # the frame the counter goes up on may still show the head where it was: the move starts after it
        j = next((j for j in range(i, end) if p[j] < rest-0.001), i)
        a = i+int(np.argmin(p[i:end]))
        back = next((x for x in range(a, end) if p[x] > rest-0.1), end-1)
        out.append({'at': float(t[i]), 'size': float(rest-p[a]), 'down': float(t[a]-t[j-1]),
                    'back': float(t[back]-t[a]), 'first': float(rest-p[j])})
    return out


def measure(r):
    d = np.abs(np.diff(r['pitch']))
    n = nods(r)
    gaps = np.diff([x['at'] for x in n]) if len(n) > 1 else np.array([0.0])
    return {'nods': len(n), 'step_max': float(d.max()) if len(d) else 0.0,
            'first_max': max((x['first'] for x in n), default=0.0),
            'size': (min((x['size'] for x in n), default=0.0), max((x['size'] for x in n), default=0.0)),
            'down': (min((x['down'] for x in n), default=0.0), max((x['down'] for x in n), default=0.0)),
            'back': (min((x['back'] for x in n), default=0.0), max((x['back'] for x in n), default=0.0)),
            'gap_min': float(gaps.min()), 'list': n}


def caption(tmp, n, left, right, title):
    cap = tmp/f'cap{n}.png'
    im = Image.new('RGBA', (2560, 720), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = font(34)
    for x, text in ((40, left), (1320, right)):
        d.rectangle((x-12, 24, x+d.textlength(text, font=f)+12, 76), fill=(0, 0, 0, 150))
        d.text((x, 30), text, fill=(255, 255, 255, 255), font=f)
    d.rectangle((28, 650, 52+d.textlength(title, font=f), 702), fill=(0, 0, 0, 150))
    d.text((40, 656), title, fill=YELLOW, font=f)
    im.save(cap)
    return cap


def part(b, a, tmp, n, title, start_ms):
    """The segment of both runs side by side from LEAD before the first thing said; the sound: the microphone
    (phrases) or the game's sound of the after run (the hero's recorded voice)."""
    fps = a['fps']
    t_l, t_r = b['said'][0]['frame']/fps-LEAD, a['said'][0]['frame']/fps-LEAD
    last = a['said'][-1]
    dur = (last['frame']-a['said'][0]['frame'])/fps+last['ms']/1000+LEAD+TAIL
    sound = tmp/f'sound{n}.wav'
    if a['said'][0]['kind'] == 'hero':
        mic = np.zeros(int(dur*RATE))
        for e in a['said']:
            with wave.open(e['wav']) as w:
                x = np.frombuffer(w.readframes(w.getnframes()), np.int16)/32768
            at = int((e['frame']/fps-t_r+start_ms/1000)*RATE)
            mic[at:at+len(x)] += x[:max(0, len(mic)-at)]
        with wave.open(str(sound), 'wb') as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes((np.clip(mic, -1, 1)*32767).astype(np.int16).tobytes())
        audio = ['-i', str(sound)]
    else:
        audio = ['-ss', f'{t_r:.3f}', '-t', f'{dur:.3f}', '-i', str(a['src']/f"{a['seg']}.wav")]
    out = tmp/f'part{n}.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{t_l:.3f}', '-t', f'{dur:.3f}',
                    '-i', str(b['src']/f"{b['seg']}.mp4"), '-ss', f'{t_r:.3f}', '-t', f'{dur:.3f}',
                    '-i', str(a['src']/f"{a['seg']}.mp4"), '-i', str(caption(tmp, n, 'было', 'стало', title)), *audio,
                    '-filter_complex', '[0:v]scale=1280:720,setsar=1[l];[1:v]scale=1280:720,setsar=1[r];'
                    '[l][r]hstack=inputs=2[s];[s][2:v]overlay=0:0[v]', '-map', '[v]', '-map', '3:a',
                    '-r', str(fps), '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                    '-ar', '48000', '-ac', '2', '-b:a', '160k', '-shortest', str(out)], check=True)
    return out, dur


def plot(runs, out):
    """ge_pitch over time: a panel per (what, segment), before and after; the syllables as ticks, the nods as dots."""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 1, figsize=(12, 6.5), constrained_layout=True)
    for ax, (title, seg, k) in zip(axes, (('фраза 1 (живой голос)', 'voice', 0),
                                         ('реплика героя 1 (записанный голос)', 'own', 0))):
        for name, r, color in (('было', runs[0][seg], '#9a9a9a'), ('стало', runs[1][seg], '#1f6fb4')):
            e = r['said'][k]
            t0 = e['t']-next(x['t'] for x in json.loads('[' + ','.join(
                (r['src']/'events.jsonl').read_text().splitlines()) + ']') if x['kind'] == 'rec' and x['seg'] == seg)
            span = (r['t'] >= t0-300) & (r['t'] <= t0+e['ms']+600)
            tt = (r['t'][span]-t0)/1000
            ax.plot(tt, r['pitch'][span], color=color, lw=1.6, marker='.', ms=3, label=name)
            nod = np.nonzero(np.diff(r['nods'][span]) > 0)[0]+1
            ax.plot(tt[nod], r['pitch'][span][nod], 'o', color=color, ms=6, mfc='none')
            if name == 'стало':
                syl = np.nonzero(np.diff(r['picks'][span]) > 0)[0]+1
                for s in tt[syl]:
                    ax.axvline(s, color='#d4a017', lw=0.6, alpha=0.6)
        ax.set_title(f'{title}: ge_pitch, градусы (вниз < 0); жёлтые линии — слоги «стало», кружки — начала кивков',
                     fontsize=10)
        ax.set_xlabel('с от начала звука')
        ax.grid(alpha=0.3)
        ax.legend(loc='lower right')
    fig.savefig(out, dpi=110)


def main(argv):
    args = list(argv)
    start_ms = 5.0
    if '--start-ms' in args:
        i = args.index('--start-ms')
        start_ms = float(args[i+1])
        del args[i:i+2]
    if len(args) < 3:
        sys.exit(__doc__)
    out = Path(args[2])
    out.mkdir(parents=True, exist_ok=True)
    runs = [{seg: load(args[k], seg) for seg in ('voice', 'own')} for k in (0, 1)]
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        parts = [part(runs[0]['voice'], runs[1]['voice'], tmp, 0, 'Четыре фразы игрока (живой голос)', start_ms),
                 part(runs[0]['own'], runs[1]['own'], tmp, 1, 'Две реплики героя (записанный голос)', start_ms)]
        (tmp/'list.txt').write_text(''.join(f"file '{p}'\n" for p, _ in parts))
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(tmp/'list.txt'),
                        '-c', 'copy', str(out/'clip.mp4')], check=True)
    plot(runs, out/'nod.png')
    key = ['Кивок головы героя, пока он говорит (player-voice-face.md, «Кивок головы»). Стенд player-voice-clip.bench '
           'на Ryzen, 30 кадров/с; «было» — ' + str(Path(args[0])) + ', «стало» — ' + str(Path(args[1])) + '.', '',
           'clip.mp4: слева «было», справа «стало».']
    t = 0.0
    for (p, dur), what in zip(parts, ('четыре фразы, звук микрофона', 'две реплики героя, звук игры «стало»')):
        key.append(f'  {t:6.2f} с — {what}, {dur:.2f} с')
        t += dur
    key += ['', 'Замер по ge_pitch (градусы): кивки (ge_nods), наибольший шаг за кадр, наибольший первый кадр кивка, '
            'размах, вниз и назад (мс), наименьший промежуток между кивками (мс):']
    for name, r in zip(('было', 'стало'), runs):
        for seg in ('voice', 'own'):
            m = measure(r[seg])
            key.append(f'  {name:5s} {seg:5s}: кивков {m["nods"]:2d}; шаг за кадр до {m["step_max"]:.2f}; первый кадр '
                       f'до {m["first_max"]:.2f}; размах {m["size"][0]:.2f}–{m["size"][1]:.2f}; вниз '
                       f'{m["down"][0]:.0f}–{m["down"][1]:.0f}; назад {m["back"][0]:.0f}–{m["back"][1]:.0f}; '
                       f'промежуток от {m["gap_min"]:.0f}')
            key.append('         кивки, мс записи: ' + ', '.join(f'{x["at"]:.0f} ({x["size"]:.1f}°)' for x in m['list']))
    (out/'key.txt').write_text('\n'.join(key)+'\n')
    print('\n'.join(key))


if __name__ == '__main__':
    main(sys.argv[1:])
