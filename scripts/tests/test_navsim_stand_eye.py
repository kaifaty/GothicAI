"""nav-sim stand eye (scripts/navsim/stand/eye.py) on a synthetic box, without game data."""
from pathlib import Path
import struct
import sys

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "navsim" / "gpumem"))
from navsim.stand import eye  # noqa: E402
import gpumem  # noqa: E402

pytestmark = pytest.mark.skipif(
    eye.library_path() is None,
    reason="navsim stand library not built (scripts/navsim/stand/native, or NAVSIM_STAND_LIB)")

EPOCH = 1_000_000_000


def quad(a, b, c, d, category):
    """Two triangles, front face (right-handed, as Embree) towards the viewer of a->b->c."""
    return [((a, b, c), category), ((a, c, d), category)]


def box(path):
    """Floor, a one-sided wall in front, a water strip and a door to the right."""
    triangles = []
    triangles += quad((-2000, 0, -2000), (-2000, 0, 2000), (2000, 0, 2000), (2000, 0, -2000), 1)
    triangles += quad((800, 0, -2000), (800, 0, 2000), (800, 1000, 2000), (800, 1000, -2000), 2)
    triangles += quad((200, 20, -2000), (200, 20, 2000), (400, 20, 2000), (400, 20, -2000), 3)
    triangles += quad((-200, 0, 800), (-200, 400, 800), (200, 400, 800), (200, 0, 800), 4)
    vertices = [c for (tri, _) in triangles for v in tri for c in v]
    path.write_bytes(b"NAVSTAT1" + struct.pack(f"<II{len(vertices)}f{len(triangles)}B", len(triangles), 0,
                                               *vertices, *(c for _, c in triangles)))
    return path


def ray(yaw, pitch):
    """Index of a layout-250 ray: 17 yaws (sorted) x 14 pitches from -70 by 10."""
    yaws = sorted([-60 + 10 * i for i in range(13)] + [-80, 80, -100, 100])
    return yaws.index(yaw) * 14 + (pitch + 70) // 10


def pose(world, x, yaw, ms, first=0):
    n = len(x)
    position = np.stack([np.asarray(x, np.float32), np.zeros(n), np.zeros(n)], 1)
    world.set_poses_yaw(position + [0, 180, 0], position, yaw, np.asarray(ms, np.uint64) + EPOCH, first)


def test_box_hits_kinds_backfaces_and_cadence(tmp_path):
    static = box(tmp_path / "static.bin")
    with eye.EyeWorld(static, agents=2, threads=2) as world:
        assert world.triangles == 8
        # Agent 0 inside looks at the wall; agent 1 stands behind the wall and looks back.
        pose(world, [0, 1500], [0, 180], [0, 0])
        out = eye.Slots.empty(2)
        step = world.update(out)
        assert step.updated.tolist() == [1, 1] and step.far_updated.tolist() == [1, 1]
        seen = {name: out.distance[0, ray(*angles)] for name, angles in
                (("wall", (0, 0)), ("floor", (0, -70)), ("water", (0, -30)), ("door", (80, 0)))}
        np.testing.assert_allclose(list(seen.values()), [800, 180 / np.sin(np.radians(70)), 320,
                                                         800 / np.sin(np.radians(80))], rtol=1e-5)
        kinds = [eye.KINDS[out.kind[0, ray(*a)]] for a in ((0, 0), (0, -70), (0, -30), (80, 0))]
        assert kinds == ["static_vob", "world", "water", "movable_vob"]
        np.testing.assert_allclose(out.normal_y[0, [ray(0, 0), ray(0, -70), ray(0, -30)]], [0, 1, 1], atol=1e-6)
        np.testing.assert_allclose(out.point[0, ray(0, 0)], [800, 180, 0], atol=1e-3)
        # The wall's back face is invisible: agent 1's forward ray flies through to its full range.
        assert not out.hit[1, ray(0, 0)] and out.complete[1, ray(0, 0)] and out.distance[1, ray(0, 0)] == 10000

        over = ray(0, 50)  # passes over the wall: near miss, then a far miss
        assert out.complete[0, over] and not out.hit[0, over] and out.time[0, eye.RAYS + over] == EPOCH
        pose(world, [0, 1500], [0, 180], [50, 50])
        assert world.update(out).updated.tolist() == [0, 0]
        pose(world, [0, 1500], [0, 180], [100, 100])  # standing: no walking gaze
        step = world.update(out)
        assert step.updated.tolist() == [1, 1] and step.far_updated.tolist() == [0, 0]
        assert not out.complete[0, over] and out.distance[0, over] == 1000 and out.time[0, over] == EPOCH + 100
        assert out.time[0, eye.RAYS + over] == EPOCH and out.complete[0, eye.RAYS + over]
        np.testing.assert_array_equal(world.read(1, 1).distance[0], out.distance[1])
        pose(world, [0, 1500], [0, 180], [500, 500])
        assert world.update(out).far_updated.tolist() == [1, 1]
        assert out.time[0, eye.RAYS + over] == EPOCH + 500

        # Slots are exactly the gpumem eye inputs for N agents and 476 slots.
        expected = {name: (np.dtype(dtype), count) for name, dtype, count in gpumem.layout(2, eye.SLOTS, 1)["eye"]}
        for name, array in out.gpumem().items():
            assert (array.dtype, array.size) == expected[name], name


def test_pool_threads_do_not_change_slots(tmp_path):
    static = box(tmp_path / "static.bin")
    slots = []
    for threads in (1, 3):
        with eye.EyeWorld(static, agents=7, threads=threads) as world:
            out = eye.Slots.empty(7)
            for t in range(8):
                pose(world, np.arange(7) * 40.0 + t * 12, np.arange(7) * 25.0 - 60 + t, [t * 100] * 7)
                world.update(out)
            slots.append(out)
    for name in ("distance", "point", "kind", "hit", "time", "complete", "direction"):
        np.testing.assert_array_equal(getattr(slots[0], name), getattr(slots[1], name), name)
