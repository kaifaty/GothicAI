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
    from contextlib import nullcontext
    from types import SimpleNamespace

    operations = []

    class Event:
        def query(self):
            return True

        def synchronize(self):
            pass

        def record(self):
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
        launch = loop.Half.launch
        mark = loop.Half.mark

        def __init__(self, index):
            self.index = index
            self.side, self.n = Side(), 1
            self.done, self.computed = Event(), Event()
            self.actions_np = np.zeros((1, 4), np.uint8)
            self.verdicts_np = np.zeros(1, np.uint8)
            self.updates = self.reads = 0
            self.torch = SimpleNamespace(cuda=SimpleNamespace(stream=lambda _: nullcontext()))
            self.stream = SimpleNamespace(wait_event=lambda event: None)
            self.staging = SimpleNamespace(copy_=lambda *args, **kwargs: None)
            self.graph = self.read_graph = None
            self.trace = None

        def measured(self, name, function):
            function()

        def memory_part(self):
            self.updates += 1
            operations.append(("update", self.index, self.side.decision))

        def read_part(self):
            self.reads += 1
            operations.append(("read", self.index, self.side.decision))

        def launch_queries(self, queries, after=None):
            self.query_count = queries.count
            self.verdicts_np[:] = 1 + self.side.decision % 2
            operations.append(("verdict_ready", self.index, self.side.decision))
            self.done.record()

        def launch_policy(self, batch, after=None):
            assert self.side.round == 2
            operations.append(("policy", self.index, self.side.decision))

    halves = [Half(0), Half(1)]
    stand = loop.Loop(halves)
    seen = []
    stand.start()
    stand.advance(3, lambda h: seen.append(h.side.decision))
    stand.drain(lambda h: seen.append(h.side.decision))
    stand.close()
    assert sorted(seen) == [0, 0, 1, 1, 2, 2, 3, 3]
    assert [index for name, index, _ in operations if name == "update"] == [0, 1] * 4
    active = None
    for name, index, _ in operations:
        if name == "update":
            assert active is None
            active = index
        elif name == "policy":
            assert active == index
            active = None  # The other half gets the GPU only after this policy is queued.
    for h in halves:
        assert h.updates == h.reads == 4  # Two query rounds, one update and read per decision.
        assert len(h.side.replies) == 8
        for decision, round_index, verdicts in h.side.replies:
            assert round_index in (0, 1)
            assert verdicts.tolist() == [1 + decision % 2]
        for decision in range(4):
            events = [name for name, index, d in operations if index == h.index and d == decision]
            assert events.index("verdict_ready") < events.index("read") < events.index("policy")


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


def test_remote_submits_own_run_and_preserves_venv(tmp_path, monkeypatch):
    import json
    from types import SimpleNamespace

    memory = tmp_path / "memory.json"
    memory.write_text(json.dumps({"traces": [], "weights": {"v5": {"sha256": "test"}}}))
    monkeypatch.setattr(loop.bench, "MEMORY_REPORT", memory)
    stamp = "submit-" + tmp_path.name
    monkeypatch.setattr(loop.bench, "run_stamp", lambda: stamp)
    monkeypatch.setattr(loop.bench, "ssh", lambda host, script: SimpleNamespace(
        stdout="/home/test" if script.startswith("printf") else ""))
    monkeypatch.setattr(loop.bench, "stage", lambda *args: None)
    commands = []

    def run(command, **kwargs):
        commands.append(command)
        if command[0] == "tar":
            Path(command[2]).touch()
        return SimpleNamespace(stdout="passage-job\n", returncode=0)

    monkeypatch.setattr(loop.subprocess, "run", run)
    args = SimpleNamespace(host="ryzen", agents=[2048, 3072], skip_parity=False,
                           precisions="fp16", torch="2.14.1", warmup=3, repeats=20,
                           fill_workers=8, gpu_mem=8, mem=6, max_seconds=3600, gq_class="quiet")
    assert loop.remote_command(args) == "passage-job"
    command = commands[-1]
    assert command[3] == "submit" and "--wait-max" not in command
    run_path = f"/home/test/gothic-ai/nav-sim-stand-passage/runs/{stamp}"
    assert command[command.index("--archive") + 1] == run_path
    body = command[-1]
    assert f"{run_path}/raw/passage.json" in body
    assert "--destination-relative nav-sim-research/stand/passage/" + stamp in body
    assert "--attempt-id " + stamp in body
    assert "/home/test/gothic-ai/nav-sim-research/" not in body
    assert ".venv" not in body.split("trap finish EXIT")[0]
    assert "scripts/nas_archive.py" in loop.STAGED
