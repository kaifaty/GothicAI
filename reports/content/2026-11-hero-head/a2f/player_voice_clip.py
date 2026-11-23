#!/usr/bin/env python3
"""The clip of the hero's face under the player's live voice (player-voice-face.md, step 1).

  player_voice_clip.py <now run> <before run> <MPFB head.glb> <head.glb> <out dir> [--clock-ms X] [--extra text]
      [--key-only]   (key.txt alone: clip.mp4 and sheet.jpg are left as they are)

The runs are outputs of scripts/bench/player-voice-clip.bench: "now" with the server's player face, "before" with
--no-player-face (the mouth still). Each recording starts where the server played its first chunk (live_lag.py: by
the bridge's log of the "now" run, --clock-ms the game's system clock minus the server's). Writes into <out dir>:
  clip.mp4   "было | стало" side by side: the four phrases, then Xardas' voice in the microphone; last the same
             without ducking | with it (both "стало"). The sound is what the microphone heard (the recordings at the
             time the server played them), not shifted to the mouth;
  sheet.jpg  pairs of frames: «п», «б», «у», a loud vowel, a pause, Xardas' voice - "было | стало" at the time the
             mouth shows the moment (the voice's time plus the measured lag);
  key.txt    what is where, and the measures of live_lag.py on the "now" run.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent/'cues'))
from ingame_clip import font  # noqa: E402
from live_lag import measure, voice  # noqa: E402

LEAD = 0.7   # s of the recording before the first phrase of a part
TAIL = 1.2   # s after the last
RATE = 16000
# moments of the sheet: the phrase, its time in s, what is said there (by the syllables of the loudness)
# (min: the frame where the mouth is most shut within NEAR of it, as the mouth shows it; max: most open)
MOMENTS = [('phrase-4.wav', 0.90, '«п»: песик, | песик', min), ('phrase-2.wav', 2.07, '«б»: тебе', min),
           ('phrase-1.wav', 0.57, '«у»: думаю', max), ('phrase-3.wav', 0.50, '«у»: слушай', max)]
NEAR = 0.06  # s
YELLOW, BG = (255, 214, 120), (22, 22, 22)


def run(src):
    src = Path(src)
    ev = [json.loads(s) for s in (src/'events.jsonl').read_text().splitlines()]
    fps = {e['seg']: e['fps'] for e in ev if e['kind'] == 'rec'}
    hero = [e for e in ev if e['kind'] == 'hero']
    with open(src/'metrics.csv') as f:
        lip = {(r['seg'], int(r['frame'])): float(r['lip_open']) for r in csv.DictReader(f)}
    return {'src': src, 'fps': fps, 'hero': hero, 'lip': lip}


def pick(r, seg, n, how):
    """The frame within NEAR of frame n where the mouth is most shut (how=min) or open (max), None: n itself."""
    if how is None:
        return n
    k = round(NEAR*r['fps'][seg])
    near = [f for f in range(n-k, n+k+1) if (seg, f) in r['lip']]
    return how(near, key=lambda f: r['lip'][(seg, f)]) if near else n


def ptts(r, seg):
    return [e for e in r['hero'] if e['seg'] == seg]


def part(now, before, seg_l, seg_r, left, right, title, start, tmp, n, encode=True):
    """One part of the clip: two recordings side by side from LEAD before their first phrase, with the microphone.
    encode=False: only its length."""
    L, R = ptts(before if left == 'было' else now, seg_l), ptts(now, seg_r)
    src_l = (before if left == 'было' else now)['src']/f'{seg_l}.mp4'
    src_r = now['src']/f'{seg_r}.mp4'
    fps = now['fps'][seg_r]
    a_l, a_r = L[0]['frame']/fps-LEAD, R[0]['frame']/fps-LEAD
    dur = (R[-1]['frame']-R[0]['frame'])/fps+R[-1]['ms']/1000+LEAD+TAIL
    if not encode:
        return None, dur
    # the microphone: each recording from the time the server started to play it
    mic = np.zeros(int(dur*RATE))
    for e in R:
        with wave.open(e['wav']) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), np.int16)/32768
        at = int((e['frame']/fps-a_r+start[(seg_r, Path(e['wav']).name)]/1000)*RATE)
        mic[at:at+len(x)] += x[:max(0, len(mic)-at)]
    wav = tmp/f'mic{n}.wav'
    with wave.open(str(wav), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((np.clip(mic, -1, 1)*32767).astype(np.int16).tobytes())
    cap = tmp/f'cap{n}.png'
    im = Image.new('RGBA', (2560, 720), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = font(34)
    for x, text in ((40, left), (1320, right)):
        d.rectangle((x-12, 24, x+d.textlength(text, font=f)+12, 76), fill=(0, 0, 0, 150))
        d.text((x, 30), text, fill=(255, 255, 255, 255), font=f)
    d.rectangle((40-12, 650, 40+d.textlength(title, font=f)+12, 702), fill=(0, 0, 0, 150))
    d.text((40, 656), title, fill=YELLOW, font=f)
    im.save(cap)
    out = tmp/f'part{n}.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', f'{a_l:.3f}', '-t', f'{dur:.3f}', '-i', str(src_l),
                    '-ss', f'{a_r:.3f}', '-t', f'{dur:.3f}', '-i', str(src_r), '-i', str(cap), '-i', str(wav),
                    '-filter_complex', '[0:v]scale=1280:720,setsar=1[l];[1:v]scale=1280:720,setsar=1[r];'
                    '[l][r]hstack=inputs=2[s];[s][2:v]overlay=0:0[v]', '-map', '[v]', '-map', '3:a',
                    '-r', str(fps), '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                    '-b:a', '160k', '-shortest', str(out)], check=True)
    return out, dur


def frame_at(video, n, fps, path):
    """Frame n of the video, the frame of metrics row n. Exact by index (seeking by time lands a frame off now and
    then, a closing mouth shows open); fps: unused, kept for the callers."""
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(video), '-vf', rf'select=eq(n\,{n})', '-frames:v', '1',
                    str(path)], check=True)
    return Image.open(path).convert('RGB')


def frame(r, seg, wav, t, ms):
    """The frame of the recording `t` s into the voice of the recording `wav`, `ms` later."""
    e = next(e for e in ptts(r, seg) if Path(e['wav']).name == wav)
    return round(e['frame']+(t+ms/1000)*r['fps'][seg])


def main(argv):
    args = list(argv)
    opt = {}
    key_only = '--key-only' in args
    if key_only:
        args.remove('--key-only')
    for name in ('--clock-ms', '--extra'):
        if name in args:
            i = args.index(name)
            opt[name] = args[i+1]
            del args[i:i+2]
    if len(args) < 5:
        sys.exit(__doc__)
    now, before = run(args[0]), run(args[1])
    base, head, out = args[2], args[3], Path(args[4])
    clock = float(opt.get('--clock-ms', 0))
    out.mkdir(parents=True, exist_ok=True)
    report, lines = measure(args[0], base, head, ('voice', 'npc0', 'npc', 'own'), 0.0, clock)
    start = {(p['seg'], p['wav']): p['start_ms'] for p in report['phrases'] if 'wav' in p}
    offset = float(np.median(list(start.values())))
    _, lines_b = measure(args[1], base, head, ('voice', 'npc'), offset, clock)
    lag = {p['wav']: p['lag_ms'] for p in report['phrases'] if p.get('seg') == 'voice'}
    mean_lag = float(np.median(list(lag.values())))
    start[('npc0', 'npc-mix.wav')] = start.get(('npc0', 'npc-mix.wav'), offset)

    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        parts = [part(now, before, 'voice', 'voice', 'было', 'стало', 'Четыре фразы игрока (микрофон)', start, tmp, 0),
                 part(now, before, 'npc', 'npc', 'было', 'стало',
                      'Голос Ксардаса в микрофоне (на 12 дБ тише героя), с 5 с — фраза героя', start, tmp, 1),
                 part(now, before, 'npc0', 'npc', 'без приглушения', 'с приглушением',
                      'Голос Ксардаса в микрофоне: как было бы без приглушения', start, tmp, 2)] if not key_only else \
            [part(now, before, s, s, '', '', '', start, tmp, i, False) for i, s in enumerate(('voice', 'npc', 'npc'))]
        (tmp/'list.txt').write_text(''.join(f"file '{p}'\n" for p, _ in parts))
        if not key_only:
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(tmp/'list.txt'),
                            '-c', 'copy', str(out/'clip.mp4')], check=True)

        # the sheet
        shots = []
        for wav, at, what, how in MOMENTS:
            if wav in lag:
                shots.append((what+f' ({wav[:-4]}, {at:.2f} с)', 'voice', 'voice', wav, at, lag[wav], 'было', 'стало',
                              how))
        voiced = [(p['wav'], p) for p in report['phrases'] if p.get('seg') == 'voice']
        taken = [(w, a) for w, a, _, _ in MOMENTS]
        loud = max(((w, *_loudest(now, w, 0, 1e9, [a for x, a in taken if x == w])) for w, _ in voiced),
                   key=lambda v: v[2])
        shots.append((f'громкая гласная ({loud[0][:-4]}, {loud[1]:.2f} с)', 'voice', 'voice', loud[0], loud[1],
                      lag[loud[0]], 'было', 'стало', max))
        pauses = [(p['ms'], w, p) for w, ph in voiced for p in ph['pauses']]
        if pauses:
            ms, w, p = max(pauses, key=lambda v: v[0])
            at = (p['at_ms']+ms/2)/1000
            shots.append((f'пауза {ms} мс ({w[:-4]}, {at:.2f} с)', 'voice', 'voice', w, at, lag[w], 'было', 'стало',
                          None))
        xa = _loudest(now, 'npc-mix.wav', 0, 4.8, [])[0]
        shots.append((f'голос Ксардаса, герой молчит (npc-mix, {xa:.2f} с)', 'npc0', 'npc', 'npc-mix.wav', xa,
                      mean_lag, 'без приглушения', 'с приглушением', None))
        cells, key_moments = [], []
        for title, seg_l, seg_r, wav, at, lg, left, right, how in shots:
            src_l = now if left != 'было' else before
            ms = start[(seg_r, wav)]+lg
            fr = pick(now, seg_r, frame(now, seg_r, wav, at, ms), how)
            fl = frame(src_l, seg_l, wav, at, ms)+fr-frame(now, seg_r, wav, at, ms)
            if left != 'было':  # both "стало": the left one picks its own frame the same way
                fl = pick(src_l, seg_l, frame(src_l, seg_l, wav, at, ms), how)
            title += '; lip_open %s %.2f, %s %.2f' % (left, src_l['lip'].get((seg_l, fl), 0), right,
                                                    now['lip'].get((seg_r, fr), 0))
            key_moments.append(f'лист: {title} — {left} кадр {fl} ({seg_l}.mp4), {right} кадр {fr} ({seg_r}.mp4)')
            if key_only:
                continue
            il = frame_at(src_l['src']/f'{seg_l}.mp4', fl, src_l['fps'][seg_l], tmp/'l.png')
            ir = frame_at(now['src']/f'{seg_r}.mp4', fr, now['fps'][seg_r], tmp/'r.png')
            cells.append((title, left, right, il, ir))
        w, h = 640, 360
        crop = lambda im: im.crop((im.width//2-im.height*8//9, 0, im.width//2+im.height*8//9, im.height))  # noqa: E731
        sheet = Image.new('RGB', (2*w+30, len(cells)*(h+60)+20), BG)
        d = ImageDraw.Draw(sheet)
        f, fs = font(26), font(20)
        for i, (title, left, right, il, ir) in enumerate(cells):
            y = 10+i*(h+60)
            d.text((10, y), title, fill=YELLOW, font=f)
            for k, (im, lbl) in enumerate(((il, left), (ir, right))):
                x = 10+k*(w+10)
                sheet.paste(crop(im).resize((w, h)), (x, y+34))
                d.text((x+8, y+40), lbl, fill=(255, 255, 255), font=fs)
        if not key_only:
            sheet.save(out/'sheet.jpg', quality=90)

    key = ['Лицо героя под живой голос игрока (player-voice-face.md, шаг 1). Стенд player-voice-clip.bench на Ryzen, '
           'голова героя 3a с формами Mark (контент hero-head-3a-voice1, профиль mark.face.json), сервер без модели, '
           'ASR fake; фразы — запись микрофона пользователя, сервер играет их как микрофон в реальном темпе.', '',
           'clip.mp4, 30 кадров/с, слева «было» (сервер с --no-player-face: рот стоит), справа «стало»; звук — как его '
           'слышал микрофон, без сдвига к рту:']
    t = 0.0
    for (p, dur), what in zip(parts, ('четыре фразы', 'голос Ксардаса в микрофоне, «было | стало»',
                                      'голос Ксардаса в микрофоне, «без приглушения | с приглушением»')):
        key.append(f'  {t:6.2f} с — {what}, {dur:.2f} с')
        t += dur
    key += ['', *key_moments, '', f'Замер «стало» (live_lag.py, часы игры минус часы сервера {clock:.1f} мс):',
            *['  '+s for s in lines], '', f'Замер «было» (звук через {offset:.1f} мс после ptt, как у «стало»):',
            *['  '+s for s in lines_b]]
    if '--extra' in opt:
        key += ['', *Path(opt['--extra']).read_text().splitlines()]
    (out/'key.txt').write_text('\n'.join(key)+'\n')
    print('\n'.join(key))


def _loudest(r, wav, lo, hi, away):
    """The loudest 10 ms of the recording within [lo, hi) s, 0.25 s away from the times `away`: its time, dB."""
    e = next(e for e in r['hero'] if Path(e['wav']).name == wav)
    _, db = voice(e['wav'])
    s = np.convolve(db, np.ones(3)/3, 'same')
    t = np.arange(len(s))/100
    s[(t < lo) | (t >= hi)] = -200
    for a in away:
        s[np.abs(t-a) < 0.25] = -200
    i = int(np.argmax(s))
    return i/100, float(db[i])


if __name__ == '__main__':
    main(sys.argv[1:])
