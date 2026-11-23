import math

import numpy as np

import harbour_life


class FlatYard(harbour_life.Ground):
    """A floor at y=0 from x -1000..1000, z -1000..1000 and a wall 3 m tall along z=500 (x -1000..1000)."""

    def __init__(self):
        floor = [[[-1000, 0, -1000], [1000, 0, -1000], [1000, 0, 1000]],
                 [[-1000, 0, -1000], [1000, 0, 1000], [-1000, 0, 1000]]]
        wall = [[[-1000, 0, 500], [1000, 0, 500], [1000, 300, 500]],
                [[-1000, 0, 500], [1000, 300, 500], [-1000, 300, 500]]]
        self.tris = np.array(floor + wall, dtype=float)
        self.waypoints = np.array([[0.0, 50.0, 0.0]])
        self.grid = {}
        lo = self.tris[..., [0, 2]].min(axis=1)
        hi = self.tris[..., [0, 2]].max(axis=1)
        for i, (a, b) in enumerate(zip(np.floor(lo / harbour_life.CELL).astype(int), np.floor(hi / harbour_life.CELL).astype(int))):
            for gx in range(a[0], b[0] + 1):
                for gz in range(a[1], b[1] + 1):
                    self.grid.setdefault((gx, gz), []).append(i)


class Boxes:
    def box(self, name):
        return {'CRATE': (np.array([-60.0, -50, -50]), np.array([60.0, 50, 50])),
                'LINE': (np.array([-180.0, 0, -30]), np.array([180.0, 230, 30]))}[name]


def test_an_item_stands_on_the_floor_and_a_crate_on_a_crate():
    entries, _ = harbour_life.place([
        {'key': 'a', 'visual': 'CRATE', 'x': 0, 'z': 0, 'yaw': 10, 'sink': 2},
        {'key': 'b', 'visual': 'CRATE', 'x': 10, 'z': 5, 'yaw': 30, 'on': 'a'},
    ], FlatYard(), Boxes())
    a, b = entries
    assert a['position'] == [0.0, 48.0, 0.0] and a['yaw_deg'] == 10.0 and a['collision'] == 'mesh'
    assert b['position'][1] == 148.0  # on a's top (98) plus its own half height


def test_against_the_wall_the_item_runs_along_it_at_the_gap():
    entries, _ = harbour_life.place([{'key': 'w', 'visual': 'CRATE', 'x': 100, 'z': 300, 'wall': 5, 'yaw': 0}],
                                    FlatYard(), Boxes())
    (w,) = entries
    # the wall runs along x at z=500: the crate's local X along it, its far side 5 cm off the wall
    assert abs(w['position'][0] - 100) < 1e-6
    assert abs(w['position'][2] - (500 - 5 - 50)) < 1e-6
    assert w['yaw_deg'] % 180 == 0


def test_rotation_rows_match_the_layer_yaw():
    # voblayer.cpp: yaw writes rows [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    m = harbour_life.rotation(30, (0, 0))
    c, s = math.cos(math.radians(30)), math.sin(math.radians(30))
    assert np.allclose(m, [[c, 0, s], [0, 1, 0], [-s, 0, c]])
    lean = harbour_life.rotation(0, (0, 8))
    assert np.allclose(lean @ lean.T, np.eye(3)) and lean[1, 1] < 1
