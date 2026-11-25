import json
from pathlib import Path
import re

import numpy as np
import pytest

from society_sim import VecSociety


@pytest.fixture
def fixture_paths(tmp_path):
    # Share the core test's small waynet and its competing chair and toilet options.
    root = Path(__file__).resolve().parents[2]
    source = (root / "OpenGothic/tests/society_core_contract.cpp").read_text()

    def fixture(name):
        return json.loads(re.search(rf'\b{name} = R"\((.*?)\)";', source, re.S).group(1))

    world, config = tmp_path / "world.json", tmp_path / "config.json"
    world.write_text(json.dumps(fixture("BackWorld")))
    settings = fixture("BackConfig")
    settings["decisions"]["teacher"].update(temperature=1, top=3)
    config.write_text(json.dumps(settings))
    return world, config


def assert_batch_equal(actual, expected):
    for name in ("world", "npc", "mask", "own", "options"):
        np.testing.assert_array_equal(actual[name], expected[name])


def test_parallel_matches_independent_worlds(fixture_paths):
    seeds = [1, 13]
    batch_sim = VecSociety(*fixture_paths, seeds, threads=2, record_events=True)
    serial = [VecSociety(*fixture_paths, [seed], record_events=True) for seed in seeds]
    horizon = batch_sim.start_time + 8 * 3600
    rows_seen = 0

    while True:
        batch = batch_sim.advance(horizon)
        individual = [sim.advance(horizon) for sim in serial]
        rows = len(batch["npc"])
        assert batch_sim.times == [sim.times[0] for sim in serial]
        assert batch["own"].shape == (rows, len(batch_sim.own_feature_names))
        assert batch["options"].shape == (
            rows, batch_sim.stride, len(batch_sim.option_feature_names)
        )
        np.testing.assert_array_equal(
            batch["world"], np.repeat(np.arange(len(serial)), [len(b["npc"]) for b in individual])
        )
        for i, other in enumerate(individual):
            for name in ("npc", "mask", "own", "options"):
                np.testing.assert_array_equal(batch[name][batch["world"] == i], other[name])

        if rows:
            selected = batch_sim.act_teacher()
            for i, sim in enumerate(serial):
                if len(individual[i]["npc"]):
                    np.testing.assert_array_equal(selected[batch["world"] == i], sim.act_teacher())
        assert batch_sim.events() == [sim.events()[0] for sim in serial]
        rows_seen += rows
        if not rows:
            break

    assert rows_seen > len(seeds)
    assert batch_sim.times == [horizon] * len(seeds)


def test_actions_and_pending_snapshot(fixture_paths):
    vec = VecSociety(*fixture_paths, [1, 13], threads=2, record_events=True)
    horizon = vec.start_time + 8 * 3600
    batch = vec.advance(horizon)
    saved = vec.save()
    scores = vec.teacher_scores()
    baseline = {name: data.copy() for name, data in batch.items()}
    assert all(data.flags.c_contiguous and not data.flags.writeable for data in batch.values())

    valid = np.argmax(batch["mask"], axis=1)
    invalid = valid.copy()
    invalid[-1] = np.flatnonzero(batch["mask"][-1] == 0)[0]
    with pytest.raises(ValueError, match="row's mask"):
        vec.act(invalid)
    with pytest.raises(ValueError, match="every pending row"):
        vec.act(valid[:-1])
    with pytest.raises(TypeError, match="one-dimensional integer"):
        vec.act(valid.astype(np.float32))
    with pytest.raises(RuntimeError, match="answer the pending batch"):
        vec.advance(horizon)

    selected = vec.act_teacher()
    after = vec.advance(horizon)
    journal = vec.events()

    vec.reset([101, 102])
    assert_batch_equal(vec.restore(saved), baseline)
    np.testing.assert_array_equal(vec.teacher_scores(), scores)
    np.testing.assert_array_equal(vec.act_teacher(), selected)
    assert_batch_equal(vec.advance(horizon), after)
    assert vec.events() == journal
    assert_batch_equal(batch, baseline)

    separate = VecSociety(*fixture_paths, [1, 13])
    with pytest.raises(ValueError, match="another VecSociety"):
        separate.restore(saved)
    with pytest.raises(RuntimeError, match="journaling is disabled"):
        separate.events()
    separate.advance(horizon)
    separate.act(valid)
