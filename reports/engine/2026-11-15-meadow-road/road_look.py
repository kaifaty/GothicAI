#!/usr/bin/env python3
"""The cart track's look by measure: the tables of the note's «Что вышло» from the frames of the filming.

    uv run --no-project --with numpy python reports/engine/2026-11-15-meadow-road/road_look.py <table> <runs>

<runs> holds one folder per run, f-<side>-<bench>: `off` is main with OPENGOTHIC_GRASS=0, `was` and `was2` two runs
of main with the set meadow-ground-1, `now` the branch with meadow-road-1, `same` the branch with meadow-ground-1,
`nowoff` the branch with meadow-road-1 and OPENGOTHIC_GRASS=0, `now2` a second run of `now`; `waseye`, `waseye2`
and `noweye` are `was`, `was2` and `now` with OPENGOTHIC_EYE=0 (landscape-meadow-variety only). The benches: landscape-meadow, landscape-meadow-road,
landscape-meadow-variety, landscape-meadow-parts. Linear light throughout; the helpers are scripts/grass_look.py's.

  tone    the road's own pixels (the cover's paint of the branch: `grass parts 256`, the track's crust blue and its
          loose earth cyan; the sunlit part of them) in «было» and «стало»: the mean luminance, the hue, the
          saturation, the loose earth against the crust, the dark quarter against the light one - and how varied
          they are. The squares of `grass parts 8385` are painted on the meadow's ground only, so the road's
          variety is by squares of the screen: the spread of the luminance (of its logarithm) and of the hue
          between blocks of 64 px, between blocks of 8 px within one of 64, between pixels within one of 8
  colour  the same pixels in OKLab, a pixel at a time: the red and the yellow axes (a, b; of 1000), the hue of
          their mean, the mean chroma and the spread of the lightness - «было», «стало» and, where its folder is
          there, `grey`: the branch's first filming, whose road had the luminance and had lost the colour. The
          hue and the saturation of `tone` are those of the mean colour, and they hid the loss
  bands   goal Б on the views along and across the track: the earth of the verge's strip (the paint's magenta to
          orange beside the road), the road's edge (the outer eighth of its width on each side) and the road
          further in (the next quarter): how far the neighbours' mean luminance is apart
  guards  the meadow's own guards: the tone of its sunlit ground against «выключено» (grass_look.py tone, the masks
          of landscape-meadow-parts) and its variety against «было» (grass_look.py variety --light)
  away    what changed on the meadow by the distance from a road: the meadow's squares of 0.5 m, counted in rings
          from those that touch the road (a ring n lies (n-1)*0.5 m from it or further); the mean difference of
          «стало» from «было» in each ring beside that of two runs of «было», the eye's adaptation off; and the
          top of the frame with it on - what the eye does to the whole frame when the road's look changes
  same    the frames of a set without the new fields on the branch against main, and of the branch with the grass
          off against main with it off: the mean difference of a frame, of 255, beside that of two runs of main
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
import grass_look as g  # noqa: E402

ROAD_VIEWS = ('track-along', 'game-verge-across', 'track-low', 'meadow-2', 'far')
RUNS = Path()


def frame(side: str, bench: str, name: str) -> np.ndarray:
    return g.read_image(str(RUNS / f'f-{side}-{bench}' / f'{name}.png'))


def road_masks(paint: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """The track's own pixels in the cover's paint: the sunlit ones, and of them the crust and the loose earth."""
    p = paint.astype(np.int32)
    r, gr, b = p[..., 0], p[..., 1], p[..., 2]
    road = (b > 40) & (3*r < b)
    lit = g.erode(road & (b >= 0.5*np.percentile(b[road], 90)), 2)
    return lit, lit & (3*gr < b), lit & (10*gr > 6*b)


def blocks(values: np.ndarray, mask: np.ndarray, size: int) -> tuple[np.ndarray, np.ndarray]:
    """Sums of the values and of the mask over squares of the screen."""
    h, w = mask.shape[0]//size*size, mask.shape[1]//size*size
    shape = (h//size, size, w//size, size)
    v = (values[:h, :w]*mask[:h, :w, None]).reshape(*shape, -1).sum(axis=(1, 3))
    return v, mask[:h, :w].reshape(shape).sum(axis=(1, 3))


def screen_variety(rgb: np.ndarray, mask: np.ndarray) -> dict:
    h, wd = mask.shape[0]//64*64, mask.shape[1]//64*64
    lin, mask = g.linear(rgb)[:h, :wd], mask[:h, :wd]
    s64, n64 = blocks(lin, mask, 64)
    s8, n8 = blocks(lin, mask, 8)
    m64, m8 = s64/np.maximum(n64, 1)[..., None], s8/np.maximum(n8, 1)[..., None]
    ok64, ok8 = n64 >= 64*64//4, (n8 >= 32) & np.repeat(np.repeat(n64 >= 64*64//4, 8, axis=0), 8, axis=1)
    l64, h64 = np.log(np.maximum(m64 @ g.COEF, 1e-6)), g.hue_of(m64)
    l8, h8 = np.log(np.maximum(m8 @ g.COEF, 1e-6)), g.hue_of(m8)
    w = n64[ok64].astype(np.float64)
    mean_h = np.degrees(np.arctan2((w*np.sin(np.radians(h64[ok64]))).sum(), (w*np.cos(np.radians(h64[ok64]))).sum()))
    out = {'lum_64': g.spread(l64[ok64] - (w*l64[ok64]).sum()/w.sum(), w), 'hue_64': g.spread(g.turn(h64[ok64] - mean_h), w)}
    up = lambda a: np.repeat(np.repeat(a, 8, axis=0), 8, axis=1)  # noqa: E731
    w = n8[ok8].astype(np.float64)
    out.update({'lum_8': g.spread((l8 - up(l64))[ok8], w), 'hue_8': g.spread(g.turn(h8 - up(h64))[ok8], w)})
    px = mask & up(ok8)
    lp, hp = np.log(np.maximum(lin @ g.COEF, 1e-4)), g.hue_of(lin)
    one = np.ones(int(px.sum()))
    out.update({'lum_px': g.spread((lp - up(l8))[px], one), 'hue_px': g.spread(g.turn(hp - up(h8))[px], one)})
    return out


def quarters(rgb: np.ndarray, mask: np.ndarray) -> float:
    lum = g.luminance(rgb)[mask]
    low, high = np.percentile(lum, 25), np.percentile(lum, 75)
    return float(lum[lum < low].mean()/lum[lum > high].mean())


def table(head: list[str], rows: list[list]) -> None:
    print('| ' + ' | '.join(head) + ' |')
    print('|' + '---|'*len(head))
    for row in rows:
        print('| ' + ' | '.join(str(x) for x in row) + ' |')
    print()


def num(value: float, digits: int = 3) -> str:
    return f'{value:.{digits}f}'.replace('.', ',')


def cmd_tone() -> None:
    bench = 'landscape-meadow-road'
    tone, relief, variety = [], [], []
    for view in ROAD_VIEWS:
        lit, crust, loose = road_masks(frame('now', bench, f'cover-{view}'))
        was, now, own = frame('was', bench, view), frame('now', bench, view), frame('now', bench, f'{view}-16384')
        tw, tn, to = g.tone_of(was, lit), g.tone_of(now, lit), g.tone_of(own, lit)
        tone.append([f'`{view}`', num(100*lit.mean(), 1), num(tw['luminance'], 4), num(tn['luminance'], 4),
                     num(tn['luminance']/tw['luminance']), num(tn['luminance']/to['luminance']),
                     f"{num(tw['hue'], 1)}° → {num(tn['hue'], 1)}°", num(tn['saturation']/tw['saturation'], 2)])
        lw, ln = g.luminance(was), g.luminance(now)
        relief.append([f'`{view}`', num(100*loose.sum()/lit.sum(), 0), num(lw[loose].mean()/lw[crust].mean(), 2),
                       num(ln[loose].mean()/ln[crust].mean(), 2), num(quarters(was, lit), 2), num(quarters(now, lit), 2)])
        vw, vn = screen_variety(was, lit), screen_variety(now, lit)
        ratio = {k: vn[k]/max(vw[k], 1e-9) for k in vw}
        variety.append([f'`{view}`'] + [f'{num(vw[k], 3 if k[0] == "l" else 1)} → {num(vn[k], 3 if k[0] == "l" else 1)}'
                                        for k in ('lum_64', 'lum_8', 'lum_px', 'hue_64', 'hue_8', 'hue_px')] +
                       [num(float(np.prod([ratio[k] for k in ('lum_64', 'hue_64', 'lum_8', 'hue_8')]))**0.25, 2),
                        num(ratio['lum_px'], 2), num(ratio['hue_px'], 2)])
    table(['Вид', 'Дорога в кадре, %', 'Яркость «было»', '«стало»', 'Отношение', 'К тому же прогону с `grass parts 16384`',
           'Оттенок', 'Насыщенность, отношение'], tone)
    table(['Вид', 'Рыхлой земли в маске, %', 'Рыхлая к корке, «было»', '«стало»', 'Тёмная четверть к светлой, «было»',
           '«стало»'], relief)
    table(['Вид', 'Яркость, блоки 64 px', 'блоки 8 px', 'пиксели', 'Оттенок, блоки 64 px', 'блоки 8 px', 'пиксели',
           'Пестрота крупного', 'Пиксели: яркость', 'оттенок'], variety)


def oklab(lin: np.ndarray) -> np.ndarray:
    lms = np.cbrt(np.maximum(lin @ np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                                             [0.2119034982, 0.6806995451, 0.1073969566],
                                             [0.0883024619, 0.2817188376, 0.6299787005]]).T, 0))
    return lms @ np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099],
                           [0.0259040371, 0.7827717662, -0.8086757660]]).T


def cmd_colour() -> None:
    bench = 'landscape-meadow-road'
    sides = [('«было»', 'was')] + ([('серое', 'grey')] if (RUNS / f'f-grey-{bench}').is_dir() else []) + [('«стало»', 'now')]
    rows = []
    for view in ROAD_VIEWS:
        lit = road_masks(frame('now', bench, f'cover-{view}'))[0]
        cells = {k: [] for k in ('a', 'b', 'hue', 'chroma', 'spread')}
        for _, side in sides:
            lab = oklab(g.linear(frame(side, bench, view))[lit])
            mean = lab.mean(axis=0)
            cells['a'].append(num(1000*mean[1], 1)); cells['b'].append(num(1000*mean[2], 1))
            cells['hue'].append(num(float(np.degrees(np.arctan2(mean[2], mean[1]))), 1) + '°')
            cells['chroma'].append(num(1000*float(np.hypot(lab[:, 1], lab[:, 2]).mean()), 1))
            cells['spread'].append(num(float(lab[:, 0].std()), 3))
        rows.append([f'`{view}`'] + [' → '.join(cells[k]) for k in ('a', 'b', 'hue', 'chroma', 'spread')])
    table(['Вид (' + ' → '.join(name for name, _ in sides) + ')', 'Красное a, из 1000', 'Жёлтое b', 'Оттенок', 'Насыщенность (хрома)',
           'Разброс светлоты'], rows)


def runs_of(mask: np.ndarray, axis: int):
    """Each line across the road: the first and the last of the road's pixels, where the road fills most between."""
    lines = mask if axis == 1 else mask.T
    for i, line in enumerate(lines):
        at = np.flatnonzero(line)
        if len(at) < 40 or len(at) < 0.5*(at[-1] - at[0] + 1):
            continue
        yield i, int(at[0]), int(at[-1])


def cmd_bands() -> None:
    bench = 'landscape-meadow-road'
    rows = []
    for view, axis in (('track-along', 1), ('game-verge-across', 0), ('track-low', 0)):
        paint = frame('now', bench, f'cover-{view}').astype(np.int32)
        r, gr, b = paint[..., 0], paint[..., 1], paint[..., 2]
        road = (b > 40) & (3*r < b)
        strip = (r > 150) & (gr < 90) & ((b > 150) | (b < 40)) & (r > 2*gr)
        edge, inner = np.zeros_like(road), np.zeros_like(road)
        for i, a, z in runs_of(road, axis):
            width = z - a + 1
            line = np.arange(road.shape[1] if axis == 1 else road.shape[0])
            sel_e = ((line >= a) & (line < a + width/8)) | ((line <= z) & (line > z - width/8))
            sel_i = ((line >= a + width/8) & (line < a + 3*width/8)) | ((line <= z - width/8) & (line > z - 3*width/8))
            if axis == 1:
                edge[i], inner[i] = sel_e & road[i], sel_i & road[i]
            else:
                edge[:, i], inner[:, i] = sel_e & road[:, i], sel_i & road[:, i]
        # the strip's earth beside the road: within a sixteenth of the frame's width of the road's pixels
        near = road.copy()
        for _ in range(4):
            grown = near.copy()
            for shift in (40, -40):
                grown |= np.roll(near, shift, axis=0) | np.roll(near, shift, axis=1)
            near = grown
        strip &= near
        for side in ('was', 'now'):
            img = frame(side, bench, view) if side == 'was' else frame('now', bench, view)
            lum = g.luminance(img)
            means = [float(lum[m].mean()) for m in (strip, edge, inner)]
            light = [float(np.sort(lum[m])[int(0.4*m.sum()):].mean()) for m in (strip, edge, inner)]
            rows.append([f'`{view}`', '«было»' if side == 'was' else '«стало»'] + [num(v, 4) for v in means] +
                        [num(means[0]/means[1], 2), num(means[1]/means[2], 2), num(light[0]/light[1], 2), num(light[1]/light[2], 2)])
    table(['Вид', '', 'Земля полосы', 'Край дороги', 'Дорога глубже', 'Полоса к краю', 'Край к глубине',
           'По светлым 60 %: полоса к краю', 'край к глубине'], rows)


def cmd_guards() -> None:
    rows = []
    for view in ('meadow-1', 'meadow-2', 'meadow-3', 'meadow-4', 'far'):
        mask = g.meadow_mask(frame('now', 'landscape-meadow-parts', f'mask-{view}'))
        tone = {side: g.tone_of(frame(side, 'landscape-meadow', view), mask)['luminance'] for side in ('off', 'was', 'was2', 'now')}
        rows.append([f'`{view}`', num(100*mask.mean(), 1)] + [num(tone[s]/tone['off']) for s in ('was', 'was2', 'now')] +
                    [num(tone['now']/tone['was'])])
    table(['Вид', 'Маска, % кадра', '«было» к «выключено»', 'второй прогон «было»', '«стало» к «выключено»',
           '«стало» к «было»'], rows)
    rows = []
    for view in ('meadow-1', 'meadow-3', 'game-verge-along', 'walk-mid', 'far'):
        bench = 'landscape-meadow-variety'
        painted = frame('now', bench, f'mask-{view}')
        mask = g.meadow_mask(painted)
        small, big = g.squares(frame('now', bench, f'cells-{view}'), mask)
        light = np.maximum(g.linear(painted)[..., 0], 1e-3)
        first = g.variety_of(frame('was', bench, f'{view}-0'), small, big, light)
        keys = ('lum_5m', 'hue_5m', 'lum_05m', 'hue_05m')
        row = [f'`{view}`']
        for side in ('was2', 'now'):
            v = g.variety_of(frame(side, bench, f'{view}-0'), small, big, light)
            ratio = {k: v[k]/max(first[k], 1e-9) for k in v if k.startswith(('lum', 'hue'))}
            row += [num(float(np.prod([ratio[k] for k in keys]))**0.25)]
            if side == 'now':
                row += [num(ratio[k], 2) for k in keys + ('lum_px', 'hue_px')]
        rows.append(row)
    table(['Вид', 'Пестрота: второй прогон «было»', '«стало»', 'яркость 5 м', 'оттенок 5 м', 'яркость 0,5 м',
           'оттенок 0,5 м', 'яркость пикселей', 'оттенок пикселей'], rows)


def box(values: np.ndarray, radius: int) -> np.ndarray:
    """The mean over a square of the screen."""
    pad = np.pad(values, radius + 1, mode='edge').cumsum(axis=0).cumsum(axis=1)
    n = 2*radius + 1
    return (pad[n:, n:] - pad[:-n, n:] - pad[n:, :-n] + pad[:-n, :-n])[:values.shape[0], :values.shape[1]]/n**2


def grow(mask: np.ndarray, n: int) -> np.ndarray:
    return ~g.erode(~mask, n)


def rings_of(view: str) -> tuple[np.ndarray, np.ndarray]:
    """The meadow's squares of 0.5 m in rings from the road: 1 touches it, 2 touches a square of 1, and so on."""
    bench = 'landscape-meadow-variety'
    was, now = frame('was', bench, f'mask-{view}').astype(np.int32), frame('now', bench, f'mask-{view}').astype(np.int32)
    r, gr, b = now[..., 0], now[..., 1], now[..., 2]
    ground = (r > 24) & (b > 24) & (2*gr < r) & (2*gr < b)
    # the road: what is not the meadow's ground and differs between the two - the painted road and the track
    road = g.erode(~grow(ground, 2) & (box(np.abs(was - now).sum(axis=2).astype(np.float64), 6) > 18.0), 3)
    small, _ = g.squares(frame('now', bench, f'cells-{view}'), ground)
    count = int(small.max()) + 1
    ring = np.zeros(count, np.int32)
    ring[np.unique(small[grow(road, 9) & (small > 0)])] = 1
    pairs = set()
    for dy, dx in ((0, 5), (5, 0), (5, 5), (5, -5)):
        a = small[max(dy, 0):, max(dx, 0):small.shape[1] + min(dx, 0)]
        c = small[:small.shape[0] - dy, max(-dx, 0):small.shape[1] - max(dx, 0)]
        both = (a > 0) & (c > 0) & (a != c)
        pairs.update(np.unique(a[both].astype(np.int64)*count + c[both]).tolist())
    left = np.array([p//count for p in pairs]); right = np.array([p % count for p in pairs])
    for n in range(1, 40):
        fresh = np.concatenate((right[(ring[left] == n) & (ring[right] == 0)], left[(ring[right] == n) & (ring[left] == 0)]))
        fresh = fresh[fresh > 0]
        if not len(fresh):
            break
        ring[fresh] = n + 1
    ring[0] = 0
    return ring[small], road


def cmd_away() -> None:
    bench = 'landscape-meadow-variety'
    bands = (('1', 1, 1, '0–0,5 м'), ('2', 2, 2, 'от 0,5 м'), ('3', 3, 3, 'от 1 м'), ('4', 4, 4, 'от 1,5 м'),
             ('5–8', 5, 8, 'от 2 м'), ('9 и дальше', 9, 99, 'от 4 м'))
    for view in ('game-verge-along', 'walk-mid', 'meadow-3', 'meadow-1', 'far'):
        ring, road = rings_of(view)
        # the eye's adaptation off (OPENGOTHIC_EYE=0): with it the whole frame follows the road's look
        was, was2, now = (frame(side, bench, f'{view}-0') for side in ('waseye', 'waseye2', 'noweye'))
        d_now = np.abs(now.astype(np.int32) - was).mean(axis=2)
        d_run = np.abs(was2.astype(np.int32) - was).mean(axis=2)
        lw, l2, ln = g.luminance(was), g.luminance(was2), g.luminance(now)
        rows = []
        for name, a, z, metres in bands:
            m = (ring >= a) & (ring <= z)
            if m.sum() < 2000:
                continue
            rows.append([name, metres, int(m.sum()), num(d_run[m].mean(), 2), num(d_now[m].mean(), 2),
                         num(l2[m].mean()/lw[m].mean()), num(ln[m].mean()/lw[m].mean())])
        top = slice(0, int(0.12*lw.shape[0]))
        eye = [g.luminance(frame(side, bench, f'{view}-0'))[top].mean() for side in ('was', 'was2', 'now', 'now2')]
        print(f'`{view}`: дорога в кадре {num(100*road.mean(), 1)} %; верх кадра (12 % высоты) без привыкания глаза: '
              f'два прогона «было» {num(l2[top].mean()/lw[top].mean())}, «стало» к «было» {num(ln[top].mean()/lw[top].mean())}; '
              f'с привыканием: второй прогон «было» {num(eye[1]/eye[0])}, два прогона «стало» {num(eye[2]/eye[0])} и '
              f'{num(eye[3]/eye[0])}')
        print()
        table(['Кольцо', 'От дороги', 'Пикселей', 'Два прогона «было», из 255', '«стало» к «было», из 255',
               'Яркость: два прогона «было»', '«стало» к «было»'], rows)


def mean_diff(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.abs(a.astype(np.int32) - b).mean())


def cmd_same() -> None:
    bench = 'landscape-meadow'
    rows = []
    for view in ('meadow-1', 'meadow-2', 'meadow-3', 'meadow-4', 'far', 'game-verge-across', 'game-verge-along',
                 'bare-low', 'game-turn', 'night-2'):
        was = frame('was', bench, view)
        rows.append([f'`{view}`', num(mean_diff(frame('was2', bench, view), was), 2),
                     num(mean_diff(frame('same', bench, view), was), 2), num(mean_diff(frame('now', bench, view), was), 2)])
    table(['Вид', 'Два прогона `main`', 'Ветка с `meadow-ground-1` к `main`', 'Ветка с `meadow-road-1` к `main`'], rows)
    bench = 'landscape-meadow-road'
    rows = []
    for view in ('track-along', 'game-verge-across', 'track-low', 'meadow-2', 'far', 'track-cross', 'track-end',
                 'track-path05', 'track-rock', 'track-sweet'):
        off = frame('off', bench, view)
        rows.append([f'`{view}`', num(mean_diff(frame('nowoff', bench, view), off), 2)])
    table(['Вид', 'Ветка с `meadow-road-1` и `OPENGOTHIC_GRASS=0` к `main` с ним же'], rows)


def main() -> int:
    global RUNS
    if len(sys.argv) != 3 or sys.argv[1] not in ('tone', 'colour', 'bands', 'guards', 'away', 'same'):
        print(__doc__)
        return 2
    RUNS = Path(sys.argv[2]).expanduser()
    {'tone': cmd_tone, 'colour': cmd_colour, 'bands': cmd_bands, 'guards': cmd_guards, 'away': cmd_away, 'same': cmd_same}[sys.argv[1]]()
    return 0


if __name__ == '__main__':
    sys.exit(main())
