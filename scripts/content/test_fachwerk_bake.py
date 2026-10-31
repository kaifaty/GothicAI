import numpy as np
import pytest

import fachwerk_bake as fb


@pytest.fixture(scope='module')
def layouts():
    return {v: fb.fachwerk(0, v) for v in fb.PERIODS}


def at(layout, u, v):
    """Height and part at texture u (from the left) and v (from the top)."""
    spec, _, grids, labels = layout
    height = grids[0]
    rows, cols = height.shape
    r, c = round((1 - v) * (rows - 1)), round(u * (cols - 1))
    return height[r, c], spec['parts'][labels[r, c]]


TIMBER = ('post', 'rail', 'head', 'sill', 'jamb', 'brace', 'peg')


@pytest.mark.parametrize('variant, wood, plaster', [
    ('wall', [(.01, .5), (.5, .02), (.5, .97), (.18, .78)], [(.5, .4), (.1, .9)]),
    ('window', [(.5, .15), (.175, .4), (.5, .675), (.2, .85)], [(.07, .4), (.5, .85)]),
    ('post', [(.46, .5), (.2, .02)], [(.2, .5), (.8, .5)]),
    ('gable', [(.5, .5), (.5, .95)], [(.2, .3), (.8, .3)]),
])
def test_members_where_the_original_has_them(layouts, variant, wood, plaster):
    for u, v in wood:
        h, part = at(layouts[variant], u, v)
        assert part in TIMBER and h > fb.PROUD - .03, (u, v, part)
    for u, v in plaster:
        h, part = at(layouts[variant], u, v)
        assert part in ('plaster', 'spall') and abs(h) < .012, (u, v, part)


def test_timber_stands_out_of_the_plaster(layouts):
    spec, _, grids, labels = layouts['wall']
    parts = np.array(spec['parts'])[labels]
    timber = np.isin(parts, TIMBER)
    assert np.median(grids[0][timber]) - np.median(grids[0][parts == 'plaster']) > .015


def test_shutter_of_boards_with_dark_gaps_and_straps(layouts):
    spec, _, grids, labels = layouts['window']
    parts = np.array(spec['parts'])[labels]
    assert (parts == 'shutter').sum() > (parts == 'dark').sum() > 0
    assert (parts == 'iron').sum() > 0
    assert np.all(grids[0][parts == 'dark'] < grids[0][parts == 'shutter'].min())


def test_squares_wrap(layouts):
    for spec, _, grids, labels in layouts.values():
        assert np.array_equal(grids[0][:, 0], grids[0][:, -1])
        assert np.array_equal(labels[0], labels[-1])
