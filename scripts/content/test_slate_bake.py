import numpy as np
import pytest

import slate_bake


@pytest.fixture(scope='module')
def built():
    return slate_bake.layout(seed=1)


@pytest.fixture(scope='module')
def spec(built):
    return built[0]


def test_layout_is_reproducible_and_complete(built, spec):
    slates = spec['pieces']
    assert len(slates) == slate_bake.ACROSS * slate_bake.COURSES
    again, _ = slate_bake.layout(seed=1)
    assert [s['outline'] for s in again['pieces']] == [s['outline'] for s in slates]
    low, high = slate_bake.THICKNESS
    assert all(low <= s['thickness'] <= high for s in slates)
    assert all(abs(s['top'][0] - s['bottom'][0] - s['thickness']) < 1e-9 for s in slates)
    assert any(s['broken'] for s in slates) and not all(s['broken'] for s in slates)
    assert all(len(built[1][s['id']]) > 50 for s in slates)       # mesh points of every face


def test_slates_with_their_copies_cover_the_square(spec):
    from shapely import affinity, unary_union
    from shapely.geometry import Polygon, box
    period = spec['period'][0]
    pieces = [affinity.translate(Polygon(s['outline']), dx, dy) for s in spec['pieces'] for dx, dy in s['copies']]
    hole = box(0, 0, period, period).difference(unary_union(pieces))
    assert hole.area < 1e-6 * period ** 2


def test_slates_overlap_their_neighbours_and_the_course_above(spec):
    from shapely.geometry import Polygon
    by_place = {(s['course'], s['index_in_course']): Polygon(s['outline']) for s in spec['pieces']}
    a, b = by_place[(3, 4)], by_place[(3, 5)]
    assert a.intersects(b) and a.intersection(b).area > 0        # the side lap
    above = [p for (c, _), p in by_place.items() if c == 4]
    assert any(a.intersection(p).area > .01 for p in above)        # the head lies under the course above


def test_joint_is_steep_at_the_top_and_sweeps_right_at_the_tail():
    v = np.linspace(0, 1, 11)
    x = slate_bake.joint(0., 1., 1., v)
    slope = np.diff(x)
    assert x[0] == 0 and np.all(slope > 0) and slope[-1] > 2 * slope[0]
    assert slate_bake.joint(0., 1., 1., -.5) == pytest.approx(-.5 * slate_bake.CURVE)


def test_drip_trails_start_dark_and_stay_in_range(spec):
    field = slate_bake.drip_trails(spec, 256, np.random.default_rng(3))
    assert field.shape == (256, 256) and field.min() >= 0 and field.max() <= 1
    assert (field > .2).mean() > .001


def test_local_coords_inside_a_visible_slate(spec):
    slate = spec['pieces'][40]
    v = .5
    x = (slate_bake.joint(*slate['joint_left'], slate['height'], v)
         + slate_bake.joint(*slate['joint_right'], slate['height'], v)) / 2
    y = slate['y_bottom'] + slate['height'] * (1 - v)
    u, v_out, width = slate_bake.local_coords(spec, np.array([[slate['id']]]), np.array([[x]]), np.array([[y]]))
    assert u[0, 0] == pytest.approx(.5) and v_out[0, 0] == pytest.approx(.5) and width[0, 0] > .2
