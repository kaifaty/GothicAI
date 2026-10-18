"""Asset-free tests for split isolation, categorical math and RL transition data."""
import gzip
import importlib.util
import json
import math
import threading
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

SPEC = importlib.util.spec_from_file_location("nav_rl", Path(__file__).resolve().parents[1] / "nav_rl.py")
rl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(rl)
demos = rl.module("nav_demos")


def test_historical_sources_exclude_all_attempts_and_both_directions(tmp_path):
    (tmp_path / "old.jsonl").write_text(json.dumps(dict(start="A", goal="B", success=False)) + "\n")
    (tmp_path / "index.json").write_text(json.dumps(dict(episodes=[dict(pair=["C", "D"], split="train")])))
    (tmp_path / "pairs.csv").write_text("region,start,goal\nmarket,E,F\n")
    assert rl.historical_pairs([tmp_path]) == {("A", "B"), ("B", "A"), ("C", "D"), ("D", "C"), ("E", "F"), ("F", "E")}
    with pytest.raises(ValueError, match="Missing exclusion"):
        rl.historical_pairs([tmp_path / "missing"])


def candidates():
    return [dict(region=region, start=f"{region}-S{n}", goal=f"{region}-G{n}", detour_complete=True)
            for region in rl.REGIONS for n in range(8)]


def test_selection_is_seeded_unique_stratified_and_excludes_reverse():
    pool = candidates()
    excluded = {(pool[0]["goal"], pool[0]["start"])}
    selected = rl.select_pairs(pool, excluded, 3, 91)
    assert selected == rl.select_pairs(list(reversed(pool)), excluded, 3, 91)
    assert selected != rl.select_pairs(pool, excluded, 3, 92)
    assert all((p["start"], p["goal"])[::-1] not in excluded for p in selected)
    assert len(selected) == 18
    assert [p["index"] for p in selected] == list(range(18))
    assert rl.Counter(p["region"] for p in selected) == {r: 3 for r in rl.REGIONS}


def test_selection_rejects_insufficient_region_and_duplicate_undirected_pairs():
    pool = candidates()
    with pytest.raises(ValueError, match="Need 9 unused"):
        rl.select_pairs(pool, set(), 9, 1)
    pool.append(dict(region="market", start=pool[0]["goal"], goal=pool[0]["start"]))
    selected = rl.select_pairs(pool, set(), 8, 1)
    assert len({frozenset((p["start"], p["goal"])) for p in selected}) == 48


def test_frozen_selection_cannot_be_replaced(tmp_path):
    path = tmp_path / "pairs.jsonl"
    rl.write_pairs(path, candidates())
    rl.write_pairs(path, candidates())
    assert rl.load_pairs(path) == candidates()
    with pytest.raises(ValueError, match="Refusing to replace"):
        rl.write_pairs(path, candidates()[1:])


def test_categorical_log_probs_use_independent_stable_softmax():
    logits = [1000, 1001, 1002, -1002, -1001, -1000, 4, 5, 6, 30, 31, -100, 100, 0, 0]
    action = [0, 2, 1, 1, 0, 1]
    expected = [1000 - (1002 + math.log(1 + math.exp(-1) + math.exp(-2))),
                -math.log(1 + math.exp(-1) + math.exp(-2)),
                -1 - math.log(1 + math.exp(-1) + math.exp(-2)),
                -math.log(1 + math.exp(-1)), -200, -math.log(2)]
    np.testing.assert_allclose(rl.categorical_log_probs(logits, action), expected, atol=1e-12)
    heads = dict(zip(rl.HEADS, np.split(np.array(logits), np.cumsum(rl.CLASSES)[:-1])))
    np.testing.assert_array_equal(rl.categorical_log_probs(heads, action), rl.categorical_log_probs(logits, action))


@pytest.mark.parametrize("action,logits", [([3, 0, 0, 0, 0, 0], [0] * 15),
                                           ([0, 0, 0, 0, 0, .5], [0] * 15),
                                           ([0] * 6, [0] * 14),
                                           ([0] * 6, [float("nan")] * 15)])
def test_categorical_log_probs_reject_corruption(action, logits):
    with pytest.raises(ValueError):
        rl.categorical_log_probs(logits, action)


def episode(path, success=True):
    path.mkdir()
    records = np.zeros(2, dtype=demos.RECORD_V2_DTYPE)
    records["time_ms"] = [0, 100]
    with gzip.open(path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, 2, demos.RECORD_BYTES, demos.RAY_COUNT,
                                      demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    (path / "episode.json").write_text(json.dumps(dict(decisions=2, success=success, elapsed_ms=200)))
    log_probs = [-math.log(n) for n in rl.CLASSES]
    samples = [dict(time_ms=n * 100, next_time_ms=(n + 1) * 100, logits=[0] * 15, action_indices=[1] * 6,
                    log_probs=log_probs, log_prob=sum(log_probs),
                    path_before_cm=1000 - 500 * n, path_after_cm=500 - 500 * n,
                    rewards=dict(progress_cm=500, arrived=int(n == 1 and success), blocked=0,
                                 fall_damage=0, death=0, jump=0, climb=0, time_ms=100),
                    terminated=n == 1 and success, truncated=n == 1 and not success) for n in range(2)]
    (path / "samples.jsonl").write_text("".join(json.dumps(sample) + "\n" for sample in samples))
    actions = np.zeros((3, 9))
    actions[:, 0] = [0, 100, 200]
    actions[:2, 4:7] = 1
    actions[-1, 4] = 1
    np.savetxt(path / "actions.txt", actions)
    return samples


def save_samples(path, samples):
    (path / "samples.jsonl").write_text("".join(json.dumps(sample) + "\n" for sample in samples))


def test_sidecar_roundtrip_validates_cpp_math_and_telescoping_progress(tmp_path):
    path = tmp_path / "episode"
    samples = episode(path)
    assert list(rl.iter_samples(path)) == samples
    result = rl.check_episode(path)
    assert result["decisions"] == 2
    assert result["max_log_prob_error"] < 1e-15
    assert result["reward_sums"]["progress_cm"] == 1000
    assert result["successful_progress_relative_error"] == 0


@pytest.mark.parametrize("corruption,error", [
    (lambda samples: samples[1].update(log_prob=0), "Log-prob error"),
    (lambda samples: samples[1].update(time_ms=0), "Nonincreasing"),
    (lambda samples: samples[1]["rewards"].update(progress_cm=3), "Detour path difference"),
    (lambda samples: samples[1].update(path_before_cm=503), "Detour path difference"),
    (lambda samples: samples[1]["rewards"].update(death=-1), "unsigned reward"),
])
def test_sidecar_rejects_corrupt_transition(tmp_path, corruption, error):
    path = tmp_path / "episode"
    samples = episode(path)
    corruption(samples)
    save_samples(path, samples)
    with pytest.raises(ValueError, match=error):
        rl.check_episode(path)


def test_failed_episode_keeps_nonzero_residual_and_negative_progress(tmp_path):
    path = tmp_path / "failed"
    samples = episode(path, success=False)
    samples[1].update(path_after_cm=600)
    samples[1]["rewards"].update(progress_cm=-100)
    save_samples(path, samples)
    result = rl.check_episode(path)
    assert result["reward_sums"]["progress_cm"] == 400
    assert result["successful_progress_relative_error"] is None


def test_population_throughput_uses_concurrent_span_not_sum(tmp_path, monkeypatch):
    monkeypatch.setattr(rl, "checked_paths", lambda *a, **k: None)
    monkeypatch.setattr(rl, "load_pairs", lambda p: candidates())
    monkeypatch.setattr(rl, "new_runner", lambda *a: SimpleNamespace(close=lambda: None))
    def worker(args, index, barrier):
        barrier.wait()
        return dict(decisions=100 + index, collector_wall_s=10 + index)
    monkeypatch.setattr(rl, "population_worker", worker)
    args = SimpleNamespace(parallel=4, pairs=None, output=tmp_path, population=20,
                           record=False, action_mode="sample")
    rl.population(args)
    result = rl.read_json(tmp_path / "throughput-n20-r4.json")
    assert result["collection_wall_s"] == 13
    assert result["decisions_per_second"] == pytest.approx(406 / 13)


def test_drive_requires_previously_frozen_teacher_set():
    with pytest.raises(ValueError, match="frozen"):
        # Exercise the guard before any runner or weights can be loaded.
        original = rl.load_pairs
        try:
            rl.load_pairs = lambda _: candidates()
            rl.drive(SimpleNamespace(pairs=Path("unused")))
        finally:
            rl.load_pairs = original


def test_sidecar_selected_action_must_equal_applied_keys(tmp_path):
    path = tmp_path / "episode"
    episode(path)
    actions = np.loadtxt(path / "actions.txt")
    actions[0, 1] = 1
    np.savetxt(path / "actions.txt", actions)
    with pytest.raises(ValueError, match="Selected action differs"):
        rl.check_episode(path)


def test_sidecar_clock_must_equal_navdemo_input_clock(tmp_path):
    path = tmp_path / "episode"
    episode(path)
    with gzip.open(path / "samples.bin.gz", "rb") as stream:
        header = stream.read(demos.HEADER.size)
        records = np.frombuffer(stream.read(), dtype=demos.RECORD_V2_DTYPE).copy()
    records["time_ms"][1] += 1
    with gzip.open(path / "samples.bin.gz", "wb") as stream:
        stream.write(header)
        stream.write(records.tobytes())
    with pytest.raises(ValueError, match="NAVDEMO/sample timestamps"):
        rl.check_episode(path)


def test_equivalence_keeps_drive_and_population_under_explicit_hero_control(tmp_path):
    args = SimpleNamespace(roads=Path('/roads.json'), npc='NPC', limit_ms=240000,
                           weights=Path('/policy.navbc'), seed=7,
                           far_waypoint='NW_FARM1_ENTRANCE_01')
    pair = dict(start='START', goal='GOAL')
    near = rl.equivalence_commands(args, pair, tmp_path / 'pair.txt', 'drive-near')
    assert near == ['roads load /roads.json', 'roads on NPC', 'navdemos hero near',
                    'navdemos drive NPC START GOAL 240000 /policy.navbc 0 7', 'navdemos wait']
    for mode in ('drive-far', 'pop-far'):
        far = rl.equivalence_commands(args, pair, tmp_path / 'pair.txt', mode)
        assert far[2:4] == ['navdemos hero keep', 'goto NW_FARM1_ENTRANCE_01']
    assert rl.equivalence_commands(args, pair, tmp_path / 'pair.txt', 'pop-near')[-2].endswith('1 7 argmax 1 1 near 0')


def test_comparison_requires_route_identity_success_and_five_percent_time():
    reference = {('A', 'B'): dict(success=True, elapsed_ms=100000)}
    assert rl.comparison_metrics(reference, {('A', 'B'): dict(success=True, elapsed_ms=105000)})['passed']
    assert not rl.comparison_metrics(reference, {('A', 'B'): dict(success=True, elapsed_ms=105001)})['passed']
    assert not rl.comparison_metrics(reference, {('A', 'B'): dict(success=False, elapsed_ms=100000)})['passed']
    with pytest.raises(ValueError, match='identical route sets'):
        rl.comparison_metrics(reference, {('A', 'C'): dict(success=True, elapsed_ms=100000)})


@pytest.mark.parametrize("parallel", [1, 4])
def test_equivalence_selects_first_pairs_and_resets_each_comparison(tmp_path, monkeypatch, parallel):
    pairs = candidates()
    monkeypatch.setattr(rl, 'checked_paths', lambda *a, **k: None)
    monkeypatch.setattr(rl, 'load_pairs', lambda _: pairs)
    closed = []
    monkeypatch.setattr(rl, 'new_runner', lambda *a: SimpleNamespace(close=lambda: closed.append(True)))
    calls = []
    def scenario(args, runner, name, commands, directory):
        prefix, index = name.rsplit('-', 1)
        mode = prefix.removeprefix('equivalence-')
        index = int(index)
        calls.append((index, mode, args.seed, commands))
        episode = dict(start=pairs[index]['start'], goal=pairs[index]['goal'], success=True,
                       elapsed_ms=1000, decisions=10, demo_version=2, drive=True, physics_mode='cv',
                       min_hero_distance_cm=12000 if mode.endswith('-far') else 500,
                       max_hero_distance_cm=15000 if mode.endswith('-far') else 1500)
        destination = directory / 'episode.json' if mode.startswith('drive-') else directory / 'episodes/0/episode.json'
        rl.write_json(destination, episode)
        np.savetxt(destination.parent / 'trajectory.txt', [[0, 0, 0, 0, 0], [1000, 100, 0, 0, 0]])
        np.savetxt(destination.parent / 'actions.txt', np.zeros((2, 9)))
    monkeypatch.setattr(rl, 'scenario', scenario)
    args = SimpleNamespace(pairs=None, count=2, output=tmp_path, seed=7, npc='NPC',
                           roads=Path('/roads.json'), weights=Path('/policy.navbc'),
                           limit_ms=240000, far_waypoint='NW_FARM1_ENTRANCE_01', min_far_cm=10000,
                           resume=False, parallel=parallel)
    rl.equivalence(args)
    if parallel == 1:
        assert [(index, mode) for index, mode, _, _ in calls] == [
            (index, mode) for index in range(2) for mode in ('drive-near', 'pop-near', 'drive-far', 'pop-far')]
    else:
        for mode in ('drive-near', 'pop-near', 'drive-far', 'pop-far'):
            assert [index for index, called_mode, _, _ in calls if called_mode == mode] == [0, 1]
    assert all(seed == 7 for _, _, seed, _ in calls)
    assert len(closed) == (1 if parallel == 1 else 4)
    result = rl.read_json(tmp_path / 'equivalence.json')
    assert result['passed']
    assert result['n'] == 2
    assert result['comparisons']['drive_vs_population_near']['reference_arrived'] == 2
    assert all(pair['far_enough'] for pair in result['far_distances'])
    assert list(rl.rows(tmp_path / 'selected-pairs.jsonl')) == pairs[:2]


def test_raw_candidates_deduplicate_names_but_final_lists_remain_strict(tmp_path):
    path = tmp_path / 'candidates.jsonl'
    first = dict(region='lobart', start='NW_FARM1_INHOUSE_03_SIT', goal='FP_CAMPFIRE_NW_FARM1_MILL_01', distance_cm=1200)
    repeated = dict(first, distance_cm=1201)
    other = dict(region='market', start='A', goal='B')
    path.write_text(''.join(json.dumps(pair) + '\n' for pair in [first, repeated, other]))
    assert rl.load_pairs(path, deduplicate=True) == [first, other]
    with pytest.raises(ValueError, match='Duplicate or identical'):
        rl.load_pairs(path)
    path.write_text(json.dumps(dict(region='market', start='A', goal='A')) + '\n')
    with pytest.raises(ValueError, match='Duplicate or identical'):
        rl.load_pairs(path, deduplicate=True)


def test_generate_deduplicates_before_reachability_preserving_index_alignment(tmp_path, monkeypatch):
    raw = [dict(region=region, start=f'{region}-S', goal=f'{region}-G') for region in rl.REGIONS]
    raw.insert(1, dict(raw[0]))
    monkeypatch.setattr(rl, 'checked_paths', lambda *a, **k: None)
    monkeypatch.setattr(rl, 'historical_pairs', lambda _: {('OLD_START', 'OLD_GOAL')})
    monkeypatch.setattr(rl, 'new_runner', lambda *a: SimpleNamespace(close=lambda: None))
    def scenario(args, runner, name, commands, directory):
        directory.mkdir(parents=True)
        if name == 'generate':
            (directory / 'pairs.jsonl').write_text(''.join(json.dumps(pair) + '\n' for pair in raw))
        else:
            runner_list = [line.split() for line in (args.output / 'candidate-pairs.txt').read_text().splitlines()]
            assert len(runner_list) == 6
            assert len({tuple(pair) for pair in runner_list}) == 6
            reached = [dict(pair_index=index, start=start, goal=goal, complete=True, length_cm=1500)
                       for index, (start, goal) in enumerate(runner_list)]
            (directory / 'reachable.jsonl').write_text(''.join(json.dumps(pair) + '\n' for pair in reached))
    monkeypatch.setattr(rl, 'scenario', scenario)
    args = SimpleNamespace(output=tmp_path, exclude=[], seed=7, candidates_per_region=2, per_region=1)
    rl.generate(args)
    selected = rl.load_pairs(tmp_path / 'pairs.jsonl')
    assert len(selected) == 6
    assert {pair['region'] for pair in selected} == set(rl.REGIONS)
    assert all(pair['detour_complete'] and pair['detour_length_cm'] == 1500 for pair in selected)


def test_parallel_qualification_selects_in_source_order_despite_reverse_completion(tmp_path, monkeypatch):
    pool = [dict(region=region, start=f'{region}-S{i}', goal=f'{region}-G{i}')
            for region in rl.REGIONS for i in range(2)]
    monkeypatch.setattr(rl, 'checked_paths', lambda *a, **k: None)
    monkeypatch.setattr(rl, 'load_pairs', lambda _: pool)
    ready = threading.Barrier(6)
    finished = {region: threading.Event() for region in rl.REGIONS}
    completion = []
    owners = []
    def new_runner(args, name):
        region = name.removeprefix('teacher-')
        owners.append(name)
        ready.wait(timeout=5)
        def close():
            completion.append(region)
            finished[region].set()
        return SimpleNamespace(region=region, close=close)
    monkeypatch.setattr(rl, 'new_runner', new_runner)
    def scenario(args, runner, name, commands, directory):
        index = int(name.rsplit('-', 1)[1])
        pair = pool[index]
        assert runner.region == pair['region']
        assert args.seed == 7
        rank = rl.REGIONS.index(pair['region'])
        if rank < 5:
            assert finished[rl.REGIONS[rank + 1]].wait(timeout=5)
        directory.mkdir(parents=True)
        episode = dict(start=pair['start'], goal=pair['goal'], success=index % 2 == 1,
                       elapsed_ms=1000, demo_version=2, teacher_version=2, physics_mode='cv')
        rl.write_json(directory / 'episode.json', episode)
        for filename in ('samples.bin.gz', 'trajectory.txt', 'actions.txt'):
            (directory / filename).touch()
    monkeypatch.setattr(rl, 'scenario', scenario)
    args = SimpleNamespace(pairs=None, output=tmp_path, seed=7, parallel=8, per_region=1,
                           npc='NPC', roads=Path('/roads.json'), limit_ms=240000, resume=False)
    rl.qualify(args)
    result = list(rl.rows(tmp_path / 'holdout.jsonl'))
    assert [pair['start'] for pair in result] == [f'{region}-S1' for region in rl.REGIONS]
    assert [pair['index'] for pair in result] == list(range(6))
    assert all(pair['teacher_success'] and pair['final_test'] for pair in result)
    assert completion == list(reversed(rl.REGIONS))
    assert set(owners) == {f'teacher-{region}' for region in rl.REGIONS}
    attempts = rl.read_json(tmp_path / 'teacher-attempts.json')
    assert [attempt['candidate'] for attempt in attempts] == list(range(12))


def test_parallel_drive_partitions_original_indices_and_owns_mutable_subtrees(tmp_path, monkeypatch):
    pool = [dict(pair, index=index, final_test=True, teacher_success=True, teacher_version=2)
            for index, pair in enumerate(candidates())]
    monkeypatch.setattr(rl, 'load_pairs', lambda _: pool)
    workers = []
    ready = threading.Barrier(4)
    def collect(args):
        workers.append(args)
        ready.wait(timeout=5)
    monkeypatch.setattr(rl, 'module', lambda _: SimpleNamespace(collect=collect))
    args = SimpleNamespace(pairs=Path('/frozen.jsonl'), output=tmp_path, seed=7, parallel=4,
                           indices='0,2,3,5,7,8', limit_ms=240000, weights=Path('/policy.navbc'))
    rl.drive(args)
    ownership = {worker.output.name: worker.indices for worker in workers}
    assert ownership == {'w0': '0,7', 'w1': '2,8', 'w2': '3', 'w3': '5'}
    assert len({worker.output for worker in workers}) == 4
    assert all(worker.output.parent == tmp_path / 'drive-workers' for worker in workers)
    assert all(worker.teacher_beta == 0 and not worker.dagger_collection and worker.limit_ms == 240000 for worker in workers)
    assert args.output == tmp_path and args.indices == '0,2,3,5,7,8'


def test_single_drive_keeps_original_output_layout(tmp_path, monkeypatch):
    monkeypatch.setattr(rl, 'load_pairs', lambda _: [dict(final_test=True, teacher_success=True, teacher_version=2)])
    calls = []
    monkeypatch.setattr(rl, 'module', lambda _: SimpleNamespace(collect=lambda args: calls.append(args)))
    args = SimpleNamespace(pairs=None, output=tmp_path, seed=7, parallel=1, indices=None)
    rl.drive(args)
    assert calls[0].output == tmp_path
    assert calls[0].indices == '0'


def test_far_comparison_accepts_improved_speed_and_reachability():
    near = {('A', 'B'): dict(success=True, elapsed_ms=100000),
            ('C', 'D'): dict(success=False, elapsed_ms=240000)}
    far = {('A', 'B'): dict(success=True, elapsed_ms=50000),
           ('C', 'D'): dict(success=True, elapsed_ms=150000)}
    result = rl.distance_comparison_metrics(near, far)
    assert result['passed']
    assert result['far_arrived'] == 2 and result['near_arrived'] == 1
    assert not result['symmetric_diagnostic']['passed']
    assert result['lost_success_routes'] == []


@pytest.mark.parametrize('success,elapsed', [(False, 240000), (True, 105001)])
def test_far_comparison_rejects_reachability_or_time_regression(success, elapsed):
    near = {('A', 'B'): dict(success=True, elapsed_ms=100000)}
    far = {('A', 'B'): dict(success=success, elapsed_ms=elapsed)}
    result = rl.distance_comparison_metrics(near, far)
    assert not result['passed']
    assert result['lost_success_routes'] == ([] if success else [dict(start='A', goal='B')])
    assert result['time_tolerance'] == .05


def test_shared_scenario_propagates_cv_and_preserves_historical_native_default(tmp_path):
    collector = rl.module('collect_nav_demos')
    args = SimpleNamespace(output=tmp_path, seed=7)
    native = collector.scenario_file(args, 'native', ['navdemos wait']).read_text()
    assert 'navdemos physics native\n' in native
    args.physics = 'cv'
    cv = collector.scenario_file(args, 'cv', ['navdemos wait']).read_text()
    assert cv.index('navdemos physics cv\n') < cv.index('navdemos wait\n')
    args.physics = 'native'
    assert 'navdemos physics native\n' in collector.scenario_file(args, 'optout', []).read_text()


def test_teacher_resume_rejects_mixed_physics_metadata(tmp_path, monkeypatch):
    pair = dict(region='market', start='A', goal='B')
    directory = tmp_path / 'teacher/00000'
    directory.mkdir(parents=True)
    rl.write_json(directory / 'episode.json', dict(pair, demo_version=2, teacher_version=2,
                                                  success=True, elapsed_ms=1000, physics_mode='native'))
    for filename in ('samples.bin.gz', 'trajectory.txt', 'actions.txt'):
        (directory / filename).touch()
    closed = []
    monkeypatch.setattr(rl, 'new_runner', lambda *a: SimpleNamespace(close=lambda: closed.append(True)))
    args = SimpleNamespace(output=tmp_path, physics='cv', resume=True, per_region=10)
    with pytest.raises(ValueError, match='physics mode differs'):
        rl.qualify_region(args, [pair], 'market', lambda _: None)
    assert closed == [True]


def test_teacher_keeps_every_success_when_fifty_candidates_cannot_fill_quota(tmp_path, monkeypatch):
    pool = [dict(region='market', start=f'A{i}', goal=f'B{i}') for i in range(3)]
    monkeypatch.setattr(rl, 'new_runner', lambda *a: SimpleNamespace(close=lambda: None))
    visited = []
    def scenario(args, runner, name, commands, directory):
        index = int(name.rsplit('-', 1)[1])
        visited.append(index)
        directory.mkdir(parents=True)
        rl.write_json(directory / 'episode.json', dict(pool[index], success=index == 1,
                     elapsed_ms=1000, demo_version=2, teacher_version=2, physics_mode='cv'))
        for filename in ('samples.bin.gz', 'trajectory.txt', 'actions.txt'):
            (directory / filename).touch()
    monkeypatch.setattr(rl, 'scenario', scenario)
    args = SimpleNamespace(output=tmp_path, resume=False, per_region=10, max_teacher_candidates=2,
                           npc='NPC', roads=Path('/roads'), limit_ms=240000, seed=7, physics='cv')
    chosen = rl.qualify_region(args, pool, 'market', lambda _: None)
    assert visited == [0, 1]
    assert [pair['start'] for pair in chosen] == ['A1']


def test_qualification_reports_sparse_regions_before_any_drive(tmp_path, monkeypatch):
    pool = [dict(region=region, start=f'{region}-A', goal=f'{region}-B') for region in rl.REGIONS]
    monkeypatch.setattr(rl, 'checked_paths', lambda *a, **k: None)
    monkeypatch.setattr(rl, 'load_pairs', lambda _: pool)
    def region(args, candidates, name, publish):
        if name == 'market':
            return [dict(pool[0], index=0, teacher_success=True)]
        return []
    monkeypatch.setattr(rl, 'qualify_region', region)
    args = SimpleNamespace(output=tmp_path, pairs=None, parallel=8, per_region=10, physics='cv')
    rl.qualify(args)
    report = rl.read_json(tmp_path / 'qualification.json')
    assert report['n'] == 1
    assert report['regions']['market'] == 1
    assert report['shortages']['market'] == 9
    assert report['shortages']['lobart'] == 10
    assert report['physics'] == 'cv'


def test_trajectory_diagnostic_locates_first_geometric_and_action_divergence(tmp_path):
    first, second = tmp_path / 'first', tmp_path / 'second'
    first.mkdir()
    second.mkdir()
    np.savetxt(first / 'trajectory.txt', [[0, 0, 0, 0, 0], [100, 10, 0, 0, 0], [200, 20, 0, 0, 0]])
    np.savetxt(second / 'trajectory.txt', [[0, 0, 0, 0, 0], [100, 10.5, 0, 0, 0], [200, 50, 0, 0, 0]])
    keys = np.zeros((4, 9))
    keys[:, 0] = [0, 100, 200, 300]
    np.savetxt(first / 'actions.txt', keys)
    keys[1, 1] = 1
    np.savetxt(second / 'actions.txt', keys)
    result = rl.trajectory_divergence(dict(directory=str(first)), dict(directory=str(second)))
    assert result['first_over_0.1_cm']['time_ms'] == 100
    assert result['first_over_1_cm']['time_ms'] == 200
    assert result['first_over_25_cm']['error_cm'] == 30
    assert result['first_action_difference']['time_ms'] == 100


def test_failed_timeouts_are_not_successful_timing_evidence(monkeypatch):
    monkeypatch.setattr(rl, 'trajectory_divergence', lambda *a: dict(available=True))
    first = {('A', 'B'): dict(success=False, elapsed_ms=240000)}
    second = {('A', 'B'): dict(success=False, elapsed_ms=240000)}
    result = rl.controlled_comparison(first, second)
    assert result['passed']
    assert result['timing_evidence_routes'] == 0
    assert not result['pairs'][0]['timing_compared']
    second[('A', 'B')]['elapsed_ms'] = 1000
    assert not rl.controlled_comparison(first, second)['passed']


def death_metadata():
    state = dict(hp=100, hp_max=100, down=False, dead=False, ragdoll=False,
                 in_air=False, climbing=False, gru_hidden_size=0, gru_l2=0,
                 memory_traveled_cm=0, guidance_points=0, eye_observed=False, held_keys=[0, 0, 0, 1, 0, 0],
                 position_cm=[100, 200, 300], expected_start_position_cm=[100, 200, 300], body_yaw_deg=45)
    return [dict(kill_injected=True, final_dead=True, final_hp=0, final_ragdoll=True,
                 final_state_started=True, final_gru_l2=.5, kill_at_ms=400),
            dict(initial_state=state, kill_injected=False, reset_after_death=True)]


def test_death_smoke_requires_actual_death_and_clean_state_before_first_step():
    result = rl.death_reset_metrics(death_metadata())
    assert result['passed']
    assert result['reset_position_error_cm'] == 0


@pytest.mark.parametrize('field,value,check', [('hp', 50, 'health_restored'), ('ragdoll', True, 'live_pose'),
                                             ('gru_l2', .001, 'recurrent_state_cleared'),
                                             ('position_cm', [300, 200, 300], 'start_pose_restored')])
def test_death_smoke_rejects_contaminated_reset(field, value, check):
    episodes = death_metadata()
    episodes[1]['initial_state'][field] = value
    result = rl.death_reset_metrics(episodes)
    assert not result['passed']
    assert not result['checks'][check]


@pytest.mark.parametrize('outlier_count', [1, 3])
def test_four_mode_equivalence_completes_all_routes_and_counts_unique_outliers(tmp_path, monkeypatch, outlier_count):
    pool = candidates()[:10]
    monkeypatch.setattr(rl, 'checked_paths', lambda *a, **k: None)
    monkeypatch.setattr(rl, 'load_pairs', lambda _: pool)
    monkeypatch.setattr(rl, 'new_runner', lambda *a: SimpleNamespace(close=lambda: None))
    monkeypatch.setattr(rl, 'trajectory_divergence', lambda *a: dict(available=True))
    calls = []
    def record(args, runner, index, pair, pair_file, mode):
        calls.append((index, mode))
        path = args.output / mode / f'{index:05d}'
        if mode.startswith('pop-'):
            path = path / 'episodes/0'
        rl.write_json(path / 'episode.json', dict(pair, drive=True, decisions=10, success=True,
                      elapsed_ms=1200 if index < outlier_count and mode.endswith('-far') else 1000,
                      min_hero_distance_cm=12000, max_hero_distance_cm=15000))
    monkeypatch.setattr(rl, 'record_equivalence', record)
    args = SimpleNamespace(pairs=None, output=tmp_path, count=10, parallel=1, seed=7,
                           far_waypoint='FAR', min_far_cm=10000, physics='cv')
    if outlier_count > 2:
        with pytest.raises(ValueError, match='more than two unique routes'):
            rl.equivalence(args)
    else:
        rl.equivalence(args)
    report = rl.read_json(tmp_path / 'equivalence.json')
    assert len(calls) == 40
    assert len(report['outlier_routes']) == outlier_count
    assert report['passed'] == (outlier_count <= 2)
    assert not report['strict_passed']
    assert all(len(route['comparisons']) == 2 for route in report['outlier_routes'])


def test_far_improvement_with_exact_drive_population_parity_uses_no_outlier_budget(tmp_path, monkeypatch):
    chosen = [dict(region='market', start='A', goal='B'), dict(region='market', start='C', goal='D')]
    for mode in ('drive-near', 'pop-near', 'drive-far', 'pop-far'):
        for index, pair in enumerate(chosen):
            far = mode.endswith('-far')
            directory = tmp_path / mode / f'{index:05d}'
            if mode.startswith('pop-'):
                directory = directory / 'episodes/0'
            rl.write_json(directory / 'episode.json', dict(pair, drive=True, decisions=10,
                          success=True if far else index == 0,
                          elapsed_ms=(130667 if index == 0 else 187133) if far else (143233 if index == 0 else 240000),
                          min_hero_distance_cm=12000, max_hero_distance_cm=15000))
    monkeypatch.setattr(rl, 'trajectory_divergence', lambda *a: dict(available=True))
    def forbid_runner(*args):
        raise AssertionError('Summary must not restart a runner')
    monkeypatch.setattr(rl, 'new_runner', forbid_runner)
    args = SimpleNamespace(output=tmp_path, far_waypoint='FAR', min_far_cm=10000, physics='cv')
    report = rl.summarize_equivalence(args, chosen)
    assert report['passed']
    assert not report['strict_passed']
    assert report['outlier_routes'] == []
    assert len(report['raw_symmetric_outlier_routes']) == 2
    assert all(metric['passed'] for metric in report['distance_noninferiority'].values())
    assert report['comparisons']['drive_vs_population_near']['passed']
    assert report['comparisons']['drive_vs_population_far']['passed']
