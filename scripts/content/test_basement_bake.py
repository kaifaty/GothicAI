import numpy as np
import pytest

import basement_bake as bb


@pytest.fixture(scope='module')
def layouts():
    return {v: bb.basement(0, v) for v in bb.LAYOUTS}


def top(spec, grids):
    """The top height of the whole square from its pieces."""
    gx, _, _, _ = bb._grid()
    out = np.full(gx.shape, -1.)
    for p in spec['pieces']:
        g = grids[p['id']]
        r0 = round(p['grid'][1] / bb.STEP)
        out[r0:r0 + g.shape[0]] = np.maximum(out[r0:r0 + g.shape[0]], g)
    return out


def test_pieces_by_band_and_by_detail(layouts):
    kinds = {v: [p['kind'] for p in spec['pieces']] for v, (spec, _, _, _) in layouts.items()}
    assert kinds['empty'] == ['block', 'rubble', 'beam', 'top', 'opening']      # the joist socket
    assert kinds['window'] == ['block', 'rubble', 'beam', 'top', 'frame', 'opening']
    assert kinds['chain'] == ['block', 'rubble', 'beam', 'top', 'frame', 'opening', 'iron']


def test_rubble_of_flat_stones_with_recessed_joints(layouts):
    spec, _, grids, labels = layouts['empty']
    height = top(spec, grids)
    _, _, u, v = bb._grid()
    band = (v > bb.RUBBLE[0] + .02) & (v < bb.RUBBLE[1] - .02)
    stones = np.array(spec['stones'])[labels]
    rubble = band & (stones == 'rubble')
    joints = band & (stones == 'mortar')
    assert .12 < joints.sum() / band.sum() < .35                     # joints show, the stones fill most
    assert np.median(height[rubble]) - np.median(height[joints]) > .012   # and lie well behind the faces
    # flat bedded stones: many more of them than courses, longer than high
    ids = np.unique(labels[rubble])
    assert 300 < len(ids) < 1200
    sizes = [(np.ptp(u[labels == i]) * bb.PERIOD[0], np.ptp(v[labels == i]) * bb.PERIOD[1]) for i in ids[::25]]
    assert np.median([w / max(h, 1e-3) for w, h in sizes if w < 1]) > 2


def test_beam_and_blocks_where_the_original_has_them(layouts):
    spec, _, grids, labels = layouts['empty']
    stones = np.array(spec['stones'])[labels]
    _, _, _, v = bb._grid()
    column = stones[:, 100]
    vs = v[:, 100]
    assert np.all(column[(vs > bb.BEAM[0] + .005) & (vs < bb.BEAM[1] - .005)] == 'beam')
    assert set(column[vs > bb.BLOCK_ROWS[0] + .01]) <= {'block', 'mortar'}


def test_the_square_wraps(layouts):
    spec, _, grids, labels = layouts['window']
    height = top(spec, grids)
    assert np.array_equal(height[:, 0], height[:, -1])
    assert np.array_equal(labels[0], labels[-1])


def test_window_and_slit_open_in_their_frames(layouts):
    for variant, rect in (('window', bb.WINDOW), ('chain', bb.SLIT)):
        spec, _, grids, _ = layouts[variant]
        height = top(spec, grids)
        gx, gy, _, _ = bb._grid()
        hole = bb._rect(gx, gy, rect['opening'])
        assert np.all(height[hole] == bb.OPENING_Z)
        frame = bb._rect(gx, gy, rect['frame']) & ~hole
        assert np.median(height[frame]) > bb.FRAME_PROUD - .01


def test_match_follows_the_target_without_stripes():
    rng = np.random.default_rng(0)
    v = np.tile(np.linspace(0, 1, 400)[:, None], (1, 50))
    out = rng.uniform(.2, .4, (400, 50, 3))
    mask = np.ones(v.shape, bool)
    target = lambda x: np.array([.1, .08, .05]) * (1 + x)          # x: the middle of a band
    bb._match(out, mask, v, target, 10)
    rows = out.mean(axis=1)
    assert np.allclose(rows[180:220].mean(axis=0), target(.5), rtol=.08)
    assert np.abs(np.diff(rows[:, 0])).max() < .02                     # no step between the bands
