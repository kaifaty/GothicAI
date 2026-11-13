#!/usr/bin/env python3
"""How far the hero's mouth lags behind the player's live voice in the game (player-voice-face.md, step 1).

  live_lag.py <bench out> <MPFB head.glb> <head.glb> [--seg voice] [--clock-ms X] [--offset-ms X] [--json out.json]

The bench is scripts/bench/player-voice-clip.bench: the hero says recordings with `ptt <wav>` (events.jsonl, kind
"hero"), the server plays them as the microphone in real time and streams them back (ptt_audio). For each phrase:
- the voice: the speech-band (120..2500 Hz, as LipSync::Track) RMS of the wav per 10 ms. It starts where the server's
  microphone played its first sample: the bridge's "[ai] ptt_audio" line of the phrase has first_ms (from the key to
  the first chunk, the game's clock) and first_delivery_us (from the server to the bridge, the two system clocks), so
  the start is the bench's `ptt` + first_ms - that delivery. --clock-ms X: the game's system clock minus the server's
  (measured outside, e.g. over one ssh session), taken off the deliveries; a log without first_ms: --offset-ms X is
  the start after `ptt`;
- the mouth: the opening of the mouth, the gap between the middles of the lips in mm (the contact points of
  lip_gap_game.py, found on the MPFB head: a head with Mark's shapes has its mesh) with the weights of face.csv on
  the head's shapes, minus the gap at rest, resampled to 10 ms;
- the lag: the peak of their normalized cross-correlation over -50..+250 ms (parabola between steps), the mouth after
  the voice > 0; with its correlation.
Also per phrase: the pauses of the voice of at least 150 ms (below the phrase's loudest moment - 24 dB, where the
mouth rule starts to open), the widest lip_open (metrics.csv: of the head's widest VISEME frame) in each, shifted by
the lag; lip_open before the first word (a breath); in npc-mix.wav the widest lip_open of its first 5 s, where only
the npc's voice is in the microphone; the widest gap of the voiced frames; the brow moves (br_w) and the accents of
game.log. A segment without `ptt` (the hero's recorded voice): the widest gap. And the end-to-end times of the
bridge's "[ai] ptt_audio" lines: to the bridge and to the tick that shows a chunk, the clocks corrected.
"""
import csv
import json
import re
import sys
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lip_gap_game as G  # noqa: E402

STEP = 10      # ms
PAUSE = 150    # ms: a pause at least this long shuts the mouth
SILENT = 24    # dB below the phrase's loudest moment: the mouth rule's silence (LipSync Range)
LEAK = 5000    # ms of npc-mix.wav with only the npc's voice in the microphone (the hero's phrase from then on)
PTT = re.compile(r'\[ai\] ptt_audio (\S+) chunks=(\d+) .*ducked=(\d+).*delivery_median_us=(-?\d+) '
                 r'delivery_p95_us=(-?\d+) shown_median_us=(-?\d+) shown_p95_us=(-?\d+)'
                 r'(?: first_ms=(\d+) first_delivery_us=(-?\d+))?')


def biquad(x, f, high, rate=16000):
    w = 2*np.pi*min(f, 0.45*rate)/rate
    c, a = np.cos(w), np.sin(w)/(2*0.7071)
    n = 1/(1+a)
    b = [(1+c)/2*n, -(1+c)*n, (1+c)/2*n] if high else [(1-c)/2*n, (1-c)*n, (1-c)/2*n]
    a1, a2 = -2*c*n, (1-a)*n
    y = np.zeros_like(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):  # the RBJ filter of LipSync::Track (direct form II transposed)
        o = b[0]*v+z1
        z1 = b[1]*v-a1*o+z2
        z2 = b[2]*v-a2*o
        y[i] = o
    return y


def voice(path):
    """Speech-band RMS (linear) and dB per 10 ms."""
    with wave.open(str(path)) as w:
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), np.int16)/32768
    y = biquad(biquad(x, 120, True, rate), 2500, False, rate)
    k = rate*STEP//1000
    n = len(y)//k
    p = (y[:n*k].reshape(n, k)**2).mean(1)
    return np.sqrt(p), 10*np.log10(p+1e-10)


def xcorr(env, mouth):
    a = env-env.mean()
    out = []
    for lag in range(-5, 26):
        u, v = (a[:len(a)-lag], mouth[lag:]) if lag >= 0 else (a[-lag:], mouth[:len(mouth)+lag])
        m = min(len(u), len(v))
        u, v = u[:m], v[:m]-v[:m].mean()
        out.append(float(u @ v/(np.linalg.norm(u)*np.linalg.norm(v)+1e-12)))
    j = int(np.argmax(out))
    s = 0.0
    if 0 < j < len(out)-1:
        y0, y1, y2 = out[j-1:j+2]
        d = y0-2*y1+y2
        s = 0.5*(y0-y2)/d if d else 0.0
    return (j-5+s)*STEP, out[j]


def runs(mask):
    i = 0
    while i < len(mask):
        if mask[i]:
            j = i
            while j < len(mask) and mask[j]:
                j += 1
            yield i, j
            i = j
        else:
            i += 1


def deliveries(log, clock):
    out = []
    us = clock*1000
    for row in log:
        if m := PTT.search(row):
            out.append({'track': m[1], 'chunks': int(m[2]), 'ducked': int(m[3]),
                        'delivery_median_us': int(m[4])-us, 'delivery_p95_us': int(m[5])-us,
                        'shown_median_us': int(m[6])-us, 'shown_p95_us': int(m[7])-us,
                        'first_ms': int(m[8]) if m[8] else None,
                        'first_delivery_us': int(m[9])-us if m[9] else None})
    return out


def phrase(seg, e, t, gap, m_t, m_open, m_brow):
    env, db = voice(e['wav'])
    t0 = e['t']+e['start']      # the first sample of the voice, ms of the recording
    mouth = np.interp(t0+np.arange(len(env))*STEP, t, gap)
    lag, r = xcorr(env, mouth)
    quiet = db < np.convolve(db, np.ones(3)/3, 'valid').max()-SILENT
    loud = np.nonzero(~quiet)[0]
    first, last = loud[0], loud[-1]
    pauses = []
    for i, j in runs(quiet):
        if i > first and j <= last and (j-i)*STEP >= PAUSE:
            lo, hi = t0+i*STEP+lag, t0+j*STEP+lag
            pauses.append({'at_ms': i*STEP, 'ms': (j-i)*STEP, 'open_min': float(m_open[(m_t >= lo) & (m_t < hi)].min()),
                           'gap_min': float(np.interp(np.arange(lo, hi, 5.), t, gap).min())})
    # before the first word as the mouth shows it (lag on), without the onset of the word
    before = (m_t >= t0+lag) & (m_t < t0+first*STEP+lag-20)
    voiced = (m_t >= t0+first*STEP) & (m_t < t0+(last+1)*STEP+lag)
    g_voiced = np.interp(m_t[voiced], t, gap)
    mix = Path(e['wav']).name.startswith('npc')
    leak = (m_t >= t0+lag) & (m_t < t0+LEAK+lag)
    return {'seg': seg, 'wav': Path(e['wav']).name, 'ms': e['ms'], 'start_ms': round(e['start'], 1),
            'lag_ms': round(lag, 1), 'r': round(r, 3), 'pauses': pauses,
            'before_open': float(m_open[before].max()) if before.any() else 0.0,
            'open_max': float(m_open[voiced].max()), 'gap_max': float(g_voiced.max()),
            'gap_p95': float(np.percentile(g_voiced, 95)),
            'brow_moves': int(((m_brow[1:] != 0) & (m_brow[:-1] == 0) & voiced[1:]).sum()),
            'leak_open': float(m_open[leak].max()) if mix else None,
            'leak_gap': float(np.interp(m_t[leak], t, gap).max()) if mix else None}


def measure(src, base, head, segs=('voice',), offset=0.0, clock=0.0):
    """The report of the run and its lines in Russian (key.txt of player_voice_clip.py)."""
    src = Path(src)
    b = G.contact(G.glb(base))
    parts = G.glb(head)
    model = (parts['face'][0], parts['face'][1], b[2], b[3], b[4])
    events = [json.loads(s) for s in (src/'events.jsonl').read_text().splitlines()]
    rec_t = {e['seg']: e['t'] for e in events if e['kind'] == 'rec'}
    with open(src/'face.csv') as f:
        names = f.readline().strip().split(',')[5:]
        face = list(csv.reader(f))
    with open(src/'metrics.csv') as f:
        metrics = list(csv.DictReader(f))
    log = (src/'game.log').read_text(errors='replace').splitlines()
    report = {'phrases': [], 'delivery': deliveries(log, clock)}
    # the bridge's "[ai] ptt_audio" lines and the bench's `ptt`s go in the same order
    ptts = [dict(e) for e in events if e['kind'] == 'hero']
    for e, d in zip(ptts, report['delivery']+[None]*len(ptts)):
        e['t'] -= rec_t[e['seg']]
        e['start'] = d['first_ms']-d['first_delivery_us']/1000 if d and d['first_ms'] is not None else offset
    out = []
    rest = G.gaps(model, names, np.zeros((1, len(names)), np.float32))[0]
    for seg in segs:
        rows = [r for r in face if r[0] == seg]
        t = np.array([float(r[2]) for r in rows])
        gap = (G.gaps(model, names, np.array([[float(v) for v in r[5:]] for r in rows], np.float32))-rest)*1000
        mt = [r for r in metrics if r['seg'] == seg]
        m_t = np.array([float(r['t']) for r in mt])
        m_open = np.array([float(r['lip_open']) for r in mt])
        m_brow = np.array([float(r['br_w']) for r in mt])
        said = [e for e in ptts if e['seg'] == seg]
        for e in said:
            ph = phrase(seg, e, t, gap, m_t, m_open, m_brow)
            report['phrases'].append(ph)
            ps = ', '.join('%d мс у %.2f с: lip_open %.3f, щель %.1f мм' % (p['ms'], p['at_ms']/1000, p['open_min'],
                                                                         p['gap_min']) for p in ph['pauses']) or 'нет'
            out.append('%-6s %-13s звук через %5.1f мс после ptt; отставание %5.1f мс (r %.2f); до первого слова '
                       'lip_open %.3f; рот шире всего %.2f (щель %.1f мм, p95 %.1f); брови %d; паузы от 150 мс: %s'
                       % (seg, ph['wav'], ph['start_ms'], ph['lag_ms'], ph['r'], ph['before_open'], ph['open_max'],
                          ph['gap_max'], ph['gap_p95'], ph['brow_moves'], ps))
            if ph['leak_open'] is not None:
                out.append('%-6s %-13s только голос NPC (первые %.1f с): lip_open шире всего %.3f, щель %.1f мм'
                           % (seg, ph['wav'], LEAK/1000, ph['leak_open'], ph['leak_gap']))
        if not said and len(t):
            g = gap[t > 500]
            report['phrases'].append({'seg': seg, 'gap_max': float(g.max()), 'gap_p95': float(np.percentile(g, 95)),
                                      'open_max': float(m_open.max())})
            out.append('%-6s голос героя из записи игры: щель шире всего %.1f мм, p95 %.1f; lip_open шире всего %.2f'
                       % (seg, g.max(), np.percentile(g, 95), m_open.max()))
    for d in report['delivery']:
        out.append('%s: %d кусков, приглушено %d; до моста медиана %.1f мс, p95 %.1f; до тика показа медиана %.1f мс, '
                   'p95 %.1f%s' % (d['track'], d['chunks'], d['ducked'], d['delivery_median_us']/1000,
                                   d['delivery_p95_us']/1000, d['shown_median_us']/1000, d['shown_p95_us']/1000,
                                   '' if d['first_ms'] is None else '; первый кусок через %d мс после нажатия, в пути '
                                   '%.1f мс' % (d['first_ms'], d['first_delivery_us']/1000)))
    for row in log:
        if '[face] accent' in row:
            out.append(row[row.index('[face]'):])
    return report, out


def main(argv):
    args = list(argv)
    opt = {}
    for name in ('--seg', '--offset-ms', '--clock-ms', '--json'):
        if name in args:
            i = args.index(name)
            opt[name] = args[i+1]
            del args[i:i+2]
    if len(args) < 3:
        sys.exit(__doc__)
    report, lines = measure(args[0], args[1], args[2], opt.get('--seg', 'voice').split(','),
                            float(opt.get('--offset-ms', 0)), float(opt.get('--clock-ms', 0)))
    print('\n'.join(lines))
    if '--json' in opt:
        Path(opt['--json']).write_text(json.dumps(report, ensure_ascii=False, indent=1))


if __name__ == '__main__':
    main(sys.argv[1:])
