import numpy as np
import pytest

import piece_bake as pb


def test_copies_reach_across_the_edges():
    period = (10., 5.)
    assert pb.copies((4, 2, 5, 3), period, .3) == [[0., 0.]]
    corner = pb.copies((-.2, 4.9, .5, 5.4), period, .3)
    assert sorted(map(tuple, corner)) == [(0., -5.), (0., 0.), (10., -5.), (10., 0.)]


def test_piece_keeps_one_ring_without_repeats():
    from shapely.geometry import Polygon, box
    ring = [(0, 0), (1, 0), (1, 0.000001), (1, 1), (0, 1)]
    piece = pb.piece(3, Polygon(ring), [0, 0, 0], [.01, 0, 0], 0, [0, 0, 0], kind='test')
    assert piece['id'] == 3 and len(piece['outline']) == 4
    xs, ys = np.array(piece['outline']).T
    assert .5 * np.sum(xs * np.roll(ys, -1) - np.roll(xs, -1) * ys) > 0       # counter-clockwise
    with pytest.raises(ValueError, match='hole inside'):
        pb.piece(4, box(0, 0, 1, 1).difference(box(.4, .4, .6, .6)), [0, 0, 0], [.01, 0, 0], 0, [0, 0, 0])


def test_mesh_points_stay_inside_the_visible_part():
    import shapely
    from shapely.geometry import box
    shape = box(0, 0, 1, 2)
    points = pb.mesh_points(shape, box(-9, -9, 9, 1), across=.02, along=.05, angle=.05)
    assert len(points) > 300
    assert shapely.contains_xy(shape.buffer(1e-3), points[:, 0], points[:, 1]).all()
    assert points[:, 1].max() <= 1 + 1e-3


def test_periodic_noise_wraps():
    field = pb.periodic_noise(np.random.default_rng(1), (64, 128), 8, 4)
    assert field.shape == (64, 128) and abs(field.std() - 1) < 1e-6
    step = np.abs(np.diff(field, axis=1)).mean()
    assert np.abs(field[:, 0] - field[:, -1]).mean() < 2 * step
    assert np.abs(field[0] - field[-1]).mean() < 2 * np.abs(np.diff(field, axis=0)).mean()


def test_seamless_along_wraps_top_to_bottom(tmp_path):
    from PIL import Image
    rng = np.random.default_rng(2)
    image = (np.linspace(0, 255, 100)[:, None, None] + rng.normal(0, 3, (100, 40, 3))).clip(0, 255).astype(np.uint8)
    Image.fromarray(image).save(tmp_path / 'in.png')
    out = np.asarray(Image.open(pb.seamless_along(tmp_path / 'in.png', tmp_path / 'out.png')), dtype=float)
    assert np.abs(out[0] - out[-1]).mean() < 10 < np.abs(image[0].astype(float) - image[-1]).mean()
    np.testing.assert_allclose(out[50], image[50], atol=1)          # the middle is the source


def test_overhang_shade_falls_below_a_step():
    height = np.zeros((40, 4))
    height[:20] = .03                    # rows above (towards the ridge) are 3 cm higher
    shade = pb.overhang_shade(height, .005)
    column = shade[:, 0]
    assert column[20:24].min() == 1 and column[:20].max() == 0       # under the step, not on it
    assert np.all(np.diff(column[20:]) <= 1e-9) and column[-1] < .1   # fading as the step gets far


def test_grid_piece_heights_go_to_the_npz(tmp_path):
    heights = np.linspace(0, -.01, 12).reshape(3, 4)
    grid = pb.grid_piece(5, (0, .2), (.01, .02), [0, 0, 0], 1, [0, 0, 0], [[0, 0]], kind='face')
    assert grid['grid'] == [0., .2, .01, .02] and 'outline' not in grid
    layout = tmp_path / 'layout.json'
    pb.write_layout(layout, dict(pieces=[grid]), {}, {5: heights})
    stored = np.load(layout.with_suffix('.npz'))
    assert np.allclose(stored['h5'], heights)
