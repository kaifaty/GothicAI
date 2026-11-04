"""nav-sim stand ground motion (scripts/navsim/stand/motion.py) on a synthetic scene, without game data."""
from pathlib import Path
import struct
import sys

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "navsim"))
from navsim.stand import eye, motion  # noqa: E402
import motion_table  # noqa: E402

pytestmark = pytest.mark.skipif(
    eye.library_path() is None,
    reason="navsim stand library not built (scripts/navsim/stand/native, or NAVSIM_STAND_LIB)")

EPOCH = 1_000_000_000
RUN, WALK, IDLE = (1, 0, 0, 0), (1, 0, 0, 1), (0, 0, 0, 0)
LEFT, RIGHT = (0, 0, -1, 0), (0, 0, 1, 0)  # turn < 0 increases the yaw (npckeys.cpp)
RUN_CM, START_CM, AFTER_IDLE_CM = 34.5, 10.0, 12.0
PADDING = 1.5  # feet above the support: CharacterVirtual keeps a 1 cm padding
FLOOR = 5000
# Lanes along +X from x = 0 (z of each lane); obstacles start at x = 300.
FREE, WALL, LOW, HIGH, STEEP, RAMP = 0, 1000, -1500, -2500, 3000, -3500


def quad(a, b, c, d, category):
    return [((a, b, c), category), ((a, c, d), category)]


def top(x0, x1, y, z0, z1, category):
    """Horizontal quad, front face up (as the eye test's floor)."""
    return quad((x0, y, z0), (x0, y, z1), (x1, y, z1), (x1, y, z0), category)


def box(x0, x1, y1, z0, z1, category=2):
    faces = top(x0, x1, y1, z0, z1, category)
    faces += quad((x0, 0, z0), (x0, y1, z0), (x0, y1, z1), (x0, 0, z1), category)
    faces += quad((x1, 0, z0), (x1, 0, z1), (x1, y1, z1), (x1, y1, z0), category)
    faces += quad((x0, 0, z0), (x1, 0, z0), (x1, y1, z0), (x0, y1, z0), category)
    faces += quad((x0, 0, z1), (x0, y1, z1), (x1, y1, z1), (x1, 0, z1), category)
    return faces


def ramp(x0, degrees, height, z0, z1, category=1):
    x1 = x0 + height / np.tan(np.radians(degrees))
    return quad((x0, 0, z0), (x0, 0, z1), (x1, height, z1), (x1, height, z0), category) + \
        top(x1, x1 + 400, height, z0, z1, category)


def scene(path):
    triangles = top(-FLOOR, FLOOR, 0, -FLOOR, FLOOR, 1)
    triangles += box(300, 320, 300, 500, 2500)                 # wall
    triangles += box(300, 600, 30, LOW - 200, LOW + 200)       # step below the 60 cm limit
    triangles += box(300, 600, 90, HIGH - 200, HIGH + 200)     # step above it
    triangles += ramp(300, 60, 300, STEEP - 200, STEEP + 200)  # steeper than the 50 deg slide angle
    triangles += ramp(300, 30, 300, RAMP - 300, RAMP + 300)    # walkable
    triangles += top(-FLOOR, FLOOR, -40, -FLOOR, FLOOR, 3)     # water is not solid: no effect below the floor
    vertices = [c for (tri, _) in triangles for v in tri for c in v]
    path.write_bytes(b"NAVSTAT1" + struct.pack(f"<II{len(vertices)}f{len(triangles)}B", len(triangles), 0,
                                               *vertices, *(c for _, c in triangles)))
    return path


def table():
    """Current keys alone (level -1), a start of the run (level 0, age 0) and a run after idle (level 1)."""
    current = [(RUN, RUN_CM, 0), (WALK, 15, 0), ((0, 1, 0, 0), 0, 20), ((1, 0, -1, 0), RUN_CM, 0),
               ((1, 0, 1, 0), RUN_CM, 0), (IDLE, 0, 0), (LEFT, 0, 0), (RIGHT, 0, 0)]
    levels = {-1: ([int(motion.codes(k)) for k, *_ in current], [v for _, *v in current])}
    start = motion_table.key_ids(np.array([RUN]))[0]
    after_idle = motion_table.key_ids(np.array([IDLE, RUN]))[1]
    levels[0] = ([start[1]], [[START_CM, 0]])
    levels[1] = ([after_idle[2]], [[AFTER_IDLE_CM, 0]])
    return {level: (np.array(k, np.int64), np.array(v, np.float32)) for level, (k, v) in levels.items()}


def world(path, agents, threads=2, shards=None):
    path.mkdir(parents=True, exist_ok=True)
    w = eye.EyeWorld(scene(path / "static.bin"), agents=agents, threads=threads)
    return w, motion.Motion(w, table=table(), shards=shards)


def place(m, lanes, yaw=None, history=None, held=None):
    n = len(lanes)
    position = np.stack([np.zeros(n), np.zeros(n), np.asarray(lanes, float)], 1)
    m.set_state(position, np.zeros(n) if yaw is None else yaw, np.full(n, EPOCH, np.uint64), history, held)


def drive(m, keys, decisions):
    m.set_keys(np.asarray(keys, np.int8))
    for _ in range(decisions):
        out = m.step()
    return out


def test_floor_run_ticks_and_turn(tmp_path):
    w, m = world(tmp_path, agents=3)
    with w:
        place(m, [FREE, FREE + 200, FREE + 400])
        m.set_keys(np.array([RUN, LEFT, RIGHT], np.int8))
        out = m.step()
        state = m.read()
        # A fresh run starts with the level-0 entry (age 0), then the current keys alone.
        assert state.level.tolist() == [0, -1, -1]
        np.testing.assert_allclose(state.table_cm[0], [START_CM, 0])
        assert out.ms.tolist() == [[EPOCH + 33, EPOCH + 66, EPOCH + 100]] * 3
        np.testing.assert_allclose(out.position[0, :, 0], np.array([33, 66, 100]) * START_CM / 100, atol=0.05)
        np.testing.assert_allclose(out.limits[0], [[60, 50, 180]] * 3, atol=1e-4)
        assert np.all(out.flags & motion.SUPPORTED) and not np.any(out.flags & (motion.BLOCKED | motion.FALLING))
        for _ in range(9):
            out = m.step()
        state = m.read()
        np.testing.assert_allclose(state.position[0, [0, 2]], [START_CM + 9 * RUN_CM, FREE], atol=0.3)
        assert state.level[0] == -1
        # 90 deg/s without a weapon, in the engine's sign: turn < 0 increases the yaw.
        np.testing.assert_allclose(state.yaw, [0, 90, -90], atol=1e-3)
        np.testing.assert_allclose(out.forward[1, -1], [0, 0, 1], atol=1e-5)
        np.testing.assert_allclose(state.position[1:, [0, 2]], [[0, FREE + 200], [0, FREE + 400]], atol=0.05)
        assert np.all(np.abs(state.position[:, 1]) < PADDING)
        # The first decision of an episode: one tick, right-aligned; the others are zeroed.
        place(m, [FREE, FREE + 200, FREE + 400])
        out = m.step(ticks=[1, 3, 0])
        assert out.ms[0].tolist() == [0, 0, EPOCH + 33] and out.ms[2].tolist() == [0, 0, 0]
        assert out.flags[0, :2].tolist() == [0, 0] and out.flags[0, 2] & motion.SUPPORTED
        assert m.read().game_ms.tolist() == [EPOCH + 33, EPOCH + 100, EPOCH]


def test_history_levels_and_unknown_keys(tmp_path):
    w, m = world(tmp_path, agents=3)
    with w:
        history = np.full((3, 3, 4), -1, np.int8)
        history[:, 0] = IDLE
        place(m, [FREE, FREE + 200, FREE + 400], history=history, held=[5, 5, 5])
        m.set_keys(np.array([RUN, WALK, (-1, -1, 0, 0)], np.int8))
        m.step()
        state = m.read()
        assert state.level.tolist() == [1, -1, -2]
        np.testing.assert_allclose(state.table_cm[:, 0], [AFTER_IDLE_CM, 15, 0])
        assert state.flags[2] & motion.UNKNOWN_KEYS and not state.flags[0] & motion.UNKNOWN_KEYS
        np.testing.assert_allclose(state.position[2, [0, 2]], [0, FREE + 400], atol=0.05)
        # The history helper is motion_table.key_ids' view of a recorded sequence.
        keys = np.array([IDLE, IDLE, RUN, RUN, RUN, WALK], np.int8)
        hist, held = motion.history_at(keys, [0, 3, 5])
        assert hist[0, :, 3].tolist() == [-1, -1, -1] and held.tolist() == [0, 0, 2]
        assert hist[1].tolist() == [list(RUN), list(IDLE), list(IDLE)]


def test_wall_stops_and_slides(tmp_path):
    w, m = world(tmp_path, agents=2)
    with w:
        place(m, [WALL, WALL], yaw=[0, 30])
        out = drive(m, [RUN, RUN], 20)
        state = m.read()
        # Head-on: the capsule (radius 8.2 cm) rests against x = 300 and reports the block.
        assert 285 < state.position[0, 0] < 300 - 8.17 + 0.5
        assert abs(state.position[0, 2] - WALL) < 1 and abs(state.position[0, 1]) < PADDING
        assert np.all(out.flags[0] & motion.BLOCKED)
        # Oblique: slides along the wall, keeping the tangential part of the run.
        assert state.position[1, 0] < 300 - 8.17 + 0.5 and state.position[1, 2] > WALL + 250
        assert not np.any(out.flags[1] & motion.BLOCKED)


def test_steps_and_slopes(tmp_path):
    w, m = world(tmp_path, agents=4)
    with w:
        place(m, [LOW, HIGH, STEEP, RAMP])
        stepped = np.zeros(4, bool)
        m.set_keys(np.array([RUN] * 4, np.int8))
        for _ in range(15):
            out = m.step()
            stepped |= np.any(out.flags & motion.STEPPED, axis=1)
        state = m.read()
        low, high, steep, walkable = state.position
        assert stepped[0] and abs(low[1] - 30) < PADDING and 400 < low[0] < 600  # onto the 30 cm step
        assert high[0] < 300 - 8.17 + 0.5 and abs(high[1]) < PADDING        # the 90 cm step stops it
        assert np.all(out.flags[1] & motion.BLOCKED)
        assert steep[1] < 20 and steep[0] < 330                               # 60 deg: no way up
        assert walkable[1] > 50 and np.all(out.flags[3] & motion.SUPPORTED)   # 30 deg: walks up
        np.testing.assert_allclose(walkable[1], (walkable[0] - 300) * np.tan(np.radians(30)), atol=2)


def test_threads_and_shards_do_not_change_motion(tmp_path):
    lanes = [FREE, WALL, LOW, HIGH, STEEP, RAMP, FREE + 300]
    keys = np.array([RUN, (1, 0, -1, 0), RUN, (1, 1, 0, 0), RUN, (1, 0, 1, 0), LEFT], np.int8)
    results = []
    for threads, shards in ((1, 1), (3, 5)):
        w, m = world(tmp_path / f"{threads}-{shards}", agents=len(lanes), threads=threads, shards=shards)
        with w:
            assert m.shards == shards
            place(m, lanes, yaw=np.arange(len(lanes)) * 7.0)
            m.set_keys(keys)
            ticks = [m.step() for _ in range(12)]
            results.append((np.stack([t.position for t in ticks]), np.stack([t.flags for t in ticks]), m.read().yaw))
    for a, b in zip(*results):
        np.testing.assert_array_equal(a, b)


def test_bad_input_is_rejected(tmp_path):
    w, m = world(tmp_path, agents=1)
    with w:
        with pytest.raises(RuntimeError, match="keys"):
            m.set_keys(np.array([[2, 0, 0, 0]], np.int8))
        with pytest.raises(RuntimeError, match="range"):
            m.set_keys(np.array([RUN, RUN], np.int8))
        with pytest.raises(RuntimeError, match="ticks"):
            m.step(ticks=[4])
