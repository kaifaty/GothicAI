import numpy as np
import pytest

import shingle_bake as sb


@pytest.fixture(scope='module')
def built():
    return sb.layout(seed=1)


def shingles(spec):
    return [p for p in spec['pieces'] if p['kind'] == 'shingle']


def test_layout_is_reproducible_and_counted(built):
    spec, points = built
    again, _ = sb.layout(seed=1)
    assert [p['outline'] for p in again['pieces']] == [p['outline'] for p in spec['pieces']]
    courses = {p['course'] for p in shingles(spec)}
    assert courses == set(range(sb.COURSES))
    per_course = [len({p['index_in_course'] for p in shingles(spec) if p['course'] == c}) for c in courses]
    assert min(per_course) >= sb.ACROSS - 2 and max(per_course) == sb.ACROSS     # holes take a few
    assert all(len(points[p['id']]) > 100 for p in shingles(spec) if p['id'] in points)
    kinds = [p['kind'] for p in spec['pieces']]
    assert kinds.count('verge') == 2 and kinds.count('ridge') == 1 and kinds.count('rafter') == sb.RAFTER[3]


def covered(spec, x, y, kinds=('shingle', 'ridge', 'verge')):
    import shapely
    from shapely import affinity, unary_union
    from shapely.geometry import Polygon
    parts = [affinity.translate(Polygon(p['outline']), dx, dy) for p in spec['pieces'] if p['kind'] in kinds
             for dx, dy in p['copies']]
    return shapely.contains_xy(unary_union(parts), x, y)


def test_holes_where_the_original_has_them_and_nowhere_else(built):
    spec, _ = built
    px, py = spec['period']
    for u0, u1, v0, v1, _ in sb.HOLES:
        assert not covered(spec, (u0 + u1) / 2 * px, py * (1 - (v0 + v1) / 2))
    rng = np.random.default_rng(5)
    x, y = rng.uniform(0, px, 400), rng.uniform(0, py, 400)
    near = np.zeros(400, bool)
    for u0, u1, v0, v1, _ in sb.HOLES:
        near |= (x > u0 * px - .3) & (x < u1 * px + .3) & (y > py * (1 - v1) - .3) & (y < py * (1 - v0) + .4)
    assert covered(spec, x[~near], y[~near]).mean() > .995       # only cracks and keyways show through


def test_the_big_hole_shows_a_rafter_and_a_lath(built):
    spec, _ = built
    px, py = spec['period']
    u0, u1, v0, v1, _ = sb.HOLES[0]
    xs = np.linspace(u0 * px + .05, u1 * px - .05, 40)
    ys = np.linspace(py * (1 - v1) + .05, py * (1 - v0) - .05, 60)
    gx, gy = np.meshgrid(xs, ys)
    open_ = ~covered(spec, gx.ravel(), gy.ravel())
    timber = covered(spec, gx.ravel(), gy.ravel(), kinds=('lath', 'rafter'))
    assert open_.mean() > .5 and (open_ & timber).mean() > .05


def test_every_course_stands_on_the_one_below_by_its_butt(built):
    spec, _ = built
    for p in shingles(spec):
        a, bx, by = p['top']
        c, dx, dy = p['bottom']
        x = np.mean([q[0] for q in p['outline']])
        y = p['y_bottom']
        assert sb.THICK_BUTT[0] - 1e-9 <= (a + bx * x + by * y) - (c + dx * x + dy * y) <= sb.THICK_BUTT[1] + 1e-9
        assert c + dx * x + dy * y > .02                      # lifted by the courses under it


def test_compose_darkens_the_top_of_a_face_and_keeps_holes_black():
    spec = dict(period=[4., 4.], pieces=[dict(id=0, kind='shingle', outline=[[0, 1], [4, 1], [4, 2], [0, 2]],
                                              y_bottom=1., height=1., course=0, joint=[1., 0.])])
    rows, cols = 64, 128
    piece = np.zeros((rows, cols), int)
    piece[:, :8] = -1                                                   # a hole on the left
    y = np.repeat(np.linspace(1.99, 1.01, rows)[:, None], cols, axis=1)  # rows run down the slope
    passes = dict(piece=piece, y=y, x=np.tile(np.linspace(0, 2, cols), (rows, 1)),
                  colour=np.full((rows, cols, 3), .3), sky=np.ones((rows, cols)), crevice=np.ones((rows, cols)),
                  height=np.zeros((rows, cols)), normal=np.dstack([np.zeros((rows, cols))] * 2 + [np.ones((rows, cols))]))
    result = sb.compose(passes, spec, seed=1)
    face = result['albedo'][:, 8:].mean(axis=(1, 2))
    assert face[:8].mean() < .3 * face[-8:].mean()                     # dark under the course above
    assert result['albedo'][:, :8].max() < .01
    assert result['roughness'].min() >= sb.WET_ROUGH - 1e-9
