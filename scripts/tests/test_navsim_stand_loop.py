"""StepBatch contract and the capture replay of the nav-sim stand, on the CPU."""
from pathlib import Path
import sys

import numpy as np
import pytest

NAVSIM = Path(__file__).resolve().parents[1] / "navsim"
sys.path.insert(0, str(NAVSIM / "stand"))
sys.path.insert(0, str(NAVSIM / "gpumem"))
import captures  # noqa: E402
import contract  # noqa: E402
import cpu_replay  # noqa: E402
import gpumem  # noqa: E402


def drive_like(path, decisions=7, seed=3):
    """Shaped like the drive records: one tick with the eye, then three ticks with
    the eye on the last, a readout after each eye tick and two trailing ticks."""
    rng = np.random.default_rng(seed)
    S = contract.SLOTS
    pattern = [(True, True)] + [(False, False), (False, False), (True, True)] * (decisions - 1) + [(False, False)] * 2
    poses = np.zeros(len(pattern), captures.POSE)
    begin, slot, changed, views = [0], [], [], []
    ms = 5000
    for u, (eye, view) in enumerate(pattern):
        ms += 33 + (u % 3 == 0)
        poses[u]["position"] = rng.normal(0, 500, 3)
        poses[u]["forward"] = (1, 0, 0)
        poses[u]["ms"], poses[u]["step"], poses[u]["slope"], poses[u]["body"] = ms, 60, 50, 180
        refreshed = np.arange(S) if u == 0 else np.sort(rng.choice(S, 40, replace=False)) if eye else np.arange(0)
        rays = np.zeros(len(refreshed), captures.RAY)
        rays["origin"] = rng.normal(0, 100, (len(refreshed), 3))
        rays["point"] = rng.normal(0, 900, (len(refreshed), 3))
        rays["distance"] = rng.uniform(10, 1000, len(refreshed))
        rays["normal_y"] = rng.uniform(-1, 1, len(refreshed))
        rays["kind"] = rng.integers(0, 6, len(refreshed))
        rays["hit"] = rays["kind"] != 0
        rays["time"] = ms
        slot.append(refreshed.astype(np.uint16))
        changed.append(rays)
        begin.append(begin[-1] + len(refreshed))
        views.append(int(view))
    capture = captures.Capture(str(path), np.zeros(0, captures.TILE), np.zeros(0, np.uint64), S, poses,
                               np.array(begin, np.int64), np.concatenate(slot), np.concatenate(changed),
                               np.array(views, np.int32))
    captures.write(path, capture)
    return captures.read(path)


def test_layout_matches_gpumem_slots():
    n = 5
    fields, total = contract.layout(n)
    ends = 0
    for f in fields.values():
        assert f.offset % contract.ALIGN == 0 and f.offset >= ends
        ends = f.offset + f.nbytes
    assert total >= ends
    eye = {name: (np.dtype(dtype), count) for name, dtype, count in gpumem.layout(n, contract.SLOTS, 1)["eye"]}
    for name, slot in contract.RAY_SLOTS.items():
        dtype, count = eye[slot]
        assert fields[name].dtype == dtype and fields[name].nbytes == dtype.itemsize * count
    batch = contract.StepBatch(n)
    assert batch.ray_origin.shape == (n, contract.SLOTS, 3) and batch.pose_ms.shape == (n, contract.TICKS)
    assert batch.vector.shape == (n, contract.VECTOR) and batch.reward.shape == (n, len(contract.REWARDS))
    batch.vector[2, 7] = 1.5
    view = batch.raw[fields["vector"].offset:fields["vector"].offset + fields["vector"].nbytes].view(np.float32)
    assert view[2 * contract.VECTOR + 7] == 1.5


def test_replay_follows_the_capture(tmp_path):
    records = [drive_like(tmp_path / "a.bin"), drive_like(tmp_path / "b.bin", decisions=4, seed=5)]
    replay = cpu_replay.Replay(records)
    assert replay.count.tolist() == [7, 4]
    side = cpu_replay.ReplaySide(replay, [0, 1], shift=False)
    batch = side.reset()
    batch.check()
    assert batch.first.tolist() == [1, 1] and batch.ticks.tolist() == [1, 1]
    capture = records[0]
    for d in range(1, 7):
        batch = side.step(np.zeros((2, contract.HEADS), np.uint8))
        batch.check()
        last = 3 * d  # the decision's eye tick in update numbers
        rays = capture.slots_after(last)
        np.testing.assert_array_equal(batch.ray_point[0], rays["point"])
        np.testing.assert_array_equal(batch.ray_time[0], rays["time"].astype(np.uint32))
        np.testing.assert_array_equal(batch.pose_ms[0], capture.poses["ms"][last - 2:last + 1])
        np.testing.assert_array_equal(batch.pose_limits[0, 2], [60, 50, 180])
        assert batch.ticks[0] == 3 and batch.first[0] == 0
        # The second capture has four decisions: it wraps at step 4 and reports truncated.
        assert batch.truncated[1] == batch.first[1] == (d % 4 == 0)
    np.testing.assert_array_equal(batch.vector[0], replay.vectors[side.decision[0] % cpu_replay.POOL])


def test_shifted_starts_and_parallel_fill_agree(tmp_path):
    records = [drive_like(tmp_path / "a.bin", decisions=9), drive_like(tmp_path / "b.bin", decisions=6, seed=8)]
    replay = cpu_replay.Replay(records)
    one = cpu_replay.ReplaySide(replay, range(10))
    many = cpu_replay.ReplaySide(replay, range(10), workers=3)
    assert len(set(one.start.tolist())) > 2
    for step in range(12):
        a = one.reset() if step == 0 else one.step(None)
        b = many.reset() if step == 0 else many.step(None)
        a.check()
        np.testing.assert_array_equal(a.raw, b.raw)
    many.close()


def test_check_rejects_broken_batches():
    batch = contract.StepBatch(2)
    batch.ticks[:] = 3
    batch.check()
    batch.first[0] = 1
    with pytest.raises(ValueError, match="one tick"):
        batch.check()
    batch.first[0], batch.truncated[1] = 0, 1
    with pytest.raises(ValueError, match="restart"):
        batch.check()
    batch.truncated[1] = 0
    batch.pose_ms[0] = (30, 20, 40)
    with pytest.raises(ValueError, match="time goes back"):
        batch.check()
