"""DAgger labels and previous inputs have deliberately different provenance."""
import gzip
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos", Path(__file__).resolve().parents[1] / "nav_demos.py")
demos = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(demos)
DRIVE_SPEC = importlib.util.spec_from_file_location("nav_drive", Path(__file__).resolve().parents[1] / "nav_drive.py")
drive = importlib.util.module_from_spec(DRIVE_SPEC)
DRIVE_SPEC.loader.exec_module(drive)


def write_episode(path, drive=True, corrupt_previous=None, teacher_turn=-1, count=65):
    # Cross the reader's 64-record chunk boundary as well as ordinary adjacent rows.
    path.mkdir()
    records = np.zeros(count, dtype=demos.RECORD_DTYPE)
    records["time_ms"] = 1000 + np.arange(count) * 100
    records["eye"]["distance_cm"] = 1000
    records["keys"]["turn"] = teacher_turn
    records["keys"]["walk"] = 1
    actual = records["keys"].copy()
    if drive:
        actual["forward"] = 1
        actual["turn"] = 1
    records["previous_keys"][1:] = actual[:-1]
    if corrupt_previous is not None:
        records["previous_keys"][corrupt_previous]["turn"] = 0
    metadata = dict(success=False, drive=drive, decisions=count, elapsed_ms=count * 100,
                    eye_layout=[[0, 0, 1000]] * demos.RAY_COUNT)
    (path / "episode.json").write_text(json.dumps(metadata))
    with gzip.open(path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, demos.VERSION, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    actions = np.zeros((count + 1, 9))
    actions[:, 0] = np.arange(count + 1) * 100
    for column, field in enumerate(("forward", "strafe", "turn", "walk", "jump", "climb",
                                    "head_yaw_deg", "head_pitch_deg"), 1):
        actions[:-1, column] = actual[field]
    actions[-1, 4] = 1
    np.savetxt(path / "actions.txt", actions)
    return actions


def test_drive_accepts_teacher_labels_with_actual_learner_previous_keys(tmp_path):
    episode = tmp_path / "drive"
    write_episode(episode)
    result = demos.summarize(episode, validate=True)
    assert result["validated"]
    assert result["decisions"] == 65
    # A failed drive is still retained and checked for DAgger.
    assert result["episodes"] == 1
    assert result["key_counts"]["turn_left"] == 65


@pytest.mark.parametrize("row", [1, 64])
def test_drive_rejects_corrupted_previous_keys_within_and_between_chunks(tmp_path, row):
    episode = tmp_path / "drive"
    write_episode(episode, corrupt_previous=row)
    with pytest.raises(ValueError, match="previous keys mismatch"):
        demos.summarize(episode, validate=True)


def test_teacher_actions_are_still_checked_against_teacher_keys(tmp_path):
    episode = tmp_path / "teacher"
    actions = write_episode(episode, drive=False)
    assert demos.summarize(episode, validate=True)["decisions"] == 65
    actions[1, 3] = 1
    np.savetxt(episode / "actions.txt", actions)
    with pytest.raises(ValueError, match=r"actions.turn differs from recorded keys"):
        demos.summarize(episode, validate=True)


def test_teacher_fractional_axis_preserves_existing_navdemo_contract(tmp_path):
    episode = tmp_path / "teacher"
    write_episode(episode, drive=False, teacher_turn=.5)
    result = demos.summarize(episode, validate=True)
    assert result["validated"]
    assert result["decisions"] == 65
    assert result["key_counts"]["turn_right"] == 65


def test_drive_still_rejects_fractional_applied_axis(tmp_path):
    episode = tmp_path / "drive"
    actions = write_episode(episode)
    actions[1, 3] = .5
    np.savetxt(episode / "actions.txt", actions)
    with pytest.raises(ValueError, match=r"actions.turn is not a digital axis"):
        demos.summarize(episode, validate=True)


def write_analysis_episode(path, changing=True):
    actions = write_episode(path, count=7)
    records = np.concatenate(list(demos.iter_chunks(path)))
    actions[:-1, 1:] = 0
    actions[:-1, 4] = 1
    if changing:
        actions[:2, 3] = 1
        actions[2:6, 1] = 1
        actions[6, 3] = -1
    else:
        actions[:-1, 1] = 1
    for column, field in enumerate(drive.ACTION_FIELDS, 1):
        records["keys"][field] = actions[:-1, column]
        records["previous_keys"][field] = np.r_[0, actions[:-2, column]]
    if changing:
        records["keys"]["turn"][[1, 4, 6]] = -.5
        records["keys"]["forward"][2] = 0
        records["keys"]["forward"][6] = 1
    else:
        # This episode has no teacher-requested changes, including at the start.
        records["previous_keys"][0] = records["keys"][0]
    with gzip.open(path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, demos.VERSION, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    np.savetxt(path / "actions.txt", actions)
    metadata = json.loads((path / "episode.json").read_text())
    metadata.update(limit_ms=700, teacher_success=True, teacher_elapsed_ms=500)
    (path / "episode.json").write_text(json.dumps(metadata))
    positions = np.zeros((8, 5))
    positions[:, 0] = np.arange(8) * 100
    positions[:, 1] = [0, 0, 0, 0, 100, 200, 200, 200]
    np.savetxt(path / "trajectory.txt", positions)


def test_drive_metrics_measure_applied_actions_and_teacher_action_changes(tmp_path):
    episode = tmp_path / "changing"
    write_analysis_episode(episode)
    result = drive.episode_metrics(episode, demos)
    assert result["travelled_xz_cm"] == 200
    assert result["observed_reason"] == "moving_at_timeout"
    assert result["action_counts"] == dict(forward=4, backward=0, strafe_left=0, strafe_right=0,
                                         turn_left=1, turn_right=2, walk=7, jump=0, climb=0, idle=0, turn_only=3)
    assert result["action_changes"] == dict(forward=2, strafe=0, turn=2, walk=0, jump=0, climb=0, any=2)
    assert result["action_repeat_fraction"] == pytest.approx(4 / 6)
    assert result["action_runs"] == 3
    assert result["longest_action_run_steps"] == 4
    assert result["final_action_run_steps"] == 1
    assert result["final_action"]["turn"] == -1
    assert result["disagreement_counts"] == dict(forward=2, strafe=0, turn=2, walk=0, jump=0, climb=0, any=4)
    assert result["action_change_steps"] == 6
    assert result["action_change_accuracy"] == dict(forward=4 / 6, strafe=1, turn=4 / 6, walk=1,
                                                    jump=1, climb=1, all_heads=2 / 6)

    unchanged = tmp_path / "unchanged"
    write_analysis_episode(unchanged, changing=False)
    no_changes = drive.episode_metrics(unchanged, demos)
    assert no_changes["action_change_steps"] == 0
    assert all(value is None for value in no_changes["action_change_accuracy"].values())
    group = drive.group_metrics([result, no_changes])
    assert group["decisions"] == 14
    assert group["action_change_steps"] == 6
    assert group["action_change_accuracy"] == result["action_change_accuracy"]
    assert group["action_counts"]["forward"] == 11
    assert group["action_counts"]["idle"] == 0
    assert group["action_fraction"]["forward"] == pytest.approx(11 / 14)
    assert group["action_changes"]["any"] == 2
    assert group["action_repeat_fraction"] == pytest.approx(10 / 12)
    assert group["longest_action_run_steps"] == 7
    assert group["action_runs"] == 4


def test_teacher_change_is_counted_at_reader_chunk_boundary(tmp_path):
    episode = tmp_path / "boundary"
    write_episode(episode)
    records = np.concatenate(list(demos.iter_chunks(episode)))
    records["keys"]["turn"][64] = 1
    with gzip.open(episode / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, demos.VERSION, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    positions = np.zeros((66, 5))
    positions[:, 0] = np.arange(66) * 100
    np.savetxt(episode / "trajectory.txt", positions)
    result = drive.episode_metrics(episode, demos)
    assert result["action_change_steps"] == 1
    assert result["action_change_accuracy"]["turn"] == 1
    assert result["action_change_accuracy"]["forward"] == 0


def test_prepare_keeps_original_train_pairs_and_only_v2_successful_validation(tmp_path):
    bc_cache, teacher_root, output = (tmp_path / name for name in ("cache", "teacher-v2", "drive"))
    bc_cache.mkdir()
    episodes = []
    for split, count in (("validation", 31), ("train", 36)):
        for index in range(count):
            region = f"region-{index % 6}"
            episodes.append(dict(id=f"old__{split}__{index:03d}", split=split, region=region,
                                 pair=[f"{split}-start-{index:03d}", f"{split}-goal-{index:03d}"],
                                 start=f"{split}-start-{index:03d}", goal=f"{split}-goal-{index:03d}"))
    (bc_cache / "index.json").write_text(json.dumps(dict(episodes=episodes)))
    for index, episode in enumerate(reversed(episodes)):
        directory = teacher_root / "episodes" / f"new-{index:03d}"
        directory.mkdir(parents=True)
        teacher = dict(start=episode["start"], goal=episode["goal"], elapsed_ms=1000,
                       teacher_version=2, demo_version=2,
                       success=not (episode["split"] == "validation" and int(episode["start"][-3:]) < 4))
        (directory / "episode.json").write_text(json.dumps(teacher))
        (directory / "samples.bin.gz").touch()
    args = SimpleNamespace(bc_cache=bc_cache, teacher_root=teacher_root, output=output,
                           pairs=None, selection_seed=17)
    drive.prepare(args)
    selected = drive.load_pairs(args)
    validation = [pair for pair in selected if pair["split"] == "validation"]
    train = [pair for pair in selected if pair["split"] == "train"]
    assert len(validation) == 27
    assert all(pair["teacher_success"] for pair in validation)
    assert len(train) == 30
    assert len([pair for pair in selected if pair["split"] == "control"]) == 1
    expected = []
    rng = drive.random.Random(17)
    for region in sorted({episode["region"] for episode in episodes if episode["split"] == "train"}):
        group = sorted((episode for episode in episodes if episode["region"] == region and episode["split"] == "train"),
                       key=lambda episode: (episode["start"], episode["goal"]))
        expected.extend(sorted(rng.sample(group, 5), key=lambda episode: (episode["start"], episode["goal"])))
    assert [pair["source_cache_id"] for pair in train] == [episode["id"] for episode in expected]
    # A repeated perturbed start cannot replace the ordinary reference.
    perturbation = teacher_root / "dagger" / "000"
    perturbation.mkdir(parents=True)
    (perturbation / "episode.json").write_text(json.dumps(dict(teacher_version=2, demo_version=2,
        start="validation-start-000", goal="validation-goal-000", success=True, dagger=0)))
    (perturbation / "samples.bin.gz").touch()
    drive.prepare(args)
    assert len([pair for pair in drive.load_pairs(args) if pair["split"] == "validation"]) == 27
    failed_reference = Path(train[0]["teacher_episode"]) / "episode.json"
    failed_teacher = json.loads(failed_reference.read_text())
    failed_teacher["success"] = False
    failed_reference.write_text(json.dumps(failed_teacher))
    args.output = tmp_path / "drive-with-train-failure"
    drive.prepare(args)
    train_with_failure = [pair for pair in drive.load_pairs(args) if pair["split"] == "train"]
    assert [pair["source_cache_id"] for pair in train_with_failure] == [pair["source_cache_id"] for pair in train]
    assert train_with_failure[0]["teacher_success"] is False


@pytest.mark.parametrize("beta", [-.1, 1.1, float("nan")])
def test_collect_rejects_invalid_mixing_probability(beta):
    with pytest.raises(ValueError, match="teacher-beta"):
        drive.collect(SimpleNamespace(teacher_beta=beta, dagger_collection=True))


def test_measurement_rejects_teacher_control():
    with pytest.raises(ValueError, match="Measured drives"):
        drive.collect(SimpleNamespace(teacher_beta=.5, dagger_collection=False))


@pytest.mark.parametrize("mode,suffix", [(None, ""), ("argmax", ""), ("sample", " sample")])
def test_policy_arguments_preserve_greedy_command_and_seed_sample_by_original_pair(mode, suffix):
    args = SimpleNamespace(limit_ms=240000, weights=Path('/policy.navbc'), teacher_beta=0,
                           mixing_seed=91)
    if mode is not None:
        args.action_mode = mode
    assert drive.policy_arguments(args, 7) == '240000 /policy.navbc 0 98' + suffix
    assert drive.policy_arguments(args, 8) == '240000 /policy.navbc 0 99' + suffix


def test_sample_collection_passes_mode_to_drive_and_drive_current_and_checks_runner(tmp_path, monkeypatch):
    pool = [dict(index=0, split='validation', split_index=0, region='lobart', npc='NPC',
                 start='START', goal='GOAL', seed=7),
            dict(index=1, split='control', split_index=0, region='lobart', npc='NPC',
                 start='intentional', goal='FENCE', seed=7)]
    monkeypatch.setattr(drive, 'load_pairs', lambda _: pool)
    monkeypatch.setattr(drive, 'indices', lambda _args, _pairs: [0, 1])
    monkeypatch.setattr(drive.sys, 'platform', 'linux')
    measured = []
    class Runner:
        def __init__(self, args, prepare):
            pass
        def scenario(self, commands, directory, seed, **kwargs):
            starts = [line for line in commands if line.startswith(('navdemos drive ', 'navdemos drive-current '))]
            if not starts:
                return
            measured.extend(starts)
            directory.mkdir(parents=True)
            for name in ('trajectory.txt', 'actions.txt', 'samples.bin.gz'):
                (directory / name).touch()
            drive.write_json(directory / 'episode.json', dict(physics_mode='cv', demo_version=2,
                teacher_version=2, teacher_beta=0, mixing_seed=int(starts[0].split()[-2]),
                action_mode=starts[0].split()[-1], success=True, elapsed_ms=100))
        def close(self):
            pass
    collector = SimpleNamespace(scenario_file=lambda args, name, commands: commands,
                                runner_type=lambda: Runner, check_log=lambda _: None)
    monkeypatch.setattr(drive, 'module', lambda _: collector)
    args = SimpleNamespace(teacher_beta=0, dagger_collection=False, action_mode='sample',
                           list_indices=False, runner=tmp_path, assets=tmp_path, save=tmp_path,
                           cache=tmp_path / 'cache', links=tmp_path, roads=tmp_path, weights=tmp_path,
                           output=tmp_path / 'out', limit_ms=240000, mixing_seed=91,
                           watchdog_seconds=900, selection_seed=17, physics='cv')
    drive.collect(args)
    assert measured == [f'navdemos drive NPC START GOAL 240000 {tmp_path} 0 91 sample',
                        f'navdemos drive-current NPC FENCE 240000 {tmp_path} 0 92 sample']
    for pair in pool:
        result = drive.read_json(drive.episode_dir(args, pair) / 'episode.json')
        assert result['action_mode'] == result['requested_action_mode'] == 'sample'


@pytest.mark.parametrize("existing", ["argmax", "sample"])
def test_resume_does_not_reuse_greedy_recording_as_sample(tmp_path, existing):
    pair = dict(index=0, split='validation', split_index=0)
    args = SimpleNamespace(output=tmp_path, split='all', indices=None, start_index=0,
                           count=None, resume=True, action_mode='sample')
    directory = drive.episode_dir(args, pair)
    directory.mkdir(parents=True)
    for name in ('trajectory.txt', 'actions.txt', 'samples.bin.gz'):
        (directory / name).touch()
    drive.write_json(directory / 'episode.json', dict(action_mode=existing))
    if existing == 'argmax':
        with pytest.raises(ValueError, match='resume action mode differs'):
            drive.indices(args, [pair])
    else:
        assert drive.indices(args, [pair]) == []


def test_live_trace_fixture_renders_and_keeps_metrics_factual(tmp_path):
    """Exercise the packed trace reader and both graphics on the same tiny run."""
    import shutil
    import subprocess

    pytest.importorskip("PIL")
    if not shutil.which("ffmpeg"):
        pytest.skip("ffmpeg is required for the recorded topdown")
    spec = importlib.util.spec_from_file_location("nav_live_render", Path(__file__).resolve().parents[1] / "nav_live_render.py")
    live = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(live)
    root = tmp_path / "live"
    root.mkdir()
    maps = {name: dict(shape=[8, 4, 4], channels=["confidence", "free", "wall", "ledge", "drop", "water", "movable", "familiar"],
                       cell_cm=25, min_forward_cm=-50, min_right_cm=-50, height_channels=[])
            for name in ("near", "far")}
    metadata = dict(schema=1, maps=maps, ray_count=1, detour_cm=[[0, 0, 0], [120, 0, 0]])
    (root / "live-format.json").write_text(json.dumps(metadata))
    decisions = []
    for index in range(12):
        decisions.append(dict(elapsed_ms=index*100, decision_index=index, position_cm=[index*10, 0, 0], yaw_deg=0,
                              goal_cm=[300, 0, 0], carrot_cm=[250, 0, 0], speed_cm_s=0 if index < 2 else 100,
                              keys=dict(forward=1, strafe=0, turn=1 if index < 2 else 0, walk=True, jump=False, climb=False),
                              teacher_keys={}, probabilities=dict(forward=[.1, .2, .7]), selected=dict(forward=2),
                              chosen_probability=dict(forward=.7), entropy=dict(forward=.801819),
                              body_blocked=2 <= index < 4, stuck=2 <= index < 5, contact=index in (6, 9), safety_intervention=index == 6,
                              edge_clearance_cm=500, edge_clearance_censored=True,
                              water_clearance_cm=250 if index == 5 else 500, water_clearance_censored=index != 5))
    (root / "live-decisions.jsonl").write_text("".join(json.dumps(row)+"\n" for row in decisions))
    (root / "episode.json").write_text(json.dumps(dict(success=True, elapsed_ms=1200, travel_cm=110, goal_kind="follow")))
    safety = [dict(water_depth_cm=value, deep_water_threshold_cm=100, falling=index == 1)
              for index, value in enumerate((0, 120, 125, 0, 110))]
    (root / "perception-safety-frames.jsonl").write_text("".join(json.dumps(row)+"\n" for row in safety))
    memory = np.zeros((8, 4, 4), dtype=np.uint8)
    memory[0] = 255
    memory[1] = 255
    memory[1, 0] = 0
    memory[2, 0] = 255
    memory[7, 2, 2] = 255
    rays = np.zeros(1, dtype=live.RAY_DTYPE)
    rays["point_cm"][0] = [100, 0, 0]
    rays["direction"][0] = [1, 0, 0]
    rays["distance_cm"] = 100
    rays["hit"] = rays["complete"] = rays["kind"] = 1
    with gzip.open(root / "live-observations.bin.gz", "wb") as stream:
        stream.write(live.HEADER.pack(live.MAGIC, 1))
        for index in (0, 4, 8):
            stream.write(live.FRAME.pack(index*100, index))
            stream.write(memory.tobytes())
            stream.write(memory.tobytes())
            stream.write(rays.tobytes())
    assert live.RAY_DTYPE.itemsize == 55
    classes = live.memory_classes(memory, maps["near"])
    assert classes[0, 0] == 2 and classes[2, 2] == 5 and classes[3, 3] == 1
    world = live._world_map(memory, maps["near"], decisions[0], [0, 0], 1, (100, 100))
    assert world.getpixel((20, 50))[:3] == tuple(live.CLASS_COLORS[2])
    assert world.getpixel((80, 50))[:3] == tuple(live.CLASS_COLORS[1])
    rotated = dict(decisions[0], yaw_deg=90)
    world = live._world_map(memory, maps["near"], rotated, [0, 0], 1, (100, 100))
    assert world.getpixel((50, 80))[:3] == tuple(live.CLASS_COLORS[2])
    report = live.render_run(root)
    assert report["decision_count"] == 12 and report["observation_frames"] == 3
    assert report["standing_seconds"] == pytest.approx(.2)
    assert report["circling_seconds"] == pytest.approx(.2)
    assert report["obstacle_contacts"] == 2 and report["deep_water_entries"] == 2 and report["falls"] == 1
    assert report["stuck_seconds"] == pytest.approx(.3)
    assert report["minimum_edge_distance_cm"] == 500 and report["minimum_edge_distance_censored"] is True
    assert report["minimum_water_distance_cm"] == 250 and report["minimum_water_distance_censored"] is False
    assert report["path_to_teacher_ratio"] is None
    assert report["follow_band_fraction"] == pytest.approx(11/12)
    assert (root / "timeline.png").read_bytes().startswith(b"\x89PNG")
    probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "json", str(root / "topdown.mp4")], capture_output=True, text=True, check=True)
    assert float(json.loads(probe.stdout)["format"]["duration"]) == pytest.approx(.6, abs=.051)
    observations = root / "live-observations.bin.gz"
    with gzip.open(observations, "rb") as stream:
        truncated = stream.read()[:-1]
    with gzip.open(observations, "wb") as stream:
        stream.write(truncated)
    with pytest.raises(ValueError, match="Truncated"):
        list(live.observation_frames(root, metadata))
