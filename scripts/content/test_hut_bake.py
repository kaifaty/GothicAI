import numpy as np
import pytest

import hut_bake as hb

LUM = np.array([.2126, .7152, .0722])


@pytest.fixture(scope='module')
def wall():
    return hb.wall(seed=0, window=False)


@pytest.fixture(scope='module')
def window():
    return hb.wall(seed=0, window=True)


@pytest.fixture(scope='module')
def gable():
    return hb.gable(seed=0)


def surface(spec, grids, u, v):
    """The top height of the layout at texture u (from the left) and v (from the top)."""
    px, py = spec['period']
    x, y = u * px, (1 - v) * py
    top = hb.DROP
    for piece in spec['pieces']:
        x0, y0, dx, dy = piece['grid']
        grid = grids[piece['id']]
        for ox, oy in piece['copies']:
            c, r = round((x - x0 - ox) / dx), round((y - y0 - oy) / dy)
            if 0 <= r < grid.shape[0] and 0 <= c < grid.shape[1]:
                top = max(top, grid[r, c])
    return top


def test_ten_boards_with_gaps_at_the_originals_places(wall):
    spec, _, grids = wall
    assert [p['kind'] for p in spec['pieces']] == ['board'] * 10
    for v in (.3, .7):
        for u in hb.EDGES:                                   # a dark gap at every measured edge, within the wave
            assert min(surface(spec, grids, u + du, v) for du in np.linspace(-.006, .006, 13)) == hb.DROP
        for a, b in zip(hb.EDGES, hb.EDGES[1:]):             # wood in the middle of every board
            assert surface(spec, grids, (a + b) / 2, v) > -.02


def test_window_keeps_the_boards_and_adds_its_frame(wall, window):
    spec, _, grids = window
    kinds = [p['kind'] for p in spec['pieces']]
    assert kinds == ['board'] * 10 + ['head', 'sill', 'jamb', 'jamb', 'shutter']
    plain = wall[2]
    assert np.array_equal(grids[0], plain[0])                # the same boards away from the window
    u0, u1, v0, v1 = hb.OPENING
    middle = surface(spec, grids, (u0 + u1) / 2, (v0 + v1) / 2)
    assert hb.SHUTTER_Z - .01 < middle < -.02                # the shutter, set back behind the wall
    assert surface(spec, grids, (hb.SILL[0] + hb.SILL[1]) / 2, (hb.SILL[2] + hb.SILL[3]) / 2) > .015


def test_gable_rows_lap_with_a_step_at_every_lower_edge(gable):
    spec, _, grids = gable
    assert len(spec['pieces']) == len(hb.ROWS)                # one cut per row at most: a board across the seam
    # down the wall at one u: a drop of over 8 mm at each of the eight lapped edges, near its measure
    vs = np.arange(0, 1, .002)
    heights = np.array([surface(spec, grids, .2, v) for v in vs])
    drops = vs[2:][heights[2:] - heights[:-2] < -.008]
    edges = [v for v, _ in hb.ROWS]
    assert all(min(abs((drops - e + .5) % 1 - .5)) < .03 for e in edges)
    for v, joints in hb.ROWS:
        for u in joints:                                     # a butt joint where the original has one: a slot
            slot = min(surface(spec, grids, u + du, v - .03) for du in np.linspace(-.002, .002, 9))
            assert slot < surface(spec, grids, u + .02, v - .03) - .01


def test_compose_keeps_the_dark_gaps_and_the_originals_median():
    spec = dict(wall='wall', period=[3., 3.], ground=hb.GROUND, nails=[], knots=[],
                pieces=[dict(id=0, kind='board'), dict(id=1, kind='board')])
    rows = cols = 96
    rng = np.random.default_rng(1)
    piece = np.zeros((rows, cols), int)
    piece[:, 48:] = 1
    piece[:, 46:50] = -1                                     # a gap between the two boards
    ys, xs = np.mgrid[0:rows, 0:cols]
    passes = dict(piece=piece, colour=.2 + .05 * rng.random((rows, cols, 3)), ao=np.ones((rows, cols)),
                  crevice=np.ones((rows, cols)), height=np.zeros((rows, cols)),
                  x=xs * 3. / cols, y=(rows - 1 - ys) * 3. / rows,
                  normal=np.dstack([np.zeros((rows, cols)), np.zeros((rows, cols)), np.ones((rows, cols))]))
    out = hb.compose(passes, spec)
    albedo = out['albedo']
    assert np.all(albedo[(piece < 0) & (out['soil'] < .5)] < .01)       # dark above the soil
    assert (albedo[(piece < 0) & (out['soil'] > .9)] @ LUM).min() > .008   # the soil fills the gaps at the foot
    srgb = (albedo @ np.array([.2126, .7152, .0722])) ** (1 / 2.2) * 255
    assert abs(np.median(srgb) - hb.LOOKS['wall']['median']) < 4
    assert out['soil'][-1].mean() > .5 > out['soil'][0].mean()     # soil over the boards' feet, none on top
