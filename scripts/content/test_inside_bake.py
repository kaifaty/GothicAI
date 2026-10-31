import numpy as np
import pytest

import inside_bake as ib


@pytest.fixture(scope='module')
def built():
    return ib.layout(seed=1)


def test_layout_is_reproducible(built):
    spec, _ = built
    again, _ = ib.layout(seed=1)
    assert [p['outline'] for p in again['pieces']] == [p['outline'] for p in spec['pieces']]


def test_rafters_and_courses_as_the_original(built):
    spec, _ = built
    px, py = spec['period']
    rafters = sorted((np.mean([q[0] for q in p['outline']]) % px) / px for p in spec['pieces'] if p['kind'] == 'rafter')
    centres = [r if r < .9 else r - 1 for r in rafters]
    assert np.allclose(sorted(centres), ib.RAFTERS, atol=.01)
    courses = {p['course'] for p in spec['pieces'] if p['kind'] == 'board'}
    assert courses == set(range(ib.COURSES))
    assert sum(p['kind'] == 'rail' for p in spec['pieces']) == 1


def test_boards_are_spaced_sheathing(built):
    """Narrow boards over wide gaps: across a field, about half of a vertical line is board."""
    import shapely
    from shapely import affinity, unary_union
    from shapely.geometry import Polygon
    spec, _ = built
    px, py = spec['period']
    boards = unary_union([affinity.translate(Polygon(p['outline']), dx, dy) for p in spec['pieces']
                          if p['kind'] == 'board' for dx, dy in p['copies']])
    ys = np.linspace(ib.RAIL[0] / 2 + .05, py - ib.RAIL[0] / 2 - .05, 600)
    share = np.mean([shapely.contains_xy(boards, np.full_like(ys, x), ys).mean() for x in (2., 7., 12.)])
    assert .3 < share < .6


def test_compose_soot_darkens_towards_the_ridge():
    spec = dict(period=[8., 4.], rafters=[0.], pieces=[dict(id=0, kind='board', outline=[[0, 0], [8, 0], [8, 4], [0, 4]])])
    rows, cols = 64, 64
    piece = np.zeros((rows, cols), int)
    y = np.repeat(np.linspace(3.5, .5, rows)[:, None], cols, axis=1)        # rows run down: the top is the ridge
    x = np.tile(np.linspace(5., 6., cols), (rows, 1))                          # away from the rafter and the stains
    passes = dict(piece=piece, x=x, y=y, colour=np.full((rows, cols, 3), .3), sky=np.ones((rows, cols)),
                  crevice=np.ones((rows, cols)), ao=np.ones((rows, cols)), height=np.zeros((rows, cols)),
                  normal=np.dstack([np.zeros((rows, cols))] * 2 + [np.ones((rows, cols))]))
    result = ib.compose(passes, spec, seed=1)
    lum = result['albedo'].mean(axis=(1, 2))
    assert lum[:10].mean() < .85 * lum[-10:].mean()
    assert result['roughness'].min() >= ib.ROUGH[0] - 1e-9
