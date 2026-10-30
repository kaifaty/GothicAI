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


@pytest.fixture
def validation_disk_space(monkeypatch):
    usage = SimpleNamespace(free=32 * 1024**3)
    monkeypatch.setattr(rl.shutil, "disk_usage", lambda path: usage)
    return usage


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


def test_parallel_population_recording_keeps_worker_outputs_and_pair_provenance(tmp_path, monkeypatch):
    pairs = [dict(pair, index=index, split="train", split_index=index)
             for index, pair in enumerate(candidates())]
    monkeypatch.setattr(rl, "checked_paths", lambda *a, **k: None)
    monkeypatch.setattr(rl, "load_pairs", lambda p: pairs)
    monkeypatch.setattr(rl, "new_runner", lambda *a: SimpleNamespace(close=lambda: None))
    def scenario(args, runner, name, commands, directory):
        assert commands[-2].split()[-3] == "1"  # RECORD flag, followed by hero/run-ms.
        rl.write_json(directory / "population.json", dict(decisions=7))
        rl.write_json(directory / "episodes/0/episode.json", dict(pair_index=0))
    monkeypatch.setattr(rl, "scenario", scenario)
    args = SimpleNamespace(parallel=4, pairs=None, output=tmp_path, population=20,
                           record=True, action_mode="sample", seed=3, npc="NPC",
                           roads=Path("/roads.json"), weights=Path("/policy.navbc"),
                           limit_ms=40000, episodes=20, hero="far", run_ms=40000,
                           watchdog_seconds=30)
    rl.population(args)
    result = rl.read_json(tmp_path / "throughput-n20-r4.json")
    directories = [Path(worker["directory"]) for worker in result["workers"]]
    assert len(set(directories)) == 4
    assert result["decisions"] == 28
    for index, directory in enumerate(directories):
        pair_index = (index * args.population) % len(pairs)
        selected = rl.read_json(directory / "episodes/0/episode.json")
        assert selected["region"] == pairs[pair_index]["region"]
        assert selected["index"] == selected["split_index"] == pair_index
        pair_file = tmp_path / "runner-pairs" / f"n20-r4-w{index}.txt"
        assert pair_file.read_text().splitlines()[0] == f"{pairs[pair_index]['start']} {pairs[pair_index]['goal']}"


@pytest.mark.parametrize("record", [False, True])
@pytest.mark.parametrize("initial_failure", [False, True])
def test_population_keeps_clean_workers_and_counts_rejected_episode_in_denominator(
        tmp_path, monkeypatch, record, initial_failure):
    pairs = [dict(pair, index=index) for index, pair in enumerate(candidates())]
    monkeypatch.setattr(rl, "checked_paths", lambda *a, **k: None)
    monkeypatch.setattr(rl, "load_pairs", lambda p: pairs)
    monkeypatch.setattr(rl, "new_runner", lambda *a: SimpleNamespace(close=lambda: None))
    diagnostic = dict(stage="observation", time_ms=1234, elapsed_ms=100,
                      diagnostic="NAVBC nonfinite body input",
                      fields=[dict(name="body.flags[2]", value="nan"), dict(name="ground_normal.y", value="nan")])
    def scenario(args, runner, name, commands, directory):
        worker = int(name.rsplit("w", 1)[1])
        if worker == 0:
            rejected = dict(pair_index=0, decisions=0 if initial_failure else 3, success=False,
                            failure="nonfinite_observation", termination="nonfinite_observation",
                            training_valid=False, nonfinite_observation=diagnostic)
            episodes = [rejected] if initial_failure else [dict(pair_index=1, decisions=4, success=True), rejected]
        else:
            episodes = [dict(pair_index=0, decisions=7, success=True, training_valid=True)]
        rl.write_json(directory / "population.json", dict(decisions=sum(ep["decisions"] for ep in episodes),
                      episodes=len(episodes), invalid_observation_episodes=int(worker == 0),
                      training_valid_episodes=len(episodes) - int(worker == 0)))
        for index, episode_data in enumerate(episodes):
            destination = directory / "episodes" / str(index)
            rl.write_json(destination / "episode.json", episode_data)
            # Collection accounting uses metadata only. Corrupt rejected records
            # must not abort the worker or remove its clean neighbours.
            (destination / "samples.bin.gz").write_bytes(b"not-a-gzip-stream")
    monkeypatch.setattr(rl, "scenario", scenario)
    args = SimpleNamespace(parallel=4, pairs=None, output=tmp_path, population=20,
                           record=record, action_mode="sample", seed=3, npc="NPC",
                           roads=Path("/roads.json"), weights=Path("/policy.navbc"),
                           limit_ms=40000, episodes=20, hero="far", run_ms=40000,
                           watchdog_seconds=30)
    rl.population(args)
    result = rl.read_json(tmp_path / "throughput-n20-r4.json")
    assert len(result["workers"]) == 4
    assert result["episodes"] == (4 if initial_failure else 5)
    assert result["decisions"] == (21 if initial_failure else 28)
    assert result["training_valid_episodes"] == (3 if initial_failure else 4)
    assert result["rejected_nonfinite_episodes"] == 1
    assert result["rejected_nonfinite_decisions"] == (0 if initial_failure else 3)
    assert result["rejected_nonfinite_fraction"] == pytest.approx(1 / result["episodes"])
    assert result["episode_terminations"]["nonfinite_observation"] == 1
    assert len(result["rejected_nonfinite_diagnostics"]) == 1
    rejected = result["rejected_nonfinite_diagnostics"][0]
    assert rejected["worker"] == 0
    assert rejected["metadata"]["nonfinite_observation"] == diagnostic
    assert rejected["metadata"]["training_valid"] is False
    assert Path(rejected["episode"]).is_dir()
    for worker in result["workers"]:
        directory = Path(worker["directory"])
        assert (directory / "measurement.json").is_file()
        for path in directory.rglob("samples.bin.gz"):
            assert path.read_bytes() == b"not-a-gzip-stream"


def test_population_cli_allows_parallel_recording(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(rl, "population", calls.append)
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "population", "--output", str(tmp_path),
                                      "--pairs", str(tmp_path / "pairs.jsonl"),
                                      "--parallel", "4", "--record"])
    rl.main()
    assert len(calls) == 1 and calls[0].parallel == 4 and calls[0].record


@pytest.mark.parametrize("parallel", [None, 1, 2, 4, 8, 18, 28])
def test_drive_cli_accepts_positive_parallel_counts(tmp_path, monkeypatch, parallel):
    calls = []
    monkeypatch.setattr(rl, "drive", calls.append)
    argv = ["nav_rl.py", "drive", "--output", str(tmp_path),
            "--pairs", str(tmp_path / "pairs.jsonl")]
    if parallel is not None:
        argv.extend(["--parallel", str(parallel)])
    monkeypatch.setattr(rl.sys, "argv", argv)
    rl.main()
    assert len(calls) == 1
    assert calls[0].parallel == (1 if parallel is None else parallel)


@pytest.mark.parametrize("parallel,error", [("0", "--parallel must be a positive integer"),
                                           ("-1", "--parallel must be a positive integer"),
                                           ("1.5", "invalid int value")])
def test_drive_cli_rejects_invalid_parallel_counts(tmp_path, monkeypatch, capsys, parallel, error):
    calls = []
    monkeypatch.setattr(rl, "drive", calls.append)
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "drive", "--output", str(tmp_path),
                                      "--pairs", str(tmp_path / "pairs.jsonl"),
                                      "--parallel", parallel])
    with pytest.raises(SystemExit) as exc:
        rl.main()
    assert exc.value.code == 2
    assert error in capsys.readouterr().err
    assert not calls


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
                           indices='0,2,3,5,7,8', limit_ms=240000, weights=Path('/policy.navbc'), action_mode='sample')
    rl.drive(args)
    ownership = {worker.output.name: worker.indices for worker in workers}
    assert ownership == {'w0': '0,7', 'w1': '2,8', 'w2': '3', 'w3': '5'}
    assert len({worker.output for worker in workers}) == 4
    assert all(worker.output.parent == tmp_path / 'drive-workers' for worker in workers)
    assert all(worker.teacher_beta == 0 and not worker.dagger_collection and worker.limit_ms == 240000 for worker in workers)
    assert all(worker.action_mode == 'sample' and worker.mixing_seed == 7 for worker in workers)
    assert args.output == tmp_path and args.indices == '0,2,3,5,7,8'


def test_single_drive_keeps_original_output_layout(tmp_path, monkeypatch):
    monkeypatch.setattr(rl, 'load_pairs', lambda _: [dict(final_test=True, teacher_success=True, teacher_version=2)])
    calls = []
    monkeypatch.setattr(rl, 'module', lambda _: SimpleNamespace(collect=lambda args: calls.append(args)))
    args = SimpleNamespace(pairs=None, output=tmp_path, seed=7, parallel=1, indices=None)
    rl.drive(args)
    assert calls[0].output == tmp_path
    assert calls[0].indices == '0'
    assert calls[0].action_mode == 'sample'


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


@pytest.mark.parametrize("command,requested,expected", [("drive", None, "sample"),
    ("drive", "sample", "sample"), ("drive", "argmax", "argmax"),
    ("population", None, "sample"), ("population", "argmax", "argmax")])
def test_action_mode_cli_defaults_to_sampling(tmp_path, monkeypatch, command, requested, expected):
    captured = []
    argv = ['nav_rl.py', command, '--output', str(tmp_path), '--pairs', '/frozen.jsonl']
    if requested is not None:
        argv += ['--action-mode', requested]
    monkeypatch.setattr(rl.sys, 'argv', argv)
    monkeypatch.setattr(rl, command, lambda args: captured.append(args))
    rl.main()
    assert captured[0].action_mode == expected


def validation_manifest(tmp_path):
    scenarios = {family: [dict(id=family + "-case", family=family, goal_kind=(
        "follow" if family == "follow_below_gate" else "point" if family == "jump" else family),
        region="onar", start="A", goal="B", split="validation", seed=19, episode=None,
        recorded=False, decisions=0, limit_ms=240000)] for family in rl.VALIDATION_FAMILIES}
    scenarios["approach"][0].update(target="SHEEP", route=["B", "A"])
    scenarios["follow_below_gate"][0].update(target="BAU_954_MALETH", route=["B", "A", "C"])
    scenarios["jump"][0].update(transition_id=43)
    path = tmp_path / "corpus.json"
    rl.write_json(path, dict(schema_version=1, splits=dict(validation=scenarios,
        train="must not be inspected", test="must not be inspected")))
    return path, scenarios


def test_manifest_validation_uses_exact_frozen_scenarios_and_ignores_other_splits(tmp_path):
    path, groups = validation_manifest(tmp_path)
    values = rl.validation_scenarios(path)
    assert values == [groups[family][0] for family in rl.VALIDATION_FAMILIES]
    assert rl.validation_scenarios(path, "point,object") == values[:2]
    with pytest.raises(ValueError, match="validation family"):
        rl.validation_scenarios(path, "test")


@pytest.mark.parametrize("corruption", [dict(split="test"), dict(recorded=True), dict(episode="/old"),
                                      dict(id="../escape"), dict(family="object"), dict(seed=-1)])
def test_manifest_validation_rejects_changed_identity(tmp_path, corruption):
    path, groups = validation_manifest(tmp_path)
    groups["point"][0].update(corruption)
    rl.write_json(path, dict(schema_version=1, splits=dict(validation=groups)))
    with pytest.raises(ValueError):
        rl.validation_scenarios(path)


def test_validation_commands_preserve_world_seed_and_force_sampled_zero_teacher(tmp_path):
    path, groups = validation_manifest(tmp_path)
    args = SimpleNamespace(npc="NPC", limit_ms=100, weights=Path("/policy.navbc"), roads=Path("/roads"))
    point = rl.validation_commands(args, groups["point"][0], 43)
    assert point[-2] == "navdemos drive NPC A B 240000 /policy.navbc 0 43 sample"
    object_commands = rl.validation_commands(args, groups["object"][0], 43)
    assert object_commands[-2].startswith("navdemos drive-object NPC A B ")
    moving = rl.validation_commands(args, groups["follow_below_gate"][0], 43)
    assert moving[-2] == ("navdemos drive-moving NPC A BAU_954_MALETH B,A,C follow "
                           "240000 19 /policy.navbc 0 43 sample")
    assert "navdemos oracle off" in moving
    assert "navdemos perception-mode gaze" in moving
    assert rl.validation_commands(args, groups["jump"][0], 43)[-2].endswith("sample 43")
    assert "roads on NPC" in point
    args.stranger = True
    assert "roads stranger NPC" in rl.validation_commands(args, groups["point"][0], 43)
    assert rl.action_seeds(None) == list(rl.ACTION_SEEDS)
    assert rl.action_seeds("7,19,43") == [7, 19, 43]
    with pytest.raises(ValueError, match="uint32"):
        rl.action_seeds("7,7")


def test_lean_validation_commands_disable_recording_for_every_family(tmp_path, monkeypatch):
    manifest, groups = validation_manifest(tmp_path)
    args = validation_drive_args(tmp_path, manifest, family=None)
    args.lean = True
    for family in rl.VALIDATION_FAMILIES:
        assert "navdemos record off" in rl.validation_commands(args, groups[family][0], 7)
    captured = []
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "drive", "--output", str(tmp_path),
        "--manifest", str(manifest), "--lean"])
    monkeypatch.setattr(rl, "drive", lambda value: captured.append(value))
    rl.main()
    assert captured[0].lean
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "population", "--output", str(tmp_path),
        "--pairs", "/pairs.jsonl", "--lean"])
    with pytest.raises(SystemExit):
        rl.main()


def test_maneuver_press_metrics_count_edges_and_typed_windows():
    times = np.arange(50) * 100
    teacher = np.zeros((50, 2))
    selected = np.zeros_like(teacher)
    teacher[12:16, 0] = 1
    teacher[30:32, 1] = 1
    selected[5:8, 0] = 1  # Within one second of the teacher jump.
    selected[30:32, 0] = 1  # Wrong maneuver at the climb label.
    selected[45:47, 1] = 1  # Too late to recall climb.
    result = rl.maneuver_press_metrics(times, teacher, selected)
    assert result["jump"] == dict(teacher_presses=1, recalled_presses=1, learner_presses=2,
        false_presses=1, recall=1, false_presses_per_1000_decisions=20)
    assert result["climb"]["recall"] == 0
    assert result["climb"]["false_presses"] == 1


def test_maneuver_press_recall_requires_one_to_one_matches():
    teacher = np.zeros((20, 2))
    selected = np.zeros_like(teacher)
    teacher[[8, 12], 0] = 1
    selected[10, 0] = 1
    result = rl.maneuver_press_metrics(np.arange(20) * 100, teacher, selected)
    assert result["jump"]["teacher_presses"] == 2
    assert result["jump"]["recall"] == .5


def test_jump_passage_uses_executed_buttons_and_frozen_source():
    route = dict(transition_id=43, type="jump", transition_start_m=[0, 0, 0])
    actions = np.zeros((4, 9))
    actions[:, 0] = [0, 100, 200, 300]
    actions[:2, 5] = 1
    frames = [dict(elapsed_ms=time, transition_id=43, position_cm=position)
              for time, position in ((0, [0, 0, 0]), (100, [50, 0, 0]), (200, [200, 0, 0]))]
    trace = rl.applied_transition_trace(route, actions, frames)
    assert [row["jump"] for row in trace] == [True, True, False]
    assert [row["on_transition"] for row in trace] == [True, True, False]
    assert all(row["active_transition_id"] == 43 for row in trace)


def test_validation_summary_pools_follow_by_time_and_keeps_each_action_seed(monkeypatch):
    monkeypatch.setattr(rl, "module", lambda _: SimpleNamespace(group_metrics=lambda episodes: dict(
        n=len(episodes), arrived=sum(ep["success"] for ep in episodes), decisions=sum(ep["decisions"] for ep in episodes),
        action_fraction=dict(turn_only=.1, idle=.2))))
    counts = {name: dict(teacher_presses=1, recalled_presses=1, learner_presses=2, false_presses=1)
              for name in ("jump", "climb")}
    episodes = [dict(id="follow-case", family="follow_below_gate", goal_kind="follow", success=False,
        action_seed=seed, decisions=100, maneuver_presses=counts, follow_observed_ms=observed,
        follow_in_band_ms=band, first_contact_ms=0, follow_full_horizon=True)
        for seed, observed, band in ((7, 1000, 1000), (19, 9000, 0))]
    result = rl.validation_summary(episodes)
    assert result["by_family"]["follow_below_gate"]["follow_fraction"] == .1
    assert set(result["by_seed"]) == {"7", "19"}
    assert result["all"]["maneuver_presses"]["jump"]["false_presses_per_1000_decisions"] == 10


def test_manifest_drive_cli_does_not_require_pairs_and_forbids_argmax(tmp_path, monkeypatch):
    captured = []
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "drive", "--output", str(tmp_path),
                                      "--manifest", "/frozen.json"])
    monkeypatch.setattr(rl, "drive", lambda args: captured.append(args))
    rl.main()
    assert captured[0].manifest == Path("/frozen.json")
    assert captured[0].pairs is None
    assert captured[0].action_mode == "sample"
    with pytest.raises(ValueError, match="sampling"):
        rl.drive_validation(SimpleNamespace(action_mode="argmax"))


def frozen_detour(tmp_path, **changes):
    value = dict(id="detour-A", family="detour", goal_kind="point", split="validation", region="harbour",
        start="A", goal="B", seed=19, geometry=dict(label="detour-A", start="A", goal="B",
        start_cm=[0, 0, 0], goal_cm=[1000, 0, 0], length_cm=1300, complete=True, straight_walkable=False))
    value.update(changes)
    path = tmp_path / "detour-val.jsonl"
    rl.write_pairs(path, [value])
    return path, value


@pytest.mark.parametrize("change", [dict(split="test"), dict(family="object"), dict(goal_kind="follow"),
    dict(visibility=False, goal_kind="object"), dict(start="changed"), dict(id="../unsafe")])
def test_detour_reader_rejects_other_splits_invalid_identity_and_invisible_objects(tmp_path, change):
    path, _ = frozen_detour(tmp_path, **change)
    with pytest.raises(ValueError):
        rl.detour_scenarios(path)


@pytest.mark.parametrize("change", [dict(complete=False), dict(straight_walkable=True), dict(length_cm=1299),
    dict(label="changed"), dict(start_cm=[float("nan"), 0, 0])])
def test_detour_reader_requires_complete_blocked_geometric_route(tmp_path, change):
    path, value = frozen_detour(tmp_path)
    value["geometry"].update(change)
    path.unlink()
    rl.write_pairs(path, [value])
    with pytest.raises(ValueError):
        rl.detour_scenarios(path)


def test_detour_reader_preserves_frozen_geometry_and_rejects_duplicate_ids(tmp_path):
    path, value = frozen_detour(tmp_path, obstacle="rock", source_id="original-v4-point")
    assert rl.detour_scenarios(path) == [value]
    with path.open("a") as stream:
        stream.write(json.dumps(value) + "\n")
    with pytest.raises(ValueError, match="duplicate"):
        rl.detour_scenarios(path)


def test_detour_drive_cli_requires_no_pairs_and_rejects_other_commands(tmp_path, monkeypatch):
    captured = []
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "drive", "--output", str(tmp_path),
        "--detour-scenarios", "/detour-val.jsonl"])
    monkeypatch.setattr(rl, "drive", lambda args: captured.append(args))
    rl.main()
    assert captured[0].manifest is None
    assert captured[0].pairs is None
    assert captured[0].action_mode == "sample"
    monkeypatch.setattr(rl.sys, "argv", ["nav_rl.py", "population", "--output", str(tmp_path),
        "--detour-scenarios", "/detour-val.jsonl"])
    with pytest.raises(SystemExit):
        rl.main()


def test_detour_drive_is_sampled_record_off_and_keeps_policy_outputs_separate(tmp_path, monkeypatch,
                                                                           validation_disk_space):
    path, value = frozen_detour(tmp_path)
    calls = []
    monkeypatch.setattr(rl, "checked_paths", lambda *args, **kwargs: None)
    monkeypatch.setattr(rl, "new_runner", lambda *args: SimpleNamespace(close=lambda: None))
    monkeypatch.setattr(rl, "module", lambda name: pytest.fail("Detour evaluation must not read NAVDEMO or retain a tail"))
    def scenario(args, runner, name, commands, directory):
        assert "navdemos record off" in commands
        seed = int(directory.name.removeprefix("seed-"))
        calls.append((args.weights.stem, args.seed, seed))
        metadata = validation_runner_metadata(value, seed)
        metadata.update(start=value["start"], goal=value["goal"], record=False, teacher_control_decisions=0)
        rl.write_json(directory / "episode.json", metadata)
        (directory / "teacher-paths.jsonl").write_text("unused diagnostic")
    monkeypatch.setattr(rl, "scenario", scenario)
    monkeypatch.setattr(rl, "detour_episode_metrics", lambda directory: dict(success=False, split="validation"))
    for policy in ("bc3", "it35"):
        args = validation_drive_args(tmp_path, None, policy, family=None)
        args.detour_scenarios = path
        rl.drive(args)
        episodes = list(args.output.rglob("episode.json"))
        assert len(episodes) == 3
        assert not list(args.output.rglob("teacher-paths.jsonl"))
        for episode in episodes:
            saved = rl.read_json(episode)
            assert saved["validation_scenario"] == value
            assert saved["weights"] == f"/{policy}.navbc"
    assert calls == [(policy, 19, seed) for policy in ("bc3", "it35") for seed in rl.ACTION_SEEDS]


def test_eval_screen_reader_keeps_frozen_scenarios_and_three_action_seeds(tmp_path):
    manifest, groups = validation_manifest(tmp_path)
    _, detour = frozen_detour(tmp_path)
    safety = dict(id="water-case", family="safety", goal_kind="point", split="validation",
                  region="water", start="A", goal="B", seed=7, limit_ms=240000)
    scenarios = [*rl.validation_scenarios(manifest), detour, safety]
    path = tmp_path / "eval-screen.jsonl"
    entries = [dict(scenario=value, action_seeds=list(rl.ACTION_SEEDS)) for value in scenarios]
    rl.write_pairs(path, entries)
    rl.write_pairs(path, entries)
    assert rl.eval_screen_jobs(path) == [(index, value, seed) for index, value in enumerate(scenarios)
                                         for seed in rl.ACTION_SEEDS]
    assert groups["point"][0] == rl.eval_screen_jobs(path)[0][1]


@pytest.mark.parametrize("corruption", ["test", "seeds", "duplicate", "unsafe"])
def test_eval_screen_reader_rejects_test_or_changed_selection(tmp_path, corruption):
    _, groups = validation_manifest(tmp_path)
    value = groups["point"][0]
    entry = dict(scenario=value, action_seeds=list(rl.ACTION_SEEDS))
    entries = [entry]
    if corruption == "test":
        value["split"] = "test"
    elif corruption == "seeds":
        entry["action_seeds"] = [rl.ACTION_SEEDS[0]]
    elif corruption == "duplicate":
        entries.append(entry)
    else:
        value["id"] = "../unsafe"
    path = tmp_path / "eval-screen.jsonl"
    rl.write_pairs(path, entries)
    with pytest.raises(ValueError):
        rl.eval_screen_jobs(path)


def test_eval_screen_cli_drives_mixed_families_lean_without_selection_overrides(tmp_path, monkeypatch,
                                                                            validation_disk_space):
    _, groups = validation_manifest(tmp_path)
    _, detour = frozen_detour(tmp_path)
    safety = dict(id="water-case", family="safety", goal_kind="point", split="validation",
                  region="water", start="A", goal="B", seed=7, limit_ms=240000)
    scenarios = [groups["point"][0], detour, safety]
    path = tmp_path / "eval-screen.jsonl"
    rl.write_pairs(path, [dict(scenario=value, action_seeds=list(rl.ACTION_SEEDS)) for value in scenarios])
    arguments = ["nav_rl.py", "drive", "--output", str(tmp_path / "screen-run"),
                 "--cache", str(tmp_path / "cache"), "--weights", "/hd.navbc", "--eval-screen", str(path)]
    captured = []
    monkeypatch.setattr(rl, "drive", lambda args: captured.append(args))
    monkeypatch.setattr(rl.sys, "argv", arguments)
    rl.main()
    args = captured[0]
    assert args.lean and args.manifest is None and args.pairs is None
    monkeypatch.setattr(rl.sys, "argv", [*arguments, "--action-seeds", "7"])
    with pytest.raises(SystemExit):
        rl.main()
    stub_validation_runner(monkeypatch)
    calls = []
    def scenario(worker, runner, name, commands, directory):
        family = directory.parent.parent.name
        value = next(value for value in scenarios if value["family"] == family)
        seed = int(directory.name.removeprefix("seed-"))
        assert "navdemos record off" in commands
        if family == "safety":
            assert "navdemos perception-monitor on" in commands
        metadata = validation_runner_metadata(value, seed)
        metadata.update(start=value["start"], goal=value["goal"], record=False, teacher_control_decisions=0)
        rl.write_json(directory / "episode.json", metadata)
        calls.append((family, value["id"], seed))
    monkeypatch.setattr(rl, "scenario", scenario)
    measured = lambda directory: dict(success=False, split="validation", recording="text_only")
    monkeypatch.setattr(rl, "detour_episode_metrics", measured)
    monkeypatch.setattr(rl, "lean_validation_episode_metrics", measured)
    rl.drive_validation(args)
    assert calls == [(value["family"], value["id"], seed) for value in scenarios for seed in rl.ACTION_SEEDS]
    assert len(list(args.output.rglob("drive-metrics.json"))) == 9


def test_detour_text_metrics_count_executed_rising_edges_without_teacher_labels(tmp_path):
    _, value = frozen_detour(tmp_path)
    directory = tmp_path / "episode"
    metadata = validation_runner_metadata(value, 7)
    metadata.update(id=value["id"], family="detour", split="validation", region=value["region"],
        scenario_seed=value["seed"], action_seed=7, start="A", goal="B", decisions=4, elapsed_ms=400,
        success=True, blocked_spans=1, blocked_ms=200, max_blocked_ms=200, teacher_control_decisions=0,
        failure="", distance_cm=0, validation_scenario=value)
    rl.write_json(directory / "episode.json", metadata)
    actions = np.zeros((5, 9))
    actions[:, 0] = np.arange(5) * 100
    actions[:, 4] = 1
    actions[:2, 5] = 1
    actions[3, 5] = 1
    actions[2, 6] = 1
    actions[1, 3] = 1
    np.savetxt(directory / "actions.txt", actions)
    np.savetxt(directory / "trajectory.txt", [[0, 0, 0, 0, 0], [400, 100, 0, 0, 0]])
    measured = rl.detour_episode_metrics(directory)
    assert measured["action_presses"] == dict(jump=dict(presses=2, per_1000_decisions=500),
        climb=dict(presses=1, per_1000_decisions=250))
    assert measured["action_fraction"]["turn_only"] == .25
    assert measured["travelled_xz_cm"] == 100
    assert "disagreement_counts" not in measured
    summary = rl.detour_summary([measured])
    assert summary["all"]["arrived"] == 1
    assert summary["all"]["blocked_ms"] == 200
    assert summary["all"]["teacher_control_decisions"] == 0
    assert summary["by_seed"]["7"]["action_presses"]["jump"]["presses"] == 2
    actions[-1, 5] = 1
    np.savetxt(directory / "actions.txt", actions)
    with pytest.raises(ValueError, match="keys"):
        rl.detour_episode_metrics(directory)


def test_manifest_drive_repeats_scenario_seed_for_three_policy_seeds(tmp_path, monkeypatch):
    manifest, groups = validation_manifest(tmp_path)
    calls, closed, tails = [], [], []
    monkeypatch.setattr(rl, "checked_paths", lambda *args, **kwargs: None)
    monkeypatch.setattr(rl, "new_runner", lambda *args: SimpleNamespace(close=lambda: closed.append(True)))
    def scenario(args, runner, name, commands, directory):
        seed = int(commands[-2].split()[-2])
        calls.append((args.seed, seed, commands[-2]))
        rl.write_json(directory / "episode.json", dict(demo_version=2, navigation_memory_version=3,
            physics_mode="cv", oracle=False, action_mode="sample", teacher_beta=0, mixing_seed=seed,
            drive=True, observation_source="learner_memory", elapsed_ms=100, success=False))
    monkeypatch.setattr(rl, "scenario", scenario)
    monkeypatch.setattr(rl, "validation_episode_metrics", lambda path: dict(success=False))
    monkeypatch.setattr(rl, "module", lambda name: SimpleNamespace(retain_tail=lambda path: tails.append(path)))
    args = SimpleNamespace(manifest=manifest, families="point", action_mode="sample", action_seeds=None,
        indices=None, parallel=1, output=tmp_path / "out", cache=tmp_path / "cache", seed=7, resume=False,
        npc="NPC", roads=Path("/roads"), weights=Path("/policy"), limit_ms=240000, physics="cv")
    rl.drive_validation(args)
    assert [(world, seed) for world, seed, _ in calls] == [(19, seed) for seed in rl.ACTION_SEEDS]
    assert len(closed) == 1
    assert len(tails) == 3


def validation_drive_args(tmp_path, manifest, policy="bc3", family="approach", resume=False):
    output = tmp_path / "validation" / policy
    output.mkdir(parents=True, exist_ok=True)
    return SimpleNamespace(manifest=manifest, families=family, action_mode="sample", action_seeds=None,
        indices=None, parallel=1, output=output, cache=tmp_path / "cache", seed=7,
        resume=resume, npc="NPC", roads=Path("/roads"), weights=Path(f"/{policy}.navbc"),
        limit_ms=240000, physics="cv")


def validation_runner_metadata(value, seed):
    return dict(demo_version=2, navigation_memory_version=3, physics_mode="cv", oracle=False,
        action_mode="sample", teacher_beta=0, mixing_seed=seed, drive=True,
        observation_source="learner_memory", goal_kind=value["goal_kind"], elapsed_ms=100, success=False)


def stub_validation_runner(monkeypatch):
    monkeypatch.setattr(rl, "checked_paths", lambda *args, **kwargs: None)
    monkeypatch.setattr(rl, "new_runner", lambda *args: SimpleNamespace(close=lambda: None))
    monkeypatch.setattr(rl, "module", lambda name: SimpleNamespace(retain_tail=lambda path: None))


def lean_text_episode(directory, family="point", seed=7):
    kind = "follow" if family == "follow_below_gate" else "point" if family in ("jump", "safety", "fence") else family
    value = dict(id=f"{family}-case", family="safety" if family == "fence" else family,
        goal_kind=kind, split="validation", region="onar", start="A", goal="B", seed=19)
    if family == "jump":
        value.update(type="jump", transition_id=43, transition_start_m=[0, 0, 0], transition_end_m=[2, 0, 0])
    if family == "safety":
        value["safety_fixture"] = dict(label="cliff-bridge", kind="cliff", link_id=123,
            hazard_cm=[0, 0, 200], start_to_hazard_cm=200)
    if family == "fence":
        value["fence_control"] = True
    metadata = dict(validation_runner_metadata(value, seed), **{key: value[key] for key in
        ("id", "family", "split", "region", "start", "goal")})
    metadata.update(scenario_seed=19, action_seed=seed, decisions=4, elapsed_ms=400, limit_ms=400,
        success=True, blocked_spans=1, blocked_ms=100, max_blocked_ms=100, teacher_control_decisions=0,
        failure="", distance_cm=0, validation_scenario=value, record=False)
    rl.write_json(directory / "episode.json", metadata)
    actions = np.zeros((5, 9))
    actions[:, 0] = np.arange(5) * 100
    actions[:, 4] = 1
    actions[0, 5] = 1
    actions[1, 3] = 1
    actions[2, 1] = 1
    np.savetxt(directory / "actions.txt", actions)
    np.savetxt(directory / "trajectory.txt", [[i * 100, i * 50, 100, 0, 0] for i in range(5)])
    if kind in ("follow", "approach"):
        np.savetxt(directory / "target-trajectory.txt", [[i * 100, 300 + i * 50, 100, 0, 300, 0] for i in range(5)])
    if family == "jump":
        common = dict(transition_id=43, type="jump", native_jump=False, native_climb=False, in_air=False,
            falling=False, supported=False, landing_path_complete=False, landing_path_m=99., landing_snap_m=99.,
            cv_active=True, source_polygon=11, end_polygon=22, current_polygon=11, source_component=1,
            end_component=2, current_component=1, side_method="component",
            source_position_cm=[0, 0, 0], end_position_cm=[200, 0, 0])
        frames = [dict(common, elapsed_ms=0, position_cm=[0, 100, 0]),
            dict(common, elapsed_ms=16, position_cm=[80, 160, 0], native_jump=True),
            dict(common, elapsed_ms=32, position_cm=[200, 0, 0], supported=True, current_polygon=22,
                current_component=2, landing_path_complete=True, landing_path_m=.25, landing_snap_m=.1)]
        rl.write_pairs(directory / "transition-frames.jsonl", frames)
    if family in ("safety", "fence"):
        common = dict(cv_active=True, supported=True, in_air=False, falling=False, native_jump=False,
            native_climb=False, swimming=False, water_depth_cm=0, deep_water_threshold_cm=140,
            hitpoints=200, hitpoints_max=200)
        frames = [dict(common, elapsed_ms=0, position_cm=[0, 100, 0]),
            dict(common, elapsed_ms=100, position_cm=[0, 80, 0], supported=False, falling=True),
            dict(common, elapsed_ms=200, position_cm=[0, 10, 0], hitpoints=175, water_depth_cm=140)]
        rl.write_pairs(directory / "perception-safety-frames.jsonl", frames)
    return value


@pytest.mark.parametrize("family", ["point", "object", "approach", "follow_below_gate", "jump", "safety", "fence"])
def test_lean_metrics_match_recorded_physical_outcomes_without_navdemo(tmp_path, monkeypatch, family):
    value = lean_text_episode(tmp_path, family)
    original_module = rl.module
    samples = np.zeros(4, dtype=demos.RECORD_V2_DTYPE)
    fake_demos = SimpleNamespace(iter_chunks=lambda directory: iter([samples]))
    monkeypatch.setattr(rl, "module", lambda name: fake_demos if name == "nav_demos" else original_module(name))
    recorded = rl.validation_episode_metrics(tmp_path)
    def lean_module(name):
        assert name not in ("nav_demos", "nav_drive", "nav_ppo_final")
        return original_module(name)
    monkeypatch.setattr(rl, "module", lean_module)
    measured = rl.lean_validation_episode_metrics(tmp_path)
    assert "familiar_input_decisions" not in measured
    episode = rl.read_json(tmp_path / "episode.json")
    counters = dict(familiar_input_decisions=4, familiar_input_nonzero_decisions=3,
        familiar_near_input_nonzero_decisions=2, familiar_far_input_nonzero_decisions=1)
    episode.update(counters)
    rl.write_json(tmp_path / "episode.json", episode)
    measured = rl.lean_validation_episode_metrics(tmp_path)
    assert all(measured[key] == count for key, count in counters.items())
    for key in ("success", "elapsed_ms", "decisions", "travelled_xz_cm", "blocked_ms"):
        assert measured[key] == recorded[key]
    for key in ("turn_only", "idle", "jump", "climb", "walk", "forward"):
        assert measured["action_counts"][key] == recorded["action_counts"][key]
    for key in ("first_contact_ms", "follow_fraction", "follow_success", "approach_success", "transition_success",
                "transition_evidence", "safety", "falls", "entered_deep_water", "swimming"):
        if key in recorded:
            assert measured[key] == recorded[key]
    assert measured["action_presses"]["jump"]["presses"] == 1
    assert measured["recording"] == "text_only"
    assert "maneuver_presses" not in measured and "disagreement_counts" not in measured
    if value["family"] == "jump":
        assert measured["transition_success"]
    if value["family"] == "safety":
        assert measured["safety"]["harmful_fall"] and measured["entered_deep_water"]


def test_lean_summary_pools_families_seeds_and_physical_evidence_without_samples(tmp_path, monkeypatch):
    episodes = []
    for seed in (7, 19, 43):
        for family in ("point", "object", "follow_below_gate", "jump", "safety"):
            directory = tmp_path / "validation" / family / f"seed-{seed}"
            lean_text_episode(directory, family, seed)
            measured = rl.lean_validation_episode_metrics(directory)
            episodes.append(measured)
            rl.write_json(directory / "drive-metrics.json", measured)
            rl.clean_lean_diagnostics(directory)
    summary = rl.validation_summary(episodes, lean=True)
    assert summary["all"]["n"] == 15
    assert summary["all"]["confirmed_transitions"] == 3
    assert summary["all"]["deep_water_episodes"] == summary["all"]["harmful_fall_episodes"] == 3
    assert summary["by_family"]["jump"]["arrival_fraction"] == 1
    assert summary["by_family"]["follow_below_gate"]["follow_fraction"] == 1
    assert summary["by_seed"]["7"]["all"]["n"] == 5
    assert summary["by_seed"]["7"]["by_family"]["safety"]["harmful_fall_episodes"] == 1
    assert "maneuver_presses" not in summary["all"]
    assert summary["all"]["action_presses"]["jump"]["per_1000_decisions"] == 250
    monkeypatch.setattr(rl, "module", lambda name: pytest.fail("Lean cached summary must not open NAVDEMO/tails"))
    monkeypatch.setattr(rl, "object_failure_summaries", lambda *args: pytest.fail("Lean summary has no sensor tails"))
    output = tmp_path / "summary"
    rl.summarize(SimpleNamespace(source=tmp_path / "validation", output=output))
    saved = rl.read_json(output / "metrics.json")
    assert saved["all"] == summary["all"]
    assert saved["object_failures_last_10s"] == []


@pytest.mark.parametrize("bad_moving_seed", [False, True])
@pytest.mark.parametrize("memory_version", [3, 4])
@pytest.mark.parametrize("journey_override", [None, "0", "1"])
def test_lean_manifest_drive_keeps_three_action_seeds_and_never_retains_navdemo(tmp_path, monkeypatch,
                                                                            bad_moving_seed, memory_version, journey_override,
                                                                            validation_disk_space):
    manifest, groups = validation_manifest(tmp_path)
    args = validation_drive_args(tmp_path, manifest, family=None)
    args.lean, args.jump_links = True, Path("/accepted-links-cv.txt")
    args.weights = tmp_path / "policy.navbc"
    args.weights.write_text(json.dumps(dict(preprocessing=dict(navdemo_version=3 if memory_version == 4 else 2,
        navigation_memory_version=memory_version))) + "\n")
    if journey_override is None:
        monkeypatch.delenv("OPENGOTHIC_NAV_JOURNEY_MEMORY", raising=False)
    else:
        monkeypatch.setenv("OPENGOTHIC_NAV_JOURNEY_MEMORY", journey_override)
    effective_memory_version = memory_version if journey_override is None else 4 if journey_override == "1" else 3
    effective_demo_version = 3 if memory_version == 4 or effective_memory_version == 4 else 2
    if effective_memory_version == 4:
        args.familiar_map = tmp_path / "familiar.bin"
        args.familiar_map.touch()
    for name in ("runner", "assets", "save", "links", "roads"):
        setattr(args, name, tmp_path)
    monkeypatch.setattr(rl.sys, "platform", "linux")
    monkeypatch.setattr(rl, "new_runner", lambda *args: SimpleNamespace(close=lambda: None))
    monkeypatch.setattr(rl, "module", lambda name: pytest.fail("Lean drive must not read samples or retain tails"))
    calls = []
    def scenario(worker, runner, name, commands, directory):
        value = groups[directory.parent.parent.name][0]
        seed = int(directory.name.removeprefix("seed-"))
        assert "navdemos record off" in commands
        if effective_memory_version == 4:
            assert f"roads familiar-load {args.familiar_map}" in commands
        metadata = validation_runner_metadata(value, seed)
        metadata.update(demo_version=effective_demo_version, navigation_memory_version=effective_memory_version)
        moving = value["goal_kind"] in ("approach", "follow")
        metadata.update(start=value["start"],
            goal="RESOLVED_CONNECTED_WAYPOINT" if value["goal_kind"] in ("approach", "follow") else value["goal"],
            seed=value["seed"] + int(bad_moving_seed and moving), record=False, teacher_control_decisions=0)
        rl.write_json(directory / "episode.json", metadata)
        (directory / "known-paths.jsonl").write_text("diagnostic")
        calls.append((value["family"], worker.seed, seed))
    monkeypatch.setattr(rl, "scenario", scenario)
    monkeypatch.setattr(rl, "lean_validation_episode_metrics", lambda directory: dict(success=False, recording="text_only"))
    if bad_moving_seed:
        with pytest.raises(ValueError, match="identity"):
            rl.drive_validation(args)
        return
    rl.drive_validation(args)
    assert set(calls) == {(family, 19, seed) for family in rl.VALIDATION_FAMILIES for seed in rl.ACTION_SEEDS}
    assert len(list(args.output.rglob("drive-metrics.json"))) == 15
    assert not list(args.output.rglob("known-paths.jsonl"))
    assert not list(args.output.rglob("samples.bin*"))


def test_lean_manifest_drive_rejects_disk_below_reserve_before_scenario(tmp_path, monkeypatch,
                                                                     validation_disk_space):
    manifest, _ = validation_manifest(tmp_path)
    args = validation_drive_args(tmp_path, manifest, family="point")
    args.lean, args.action_seeds = True, "7"
    validation_disk_space.free = 20 * 1024**3 + 512 * 1024**2 - 1
    closed = []
    monkeypatch.setattr(rl, "checked_paths", lambda *args, **kwargs: None)
    monkeypatch.setattr(rl, "new_runner", lambda *args: SimpleNamespace(close=lambda: closed.append(True)))
    monkeypatch.setattr(rl, "scenario", lambda *args: pytest.fail("Low disk must prevent scenario execution"))
    with pytest.raises(ValueError, match="20-GiB disk reserve plus 512 MiB"):
        rl.drive_validation(args)
    assert closed == [True]
    assert not list(args.output.rglob("episode.json"))


@pytest.mark.parametrize("corruption", [None, dict(record=True), dict(teacher_control_decisions=1),
    dict(weights="/other.navbc"), dict(action_seed=19), dict(mixing_seed=19), dict(validation_scenario={}), "samples"])
def test_lean_resume_validates_identity_and_cleans_diagnostics_without_rerun(tmp_path, monkeypatch, corruption):
    manifest, groups = validation_manifest(tmp_path)
    value = groups["point"][0]
    args = validation_drive_args(tmp_path, manifest, family="point", resume=True)
    args.lean, args.action_seeds = True, "7"
    directory = args.output / "validation/point" / value["id"] / "seed-7"
    metadata = dict(validation_runner_metadata(value, 7), validation_scenario=value, action_seed=7,
        weights=str(args.weights), record=False, teacher_control_decisions=0, start="A", goal="B")
    if isinstance(corruption, dict):
        metadata.update(corruption)
    rl.write_json(directory / "episode.json", metadata)
    rl.write_json(directory / "drive-metrics.json", dict(success=False, recording="text_only"))
    if corruption == "samples":
        (directory / "samples.bin.gz").write_bytes(b"forbidden")
    for name in ("known-paths.jsonl", "teacher-paths.jsonl", "transition-frames.jsonl",
                 "perception-safety-frames.jsonl", "perception-water-observations.jsonl"):
        (directory / name).write_text("diagnostic")
    monkeypatch.setattr(rl, "checked_paths", lambda *args, **kwargs: None)
    monkeypatch.setattr(rl, "new_runner", lambda *args: SimpleNamespace(close=lambda: None))
    monkeypatch.setattr(rl, "module", lambda name: pytest.fail("Lean resume must not read samples/tails"))
    monkeypatch.setattr(rl, "scenario", lambda *args: pytest.fail("Completed episode was rerun"))
    if corruption:
        with pytest.raises(ValueError):
            rl.drive_validation(args)
    else:
        rl.drive_validation(args)
        assert not list(directory.glob("*.jsonl"))


def test_validation_setup_refusal_continues_and_is_shared_between_policies(tmp_path, monkeypatch):
    manifest, groups = validation_manifest(tmp_path)
    rejected = groups["approach"][0]
    healthy = dict(rejected, id="approach-connected", route=["B", "C"])
    groups["approach"].append(healthy)
    rl.write_json(manifest, dict(schema_version=1, splits=dict(validation=groups)))
    stub_validation_runner(monkeypatch)
    calls, runners, healthy_runners = [], [], []
    reason = "Runner rejected advance: navdemos moving route needs distinct connected points"

    class FakeRunner:
        pending = False
        closes = 0

        def close(self):
            self.closes += 1

    def new_runner(*args):
        runner = FakeRunner()
        runners.append(runner)
        return runner

    monkeypatch.setattr(rl, "new_runner", new_runner)
    def scenario(args, runner, name, commands, directory):
        if runner.pending:
            raise RuntimeError("Runner rejected reset: advance is pending")
        scenario_id = directory.parent.name
        seed = int(directory.name.removeprefix("seed-"))
        calls.append((args.weights.stem, scenario_id, seed))
        if scenario_id == rejected["id"]:
            runner.pending = True
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "runner.log").write_text(reason)
            raise RuntimeError(reason)
        healthy_runners.append(runner)
        rl.write_json(directory / "episode.json", validation_runner_metadata(healthy, seed))

    monkeypatch.setattr(rl, "scenario", scenario)
    monkeypatch.setattr(rl, "validation_episode_metrics", lambda path: dict(success=False))
    for policy in ("bc3", "it35"):
        args = validation_drive_args(tmp_path, manifest, policy, resume=True)
        seed = rl.ACTION_SEEDS[0]
        ready = args.output / "validation/approach" / healthy["id"] / f"seed-{seed}"
        metadata = dict(validation_runner_metadata(healthy, seed), validation_scenario=healthy,
                        action_seed=seed, weights=str(args.weights))
        rl.write_json(ready / "episode.json", metadata)
        rl.write_json(ready / "drive-metrics.json", dict(success=False))
        (ready / "samples.bin.gz").write_bytes(b"ready original samples")
        rl.drive_validation(args)
        assert rl.read_json(ready / "episode.json") == metadata
        assert (ready / "samples.bin.gz").read_bytes() == b"ready original samples"

    assert calls == [("bc3", rejected["id"], rl.ACTION_SEEDS[0])] + [
        (policy, healthy["id"], seed) for policy in ("bc3", "it35") for seed in rl.ACTION_SEEDS[1:]]
    assert [runner.closes for runner in runners] == [1, 1, 1]
    assert runners[0].pending
    assert healthy_runners == [runners[1], runners[1], runners[2], runners[2]]
    saved = rl.read_json(tmp_path / "validation/setup-rejections" / f"{rejected['id']}.json")
    assert saved["id"] == rejected["id"]
    assert saved["family"] == "approach"
    assert saved["validation_scenario"] == rejected
    assert saved["reason"] == reason
    for policy in ("bc3", "it35"):
        assert len(list((tmp_path / "validation" / policy).rglob("episode.json"))) == 3


@pytest.mark.parametrize("failure_stage", ["close", "restart"])
def test_validation_setup_refusal_keeps_runner_replacement_failures_fatal(tmp_path, monkeypatch, failure_stage):
    manifest, _ = validation_manifest(tmp_path)
    stub_validation_runner(monkeypatch)
    started, calls = [], []

    def close():
        if failure_stage == "close":
            raise RuntimeError("Runner close failed")

    def new_runner(*args):
        started.append(True)
        if len(started) > 1:
            raise RuntimeError("Runner restart failed")
        return SimpleNamespace(close=close)

    def scenario(*args):
        calls.append(True)
        raise RuntimeError("Runner rejected advance: navdemos moving route needs distinct connected points")

    monkeypatch.setattr(rl, "new_runner", new_runner)
    monkeypatch.setattr(rl, "scenario", scenario)
    with pytest.raises(RuntimeError, match=f"Runner {failure_stage} failed"):
        rl.drive_validation(validation_drive_args(tmp_path, manifest))
    assert calls == [True]
    assert len(started) == (1 if failure_stage == "close" else 2)


@pytest.mark.parametrize("family,failure", [("approach", RuntimeError("Runner exited with code 1")),
    ("approach", ValueError("NAVDEMO record is corrupt")), ("approach", OSError("No space left on device")),
    ("point", RuntimeError("Runner rejected advance: navdemos moving route needs distinct connected points"))])
def test_validation_keeps_runner_record_and_disk_failures_fatal(tmp_path, monkeypatch, family, failure):
    manifest, _ = validation_manifest(tmp_path)
    stub_validation_runner(monkeypatch)
    calls = []

    def scenario(*args):
        calls.append(True)
        raise failure

    monkeypatch.setattr(rl, "scenario", scenario)
    with pytest.raises(type(failure), match=str(failure)):
        rl.drive_validation(validation_drive_args(tmp_path, manifest, family=family))
    assert calls == [True]
    assert not list((tmp_path / "validation/setup-rejections").glob("*.json"))


def test_validation_summary_removes_shared_setup_refusals_from_both_policies(tmp_path, monkeypatch):
    manifest, groups = validation_manifest(tmp_path)
    rejected = groups["approach"][0]
    reason = "Runner rejected advance: navdemos moving route needs distinct connected points"
    rl.write_json(tmp_path / "validation/setup-rejections" / f"{rejected['id']}.json",
        dict(id=rejected["id"], family="approach", validation_scenario=rejected, reason=reason))
    captured = []
    monkeypatch.setattr(rl, "validation_summary", lambda episodes: captured.append(episodes) or dict(episodes=episodes))
    monkeypatch.setattr(rl, "object_failure_summaries", lambda episodes: [])
    for policy in ("bc3", "it35"):
        output = validation_drive_args(tmp_path, manifest, policy).output
        for identifier in (rejected["id"], "approach-connected"):
            rl.write_json(output / "validation/approach" / identifier / "seed-7/drive-metrics.json",
                dict(id=identifier, family="approach", split="validation", success=False, action_seed=7))
        rl.summarize(SimpleNamespace(source=output, output=output))
        summary = rl.read_json(output / "metrics.json")
        assert [episode["id"] for episode in summary["episodes"]] == ["approach-connected"]
        assert summary["setup_rejections"][0]["id"] == rejected["id"]
        assert summary["setup_rejections"][0]["reason"] == reason
    assert [[episode["id"] for episode in episodes] for episodes in captured] == [
        ["approach-connected"], ["approach-connected"]]


def test_validation_resume_recovers_completed_episode_without_collector_metadata(tmp_path, monkeypatch):
    manifest, groups = validation_manifest(tmp_path)
    args = validation_drive_args(tmp_path, manifest, family="point", resume=True)
    args.action_seeds = "7"
    value = groups["point"][0]
    directory = args.output / "validation/point" / value["id"] / "seed-7"
    rl.write_json(directory / "episode.json", validation_runner_metadata(value, 7))
    (directory / "samples.bin.gz").write_bytes(b"completed original samples")
    stub_validation_runner(monkeypatch)
    monkeypatch.setattr(rl, "scenario", lambda *args: pytest.fail("Completed episode was rerun"))
    measured = []

    def metrics(path):
        measured.append(path)
        metadata = rl.read_json(path / "episode.json")
        assert metadata["validation_scenario"] == value
        assert metadata["action_seed"] == 7
        assert metadata["weights"] == str(args.weights)
        return dict(success=False)

    monkeypatch.setattr(rl, "validation_episode_metrics", metrics)
    rl.drive_validation(args)
    assert measured == [directory]
    assert (directory / "samples.bin.gz").read_bytes() == b"completed original samples"
    assert rl.read_json(directory / "drive-metrics.json") == dict(success=False)


def test_validation_resume_recreates_only_incomplete_episode(tmp_path, monkeypatch):
    manifest, groups = validation_manifest(tmp_path)
    args = validation_drive_args(tmp_path, manifest, family="point", resume=True)
    args.action_seeds = "7"
    value = groups["point"][0]
    directory = args.output / "validation/point" / value["id"] / "seed-7"
    directory.mkdir(parents=True)
    (directory / "partial-samples.bin.gz").write_bytes(b"incomplete")
    neighbour = directory.parent / "seed-19" / "episode.json"
    rl.write_json(neighbour, dict(sentinel="other episode"))
    stub_validation_runner(monkeypatch)
    calls = []

    def scenario(args, runner, name, commands, output):
        assert output == directory
        assert not (output / "partial-samples.bin.gz").exists()
        calls.append(output)
        rl.write_json(output / "episode.json", validation_runner_metadata(value, 7))

    monkeypatch.setattr(rl, "scenario", scenario)
    monkeypatch.setattr(rl, "validation_episode_metrics", lambda path: dict(success=False))
    rl.drive_validation(args)
    assert calls == [directory]
    assert rl.read_json(neighbour) == dict(sentinel="other episode")


def test_object_detour_summary_counts_scenarios_and_keeps_exact_ratio_boundaries(tmp_path):
    probes, episodes = [], []
    cases = [("short", 110, [True, False, False]), ("lower-boundary", 120, [False, True, True]),
             ("upper-boundary", 150, [True, True, True]), ("long", 200, [False, False, False])]
    for identifier, length, wins in cases:
        probes.append(dict(label=identifier, start=f"{identifier}-A", goal=f"{identifier}-B",
                           detour_complete=True, detour_length_cm=length, direct_cm=100))
        episodes.extend(dict(id=identifier, family="object", start=f"{identifier}-A", goal=f"{identifier}-B",
                             action_seed=seed, success=success) for seed, success in zip(rl.ACTION_SEEDS, wins))
    episodes.append(dict(family="point", success=True))
    path = tmp_path / "object-probes.jsonl"
    path.write_text("".join(json.dumps(probe) + "\n" for probe in probes))

    result = rl.object_detour_summary(episodes, path)

    assert result["source"] == str(path)
    scenarios = {value["id"]: value for value in result["scenarios"]}
    assert set(scenarios) == {identifier for identifier, _, _ in cases}
    for identifier, length, wins in cases:
        value = scenarios[identifier]
        assert value["start"] == f"{identifier}-A"
        assert value["goal"] == f"{identifier}-B"
        assert value["detour_length_cm"] == length
        assert value["direct_cm"] == 100
        assert value["detour_ratio"] == pytest.approx(length / 100)
        assert value["episodes"] == 3
        assert value["successes"] == sum(wins)
        assert value["success_fraction"] == pytest.approx(sum(wins) / 3)
    assert {identifier: value["bucket"] for identifier, value in scenarios.items()} == {
        "short": "<1.2", "lower-boundary": "1.2-1.5", "upper-boundary": "1.2-1.5", "long": ">1.5"}
    for bucket, scenario_count, successes, count in (("<1.2", 1, 1, 3), ("1.2-1.5", 2, 5, 6), (">1.5", 1, 0, 3)):
        value = result["by_bucket"][bucket]
        assert (value["scenarios"], value["episodes"], value["successes"]) == (scenario_count, count, successes)
        assert value["success_fraction"] == pytest.approx(successes / count)
    middle = result["by_bucket"]["1.2-1.5"]["by_seed"]
    for seed, successes in zip(rl.ACTION_SEEDS, (1, 2, 2)):
        assert middle[str(seed)] == dict(episodes=2, successes=successes, success_fraction=successes / 2)
    detours = result["detours_over_1.2"]
    assert (detours["scenarios"], detours["episodes"], detours["successes"]) == (2, 6, 3)
    assert detours["success_fraction"] == .5
    assert detours["by_seed"] == {str(seed): dict(episodes=2, successes=1, success_fraction=.5)
                                  for seed in rl.ACTION_SEEDS}


@pytest.mark.parametrize("corruption", [None, dict(detour_complete=False), dict(detour_length_cm=float("nan")),
    dict(direct_cm=float("inf")), dict(direct_cm=0), dict(start="WRONG-A"), dict(goal="WRONG-B")])
def test_object_detour_summary_rejects_missing_or_invalid_selected_probe(tmp_path, corruption):
    identifier = "object-case"
    probe = dict(label=identifier, start="A", goal="B", detour_complete=True, detour_length_cm=150, direct_cm=100)
    if corruption is not None:
        probe.update(corruption)
    path = tmp_path / "object-probes.jsonl"
    path.write_text("" if corruption is None else json.dumps(probe) + "\n")
    episode = dict(id=identifier, family="object", start="A", goal="B", action_seed=7, success=False)
    with pytest.raises(ValueError, match=identifier):
        rl.object_detour_summary([episode], path)


def test_eval_screen_freeze_is_immutable_and_rejects_test(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    screen = rl.module("nav_eval_screen")
    scenarios = {f"point/p{i}": dict(id=f"p{i}", family="point", split="validation", region="market",
        start=f"A{i}", goal=f"B{i}", goal_kind="point", seed=7, recorded=False, episode=None) for i in range(2)}
    episodes = [dict(value, action_seed=seed, success=True, collector_wall_s=1)
                for value in scenarios.values() for seed in rl.ACTION_SEEDS]
    history = dict(scenarios=scenarios, models={model: episodes for model in screen.MODELS})
    path = tmp_path / "screen.jsonl"
    selected, _ = screen.freeze(history, path)
    original = path.read_bytes()
    assert selected == set(scenarios)
    assert len(rl.eval_screen_jobs(path)) == 6
    screen.freeze(history, path)
    assert path.read_bytes() == original
    scenarios["point/p0"]["start"] = "CHANGED"
    with pytest.raises(ValueError, match="Refusing to replace"):
        screen.freeze(history, path)
    scenarios["point/p0"]["split"] = "test"
    with pytest.raises(ValueError, match="cannot include test"):
        screen.freeze(history, tmp_path / "test.jsonl")
    assert path.read_bytes() == original


def test_eval_screen_agreement_uses_paired_routes_and_pooled_follow(monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    screen = rl.module("nav_eval_screen")
    monkeypatch.setattr(screen, "METRICS", ("point", "follow_below_gate"))
    def episodes(wins, band):
        return [dict(id=f"p{i}", family="point", action_seed=seed, success=win)
                for i, win in enumerate(wins) for seed in rl.ACTION_SEEDS] + [
            dict(id=f"f{i}", family="follow_below_gate", action_seed=seed,
                 follow_in_band_ms=value, follow_observed_ms=observed)
            for i, (value, observed) in enumerate(zip(band, (10, 90))) for seed in rl.ACTION_SEEDS]
    full = dict(it35=episodes([0, 0, 0], [0, 0]), bc3=episodes([1, 1, 1], [10, 0]),
                hd=episodes([1, 1, 0], [10, 90]), bc4=episodes([0, 0, 0], [0, 0]))
    delta = screen.paired_difference(full["hd"], full["it35"], "point")
    assert delta["delta"] == pytest.approx(2 / 3)
    assert delta["se"] == pytest.approx(1 / 3)  # Three routes, not nine independent seeds.
    assert delta["routes"] == 3
    following = screen.paired_difference(full["bc3"], full["it35"], "follow_below_gate")
    assert following["left"] == .1  # Pooled 10/100, not the mean episode fraction .5.
    sample = dict(full, it35=full["bc3"], bc3=full["it35"])
    result = screen.agreement(full, sample)
    assert len(result) == 12  # Six pairs for each of two metrics.
    pair = next(row for row in result if (row["metric"], row["left"], row["right"]) == ("point", "it35", "bc3"))
    assert pair["significant"] and not pair["agrees"]
    noise = next(row for row in result if (row["metric"], row["left"], row["right"]) == ("point", "it35", "hd"))
    assert not noise["significant"] and noise["agrees"] is None  # Exactly two SE is not above two SE.
