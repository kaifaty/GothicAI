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
import loop  # noqa: E402


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
    finished = contract.FinishBatch(n)
    assert finished.vector.shape == (n, contract.VECTOR) and batch.reward.shape == (n, len(contract.REWARDS))
    finished.vector[2, 7] = 1.5
    f = finished.fields["vector"]
    view = finished.raw[f.offset:f.offset + f.nbytes].view(np.float32)
    assert view[2 * contract.VECTOR + 7] == 1.5


def test_replay_follows_the_capture(tmp_path):
    records = [drive_like(tmp_path / "a.bin"), drive_like(tmp_path / "b.bin", decisions=4, seed=5)]
    replay = cpu_replay.Replay(records)
    assert replay.count.tolist() == [7, 4]
    side = cpu_replay.ReplaySide(replay, [0, 1], shift=False)
    batch = side.reset()
    batch.check()
    assert batch.first.tolist() == [1, 1] and batch.ticks.tolist() == [1, 1]
    side.finish(np.zeros(batch.queries.count, np.uint8))
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
        verdicts = np.arange(batch.queries.count, dtype=np.uint8) % 3
        finished = side.finish(verdicts)
        np.testing.assert_array_equal(side.last_verdicts, verdicts)
    np.testing.assert_array_equal(finished.vector[0], replay.vectors[side.decision[0] % cpu_replay.POOL])
    assert not finished.teacher_valid.any()


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
        assert a.queries.count == b.queries.count
        np.testing.assert_array_equal(a.queries.raw, b.queries.raw)
        verdicts = np.zeros(a.queries.count, np.uint8)
        np.testing.assert_array_equal(one.finish(verdicts).raw, many.finish(verdicts).raw)
    many.close()


def test_replay_requests_and_phase_order(tmp_path):
    replay = cpu_replay.Replay([drive_like(tmp_path / "a.bin")])
    empty = cpu_replay.ReplaySide(replay, [2], shift=False)
    assert empty.reset().queries.count == 0
    assert isinstance(empty.finish(np.zeros(0, np.uint8)), contract.FinishBatch)
    side = cpu_replay.ReplaySide(replay, range(50), shift=False)
    for step in range(3):
        batch = side.reset() if step == 0 else side.step(None)
        counts = np.bincount(batch.queries.agents[:batch.queries.count], minlength=50)
        assert (counts == 34).sum() == 1  # 2%, all rebuild candidates in one round
        assert np.all((counts == 0) | (counts == 1) | (counts == 34))
        with pytest.raises(RuntimeError, match="finish"):
            side.step(None)
        with pytest.raises(ValueError, match="per current query"):
            side.finish(np.zeros(batch.queries.count + 1, np.uint8))
        side.finish(np.full(batch.queries.count, 2, np.uint8))
        assert (side.last_verdicts == 2).all()


def test_loop_resumes_finish_without_repeating_memory():
    class Event:
        def query(self):
            return True

        def synchronize(self):
            pass

    class Side:
        def __init__(self):
            self.agents = 1
            self.decision = -1
            self.replies = []
            self.queries = contract.PassageQueries(1)
            self.queries.count = 1

        def reset(self):
            self.decision = -1
            return self.step(None)

        def step(self, actions):
            self.decision += 1
            self.round = 0
            b = contract.StepBatch(1)
            b.queries = self.queries
            return b

        def finish(self, verdicts):
            self.replies.append((self.decision, self.round, verdicts.copy()))
            self.round += 1
            return self.queries if self.round == 1 else contract.FinishBatch(1)

    class Half:
        def __init__(self):
            self.side, self.n = Side(), 1
            self.done = self.computed = Event()
            self.actions_np = np.zeros((1, 4), np.uint8)
            self.verdicts_np = np.zeros(1, np.uint8)
            self.updates = 0

        def launch(self, batch, after=None):
            self.updates += 1
            self.launch_queries(batch.queries)

        def launch_queries(self, queries, after=None):
            self.query_count = queries.count
            self.verdicts_np[:] = 1 + self.side.decision % 2

        def launch_policy(self, batch, after=None):
            assert self.side.round == 2

    halves = [Half(), Half()]
    stand = loop.Loop(halves)
    seen = []
    stand.start()
    stand.advance(3, lambda h: seen.append(h.side.decision))
    stand.drain(lambda h: seen.append(h.side.decision))
    stand.close()
    assert sorted(seen) == [0, 0, 1, 1, 2, 2, 3, 3]
    for h in halves:
        assert h.updates == 4  # two query rounds, one memory update per decision
        assert len(h.side.replies) == 8
        for decision, round_index, verdicts in h.side.replies:
            assert round_index in (0, 1)
            assert verdicts.tolist() == [1 + decision % 2]


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
