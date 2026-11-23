#!/usr/bin/env python3
"""The head of one whose hands gesture as it speaks, before and after (gesture-head.md): two runs of
scripts/bench/gesture-head-clip.bench, "было" with OPENGOTHIC_GESTURE_HEAD=0 and "стало".

  clip.py <before out> <after out> <out dir>

Needs track-<take>.csv of track.py in both runs for the close takes. Writes into <out dir>:
  clip.mp4   "было | стало" side by side, take by take, the game's sound of the "after" run;
  steps.png  the face against the wall on the screen, px a frame, over time: both runs, a panel a close take;
  key.txt    the parts of the clip and the table: per take and run the screen (largest, 95th percentile, frames over
             the threshold of 3 px), the tilt of the head as animated and as shown (g_animroll, g_froll), its
             largest step of a frame, the gaze error, the gestures played and stopped.
"""
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent.parent/'cues'))
from ingame_clip import font  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from track import DEG, read  # noqa: E402

TAKES = [('g3', 'Жест 03'), ('g6', 'Жест 06: реплика кончается, жест останавливается'), ('g8', 'Жест 08'),
         ('g9', 'Жест 09: реплика кончается, жест останавливается'),
         ('wide', 'Общий план: остановка жеста 06'), ('npc', 'Ксардас, жест 06')]
LEAD, LIMIT = 0.5, 3.0
YELLOW = (255, 214, 120)


def load(src, seg):
    src = Path(src)
    with open(src/'metrics.csv') as f:
        rows = [r for r in csv.DictReader(f) if r['seg'] == seg]
    if not rows or not (src/f'{seg}.mp4').exists():
        return None
    ev = [e for e in map(json.loads, (src/'events.jsonl').read_text().splitlines()) if e.get('seg') == seg]
    said = [e for e in ev if e['kind'] == 'output']
    col = lambda c: np.array([float(r[c]) if r.get(c, '') != '' else np.nan for r in rows])  # noqa: E731
    out = {'src': src, 'seg': seg, 't': col('t')/1000, 'said': said, 'frames': len(rows),
           'fps': next((e['fps'] for e in ev if e['kind'] == 'rec'), 30),
           'animroll': col('g_animroll'), 'froll': col('g_froll'), 'err': col('g_err'), 'steady': col('g_steady'),
           'ge': [int(r['ge_id']) for r in rows], 'stops': int(rows[-1]['ge_stops'])-int(rows[0]['ge_stops'])}
    tr = src/f'track-{seg}.csv'
    if tr.exists():
        face, wall = read(tr)
        out['screen'] = np.hypot(face[:, 0], face[:, 1])
        out['wall'] = wall
    return out


def caption(tmp, n, title):
    cap = tmp/f'cap{n}.png'
    im = Image.new('RGBA', (2560, 720), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = font(34)
    for x, text in ((40, 'было'), (1320, 'стало')):
        d.rectangle((x-12, 24, x+d.textlength(text, font=f)+12, 76), fill=(0, 0, 0, 150))
        d.text((x, 30), text, fill=(255, 255, 255, 255), font=f)
    d.rectangle((28, 650, 52+d.textlength(title, font=f), 702), fill=(0, 0, 0, 150))
    d.text((40, 656), title, fill=YELLOW, font=f)
    im.save(cap)
    return cap


def part(b, a, tmp, n, title):
    """A take of both runs side by side from LEAD before the line starts to the end of the shorter recording."""
    fps = a['fps']
    t_l = max(0.0, (b['said'][0]['frame'] if b['said'] else 0)/fps-LEAD)
    t_r = max(0.0, (a['said'][0]['frame'] if a['said'] else 0)/fps-LEAD)
    dur = min(b['frames']/fps-t_l, a['frames']/fps-t_r)-0.05
    out = tmp/f'part{n}.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{t_l:.3f}', '-t', f'{dur:.3f}',
                    '-i', str(b['src']/f"{b['seg']}.mp4"), '-ss', f'{t_r:.3f}', '-t', f'{dur:.3f}',
                    '-i', str(a['src']/f"{a['seg']}.mp4"), '-i', str(caption(tmp, n, title)),
                    '-ss', f'{t_r:.3f}', '-t', f'{dur:.3f}', '-i', str(a['src']/f"{a['seg']}.wav"),
                    '-filter_complex', '[0:v]scale=1280:720,setsar=1[l];[1:v]scale=1280:720,setsar=1[r];'
                    '[l][r]hstack=inputs=2[s];[s][2:v]overlay=0:0[v]', '-map', '[v]', '-map', '3:a',
                    '-r', str(fps), '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                    '-ar', '48000', '-ac', '2', '-b:a', '160k', '-shortest', str(out)], check=True)
    return out, dur


def plot(pairs, out):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    pairs = [p for p in pairs if 'screen' in p[1] and 'screen' in p[2]]
    fig, axes = plt.subplots(len(pairs), 1, figsize=(12, 2.6*len(pairs)), constrained_layout=True, squeeze=False)
    for ax, (title, b, a) in zip(axes[:, 0], pairs):
        for name, r, color in (('было', b, '#9a9a9a'), ('стало', a, '#1f6fb4')):
            t0 = r['said'][0]['frame']/r['fps'] if r['said'] else 0
            ax.plot(np.arange(1, len(r['screen'])+1)/r['fps']-t0, r['screen'], color=color, lw=1.3, label=name)
        ax.axhline(LIMIT, color='#c0392b', lw=0.8, ls='--')
        ax.set_title(f'{title}: лицо против стены на экране, px за кадр (пунктир — порог {LIMIT:g} px)', fontsize=10)
        ax.set_xlabel('с от начала реплики')
        ax.grid(alpha=0.3)
        ax.legend(loc='upper right')
    fig.savefig(out, dpi=110)


def row(name, r):
    if r is None:
        return f'| {name} | нет записи |'
    step = np.nanmax(np.abs(np.diff(r['froll']))) if np.isfinite(r['froll']).any() else float('nan')
    ges = sorted({g for g in r['ge'] if g})
    scr = '— | — | — | —'
    if 'screen' in r:
        s = r['screen'][~np.isnan(r['screen'])]
        scr = (f'{s.max():.1f} ({s.max()*DEG:.2f}) | {np.percentile(s, 95):.1f} | {int((s > LIMIT).sum())} из {len(s)} | '
               f'{np.nanmax(r["wall"]):.1f}')
    return (f'| {name} | {scr} | {np.nanmin(r["animroll"]):+.1f}…{np.nanmax(r["animroll"]):+.1f} | '
            f'{np.nanmin(r["froll"]):+.1f}…{np.nanmax(r["froll"]):+.1f} | {step:.2f} | {np.nanmax(r["err"]):.1f} | '
            f'{", ".join(map(str, ges)) or "—"} | {r["stops"]} |')


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out = Path(argv[2])
    out.mkdir(parents=True, exist_ok=True)
    pairs = []
    for seg, title in TAKES:
        b, a = load(argv[0], seg), load(argv[1], seg)
        if b is not None and a is not None:
            pairs.append((title, b, a))
    key = ['Голова говорящего под жестами рук (gesture-head.md). Стенд gesture-head-clip.bench на Ryzen, 1280×720, '
           '30 кадров/с; кивок и покачивание голоса выключены.',
           f'«было» (OPENGOTHIC_GESTURE_HEAD=0) — {Path(argv[0])}', f'«стало» — {Path(argv[1])}', '',
           'clip.mp4: слева «было», справа «стало»; звук игры «стало».']
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        parts = [part(b, a, tmp, n, title) for n, (title, b, a) in enumerate(pairs)]
        (tmp/'list.txt').write_text(''.join(f"file '{p}'\n" for p, _ in parts))
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(tmp/'list.txt'),
                        '-c', 'copy', str(out/'clip.mp4')], check=True)
    at = 0.0
    for (title, _, _), (_, dur) in zip(pairs, parts):
        key.append(f'  {at:6.2f} с — {title}, {dur:.2f} с')
        at += dur
    plot(pairs, out/'steps.png')
    key += ['', f'Экран — движение лица против стены за кадр (track.py), порог {LIMIT:g} px; наклон набок — g_animroll '
            '(по клипу) и g_froll (на экране), градусы; взгляд — g_err, градусы.', '',
            '| дубль | экран: наиб., px (°) | p95, px | кадров выше порога | стена: наиб., px | наклон по клипу, ° | '
            'наклон на экране, ° | его шаг за кадр, ° | ошибка взгляда, ° | жесты | оборвано |',
            '|---|---|---|---|---|---|---|---|---|---|---|']
    for title, b, a in pairs:
        key.append(row(f'{b["seg"]} было', b))
        key.append(row(f'{a["seg"]} стало', a))
    (out/'key.txt').write_text('\n'.join(key)+'\n')
    print('\n'.join(key))


if __name__ == '__main__':
    main(sys.argv[1:])
