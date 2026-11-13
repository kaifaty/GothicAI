#!/usr/bin/env python3
"""The clip of the hero's face under the player's live voice (player-voice-face.md, step 1).

  player_voice_clip.py <now run> <before run> <MPFB head.glb> <head.glb> <out dir> [--offset-ms X] [--extra text]

The runs are outputs of scripts/bench/player-voice-clip.bench: "now" with the server's player face, "before" with
--no-player-face (the mouth still). --offset-ms: the game -> server trip of ptt_start (the server's microphone starts
that much after the bench's `ptt`), measured outside. Writes into <out dir>:
  clip.mp4   "было | стало" side by side: the four phrases, then Xardas' voice in the microphone; last the same
             without ducking | with it (both "стало"). The sound is what the microphone heard (the recordings at the
             time the server played them), not shifted to the mouth;
  sheet.jpg  pairs of frames: «п», «б», «у», a loud vowel, a pause, Xardas' voice - "было | стало" at the time the
             mouth shows the moment (the voice's time plus the measured lag);
  key.txt    what is where, and the measures of live_lag.py on the "now" run.
"""
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
from ingame_clip import font, frame_at  # noqa: E402
from live_lag import measure, voice  # noqa: E402

LEAD = 0.7   # s of the recording before the first phrase of a part
TAIL = 1.2   # s after the last
RATE = 16000
# moments of the sheet: the phrase, its time in s, what is said there (by the syllables of the loudness)
MOMENTS = [('phrase-4.wav', 0.90, '«п»: песик, | песик'), ('phrase-2.wav', 2.07, '«б»: тебе'),
           ('phrase-1.wav', 0.57, '«у»: думаю'), ('phrase-3.wav', 0.50, '«у»: слушай')]
YELLOW, BG = (255, 214, 120), (22, 22, 22)


def run(src):
    src = Path(src)
    ev = [json.loads(s) for s in (src/'events.jsonl').read_text().splitlines()]
    fps = {e['seg']: e['fps'] for e in ev if e['kind'] == 'rec'}
    hero = [e for e in ev if e['kind'] == 'hero']
    return {'src': src, 'fps': fps, 'hero': hero}


def ptts(r, seg):
    return [e for e in r['hero'] if e['seg'] == seg]


def part(now, before, seg_l, seg_r, left, right, title, offset, tmp, n):
    """One part of the clip: two recordings side by side from LEAD before their first phrase, with the microphone."""
    L, R = ptts(before if left == 'было' else now, seg_l), ptts(now, seg_r)
    src_l = (before if left == 'было' else now)['src']/f'{seg_l}.mp4'
    src_r = now['src']/f'{seg_r}.mp4'
    fps = now['fps'][seg_r]
    a_l, a_r = L[0]['frame']/fps-LEAD, R[0]['frame']/fps-LEAD
    dur = (R[-1]['frame']-R[0]['frame'])/fps+R[-1]['ms']/1000+LEAD+TAIL
    # the microphone: each recording from the time the server started to play it
    mic = np.zeros(int(dur*RATE))
    for e in R:
        with wave.open(e['wav']) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), np.int16)/32768
        at = int((e['frame']/fps-a_r+offset/1000)*RATE)
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


def frame(r, seg, wav, t, lag, offset):
    e = next(e for e in ptts(r, seg) if Path(e['wav']).name == wav)
    fps = r['fps'][seg]
    return round(e['frame']+(t+(offset+lag)/1000)*fps)


def main(argv):
    args = list(argv)
    opt = {}
    for name in ('--offset-ms', '--extra'):
        if name in args:
            i = args.index(name)
            opt[name] = args[i+1]
            del args[i:i+2]
    if len(args) < 5:
        sys.exit(__doc__)
    now, before = run(args[0]), run(args[1])
    base, head, out = args[2], args[3], Path(args[4])
    offset = float(opt.get('--offset-ms', 0))
    out.mkdir(parents=True, exist_ok=True)
    report, lines = measure(args[0], base, head, ('voice', 'npc0', 'npc', 'own'), offset)
    rb, lines_b = measure(args[1], base, head, ('voice', 'npc'), offset)
    lag = {p['wav']: p['lag_ms'] for p in report['phrases'] if p.get('seg') == 'voice'}
    mean_lag = float(np.median(list(lag.values())))

    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        parts = [part(now, before, 'voice', 'voice', 'было', 'стало', 'Четыре фразы игрока (микрофон)', offset, tmp, 0),
                 part(now, before, 'npc', 'npc', 'было', 'стало',
                      'Голос Ксардаса в микрофоне (на 12 дБ тише героя), с 3 с — фраза героя', offset, tmp, 1),
                 part(now, before, 'npc0', 'npc', 'без приглушения', 'с приглушением',
                      'Голос Ксардаса в микрофоне: как было бы без приглушения', offset, tmp, 2)]
        (tmp/'list.txt').write_text(''.join(f"file '{p}'\n" for p, _ in parts))
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', str(tmp/'list.txt'),
                        '-c', 'copy', str(out/'clip.mp4')], check=True)

        # the sheet
        shots = []
        for wav, at, what in MOMENTS:
            if wav in lag:
                shots.append((what+f' ({wav[:-4]}, {at:.2f} с)', 'voice', 'voice', wav, at, lag[wav], 'было', 'стало'))
        voiced = [(p['wav'], p) for p in report['phrases'] if p.get('seg') == 'voice']
        loud = max(((w, *_loudest(now, w)) for w, _ in voiced), key=lambda v: v[2])
        shots.append((f'громкая гласная ({loud[0][:-4]}, {loud[1]:.2f} с)', 'voice', 'voice', loud[0], loud[1],
                      lag[loud[0]], 'было', 'стало'))
        pauses = [(p['ms'], w, p) for w, ph in voiced for p in ph['pauses']]
        if pauses:
            ms, w, p = max(pauses, key=lambda v: v[0])
            at = (p['at_ms']+ms/2)/1000
            shots.append((f'пауза {ms} мс ({w[:-4]}, {at:.2f} с)', 'voice', 'voice', w, at, lag[w], 'было', 'стало'))
        shots.append(('голос Ксардаса, герой молчит (npc-mix, 1.50 с)', 'npc0', 'npc', 'npc-mix.wav', 1.5, mean_lag,
                      'без приглушения', 'с приглушением'))
        cells, key_moments = [], []
        for title, seg_l, seg_r, wav, at, lg, left, right in shots:
            src_l = now if left != 'было' else before
            fl, fr = frame(src_l, seg_l, wav, at, lg, offset), frame(now, seg_r, wav, at, lg, offset)
            il = frame_at(src_l['src']/f'{seg_l}.mp4', fl, tmp/'l.png')
            ir = frame_at(now['src']/f'{seg_r}.mp4', fr, tmp/'r.png')
            cells.append((title, left, right, il, ir))
            key_moments.append(f'лист: {title} — {left} кадр {fl} ({seg_l}.mp4), {right} кадр {fr} ({seg_r}.mp4)')
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
    key += ['', *key_moments, '', f'Замер «стало» (live_lag.py, сдвиг от ptt стенда до сервера {offset:.1f} мс):',
            *['  '+s for s in lines], '', 'Замер «было»:', *['  '+s for s in lines_b]]
    if '--extra' in opt:
        key += ['', *Path(opt['--extra']).read_text().splitlines()]
    (out/'key.txt').write_text('\n'.join(key)+'\n')
    print('\n'.join(key))


def _loudest(r, wav):
    """The loudest 10 ms of the phrase: its time in s and dB."""
    e = next(e for e in r['hero'] if Path(e['wav']).name == wav)
    _, db = voice(e['wav'])
    i = int(np.argmax(np.convolve(db, np.ones(3)/3, 'same')))
    return i/100, float(db[i])


if __name__ == '__main__':
    main(sys.argv[1:])
