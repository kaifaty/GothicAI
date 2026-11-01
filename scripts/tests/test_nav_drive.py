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
    assert result["forced_pace"] is None
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
    assert group["forced_pace"] is None
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
    monkeypatch.setenv("OPENGOTHIC_NAV_FORCE_PACE", "walk")
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
        assert result['forced_pace'] == 'walk'


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
        args.force_pace = "walk"
        with pytest.raises(ValueError, match='resume forced pace differs'):
            drive.indices(args, [pair])


def test_live_trace_fixture_renders_and_keeps_metrics_factual(tmp_path, monkeypatch):
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
    monkeypatch.setenv("OPENGOTHIC_BASE_FINGERPRINT", "b" * 64)
    assert live.summarize(root, metadata, decisions)["base_fingerprint"] is None
    episode = json.loads((root / "episode.json").read_text())
    episode["base_fingerprint"] = "a" * 64
    (root / "episode.json").write_text(json.dumps(episode))
    report = live.render_run(root)
    assert report["base_fingerprint"] == "a" * 64
    assert report["decision_count"] == 12 and report["observation_frames"] == 3
    assert report["standing_seconds"] == pytest.approx(.2)
    assert report["circling_seconds"] == pytest.approx(.2)
    assert report["obstacle_contacts"] == 2 and report["deep_water_entries"] == 2 and report["falls"] == 1
    assert report["stuck_seconds"] == pytest.approx(.3)
    assert report["minimum_edge_distance_cm"] == 500 and report["minimum_edge_distance_censored"] is True
    assert report["minimum_water_distance_cm"] == 250 and report["minimum_water_distance_censored"] is False
    assert report["path_to_teacher_ratio"] is None
    assert report["follow_band_fraction"] == pytest.approx(11/12)
    assert report['follow_mean_distance_m'] == pytest.approx(2.45)
    assert (root / "timeline.png").read_bytes().startswith(b"\x89PNG")
    probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "json", str(root / "topdown.mp4")], capture_output=True, text=True, check=True)
    assert float(json.loads(probe.stdout)["format"]["duration"]) == pytest.approx(.6, abs=.051)
    (root / 'target-trajectory.txt').write_text('0 0 0 0 100\n200 0 0 0 300\n1000 0 0 0 900\n1200 0 0 0 9999\n')
    protected = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in root.iterdir() if path.name != 'summary.json'}
    refreshed = live.refresh_follow_summary(root)
    assert refreshed['follow_mean_distance_m'] == pytest.approx(11/3)
    assert {key: value for key, value in refreshed.items() if key not in ('follow_mean_distance_m', 'metric_sources')} == {
        key: value for key, value in report.items() if key not in ('follow_mean_distance_m', 'metric_sources')}
    assert {key: value for key, value in refreshed['metric_sources'].items() if key != 'follow_mean_distance'} == {
        key: value for key, value in report['metric_sources'].items() if key != 'follow_mean_distance'}
    assert all((path.read_bytes(), path.stat().st_mtime_ns) == value for path, value in protected.items())
    summary_time = (root / 'summary.json').stat().st_mtime_ns
    assert live.refresh_follow_summary(root) == refreshed and (root / 'summary.json').stat().st_mtime_ns == summary_time
    observations = root / "live-observations.bin.gz"
    with gzip.open(observations, "rb") as stream:
        truncated = stream.read()[:-1]
    with gzip.open(observations, "wb") as stream:
        stream.write(truncated)
    with pytest.raises(ValueError, match="Truncated"):
        list(live.observation_frames(root, metadata))


@pytest.mark.parametrize("failure", [None, "checksum", "permission"])
def test_live_binary_pin_copies_nas_bytes_and_probes_local_loader_before_game(tmp_path, monkeypatch, failure):
    import hashlib
    import subprocess
    import sys

    scripts = Path(__file__).resolve().parents[1]
    launcher = (scripts / "nav-dagger4-remote.sh").read_text()
    phase = launcher.split("<<'LIVE_R1_PHASE'\n", 1)[1].split("\nLIVE_R1_PHASE", 1)[0]
    block = phase.split("<<'LIVE_R1_PIN'\n", 1)[1].split("\nLIVE_R1_PIN", 1)[0]
    payload = b"fixture native binary"
    expected = hashlib.sha256(payload).hexdigest()
    archive, local = tmp_path / "archive", tmp_path / "local"
    binary = archive / expected / "Gothic2Notr"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"corrupt fixture" if failure == "checksum" else payload)
    binary.chmod(0o644)
    destination = local / expected / "Gothic2Notr"
    calls = []
    run_shell = subprocess.run

    def probe(command, **kwargs):
        calls.append(command)
        assert command == [str(destination)]
        assert kwargs["env"]["LD_TRACE_LOADED_OBJECTS"] == "1"
        assert kwargs["capture_output"] and kwargs["text"] and kwargs["timeout"] == 15
        assert destination.read_bytes() == payload
        assert destination.stat().st_mode & 0o111
        if failure == "permission":
            raise PermissionError("fixture permission denied")
        return subprocess.CompletedProcess(command, 0, stdout="fixture loader ready\n", stderr="")

    monkeypatch.setattr(subprocess, "run", probe)
    monkeypatch.setattr(sys, "argv", ["LIVE_R1_PIN", str(archive), str(local), expected])
    if failure is None:
        exec(compile(block, "LIVE_R1_PIN", "exec"), {})
    else:
        exception = ValueError if failure == "checksum" else RuntimeError
        message = "Archived live binary checksum differs" if failure == "checksum" else "Cannot execute local live binary"
        with pytest.raises(exception, match=message):
            exec(compile(block, "LIVE_R1_PIN", "exec"), {})
    assert binary.stat().st_mode & 0o777 == 0o644
    if failure == "checksum":
        assert not local.exists() and calls == []
        return
    assert calls == [[str(destination)]]
    assert hashlib.sha256(destination.read_bytes()).hexdigest() == expected
    assert destination.stat().st_mode & 0o777 == 0o755
    if failure is None:
        # Execute only the worker's export and bench's host-label block.
        export = next(line for line in phase.splitlines() if line == "export RYZEN_BENCH=1")
        bench = (scripts / "bench.sh").read_text()
        host = "HOST=mac\n" + bench.split("\nHOST=mac\n", 1)[1].split("\nREPORT_RC=0", 1)[0]
        output = tmp_path / "host"
        output.mkdir()
        result = run_shell(["bash", "-c", 'set -euo pipefail\nOUT="$1"\nunset RYZEN_BENCH\n'
                            + export + "\n" + host, "live-host-test", str(output)],
                           capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, result.stderr
        assert (output / "host.txt").read_text() == "ryzen\n"


@pytest.mark.parametrize("failure,explicit", [("live_setup_no_safe_observer_position", True),
                                             ("dagger_start_not_supported", False)])
def test_live_setup_rejection_preserves_diagnostics_without_rendering(tmp_path, monkeypatch, failure, explicit):
    import subprocess
    import time

    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    import nav_live as live

    suite = tmp_path / "suite.json"
    suite.write_text("{}")
    case = dict(id="01-maleth-fence", number=1, npc="NPC", start="START", goal="GOAL",
                time=[10, 0], seeds=[18102027])
    args = SimpleNamespace(output=tmp_path, suite=suite, binary=tmp_path / "game", binary_sha256="binary",
                           input_sha256={}, inputs=dict(save="save", roads="roads", familiar="familiar",
                                                        links="links", cache="cache"),
                           deadline=time.monotonic() + 300)
    args.binary.write_text("fixture binary")
    directory = live.teacher_directory(args, case)
    rejected = dict(success=False, failure=failure, elapsed_ms=0, decisions=0)
    if explicit:
        rejected['setup_rejected'] = True
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        live.write_json(directory / "episode.json", rejected)
        (directory / "live-guide-start.json").write_text('{"candidates":[]}')
        return subprocess.CompletedProcess(command, 1)

    monkeypatch.setattr(live, "check_space", lambda unused: 0)
    monkeypatch.setattr(live.subprocess, "run", run)
    assert live.run_episode(args, case, None, case['seeds'][0], directory) == dict(rejected, setup_rejected=True)
    failures = live.teachers(args, [case])
    assert len(calls) == 1 and failures[0]['failure'] == rejected['failure']
    assert failures[0]['setup_rejected'] and failures[0]['decisions'] == 0
    assert (directory / "live-guide-start.json").read_text() == '{"candidates":[]}'
    assert not (directory / "summary.json").exists()


def test_live_qualification_keeps_place_failures_and_never_runs_learners(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    import nav_live as live
    import nav_live_prepare as prepare

    inputs = {}
    for name in ('save', 'roads', 'familiar', 'links'):
        path = tmp_path / name
        live.write_json(path, dict(points={}, places=[]))
        inputs[name] = str(path)
    live.write_json(tmp_path / 'v4-manifest.json', dict(split_policy=dict(object=dict(groups=dict(test=[])))))
    (tmp_path / 'water-scenarios.jsonl').write_text('')
    detour = dict(id='detour-val', split='validation', start='START', goal='GOAL', goal_kind='point',
                  place_id='NEW', geometry=dict(length_cm=3000, start_cm=[0, 0, 0], goal_cm=[1000, 0, 0]))
    point_detour = dict(detour, id='detour-point', geometry=dict(detour['geometry'], length_cm=4000))
    point_detour.pop('place_id')
    (tmp_path / 'detour-val.jsonl').write_text(json.dumps(detour) + '\n' + json.dumps(point_detour) + '\n')
    identifiers = ['01-maleth-fence', '02-routine-return', '03-follow-hero', '04-visible-detour', '05-wall-pocket',
                   '06-harbour-water', '07-cliff-trail', '08-market-crowd', '09-door-stairs', '10-local-stranger']
    scenarios = [dict(id=identifiers[number-1], number=number, name=f'Scene {number}', npc='NPC', start='START',
        goal='GOAL', goal_kind='point', time=[10, 0], split='validation', source='fixture', training_eligible=False)
        for number in range(1, 11)]
    scenarios[1]['variants'] = [dict(id=name, seeds=[seed]) for name, seed in
                              zip(('forest-tree', 'farm-edge', 'forest-pine'), live.ACTION_SEEDS)]
    scenarios[2]['route'] = ['START', 'GOAL', 'THIRD', 'FOURTH']
    suite = tmp_path / 'suite.json'
    live.write_json(suite, dict(schema_version=1, training_eligible=False, seeds=live.ACTION_SEEDS,
                               inputs=inputs, scenarios=scenarios))
    output, runtime = tmp_path / 'round', tmp_path / 'runtime'
    live.write_json(output / 'preparation/complete.json', dict(complete=True))
    geometry = output / 'preparation/geometry'
    geometry.mkdir()
    (geometry / 'points-v3.jsonl').write_text('\n'.join(json.dumps(dict(name=name, position_cm=point))
        for name, point in (('START', [0, 0, 0]), ('GOAL', [1000, 0, 0]))))
    (geometry / 'roads-events.jsonl').write_text(json.dumps(dict(kind='roads_actors',
        actors=[dict(instance='NPC', routines=[dict(point='GOAL')])])) + '\n')
    live.write_json(output / 'preparation/market-census.json', dict(peak=dict(time=[6, 59])))
    for name in ('gothic-body-runner', 'Gothic2Notr'):
        path = runtime / 'build/opengothic' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    support = dict(supported=True, dry=True, capsule_clear=True, ground_known=True, height_delta_cm=0, ground_snap_cm=0)
    probe = dict(detour_complete=True, route_sample_count=2, route_deep_water_samples=0, route_no_floor_samples=0,
        start_support=support, goal_support=support, detour_length_cm=3000, detour_ratio=3,
        first_detour_goal_dot=-1, goal_visible=True, direct_deep_water_samples=1,
        direct_obstruction=dict(hit=True, label='ROCK', material_name='stone', world_hit=True, normal_y=0,
                                point_cm=[500, 90, 0]),
        direct_sensor=dict(kind='world'), route_min_edge_clearance_cm=300, route_edge_clearance_censored=True,
        probe=dict(edge_drop_cm=100))
    monkeypatch.setattr(prepare, 'test_pairs', lambda unused: set())
    monkeypatch.setattr(prepare, 'water_pool', lambda base, *unused: [dict(base, probe_label='water')])
    monkeypatch.setattr(prepare, 'Worker', lambda *unused: SimpleNamespace(close=lambda: None))
    monkeypatch.setattr(prepare, 'probe_groups', lambda worker, cases, *unused:
        {case['probe_label']: dict(probe=probe, file='native.jsonl') for case in cases})
    monkeypatch.setenv('GOTHIC_DIR', str(tmp_path))
    monkeypatch.setattr(live, 'check_space', lambda unused: 0)
    monkeypatch.setattr(live, 'raw_complete', lambda directory, *unused: (directory / 'episode.json').is_file())
    monkeypatch.setattr(live, 'processed', lambda directory: (directory / 'summary.json').is_file())
    calls = []
    original_run_episode = live.run_episode

    failed_cases = {identifiers[4]}

    def episode(args, case, model, seed, directory):
        assert model is None
        calls.append(case['id'])
        live.write_json(directory / 'episode.json', dict(success=case['id'] not in failed_cases, elapsed_ms=1000, decisions=10,
                                                       failure='timeout' if case['id'] in failed_cases else None))
        live.write_json(directory / 'summary.json', dict(schema=1, deep_water_entries=0, falls=0,
                                                       minimum_edge_distance_cm=200))
        live.write_json(directory / 'identity.json', dict(actual=live.actual_inputs(args, case, model, seed)))
        live.write_json(directory / 'live-guide-start.json', dict(selected_cm=[0, 0, 0]))

    monkeypatch.setattr(live, 'run_episode', episode)
    args = SimpleNamespace(suite=suite, output=output, runtime=runtime, weights='missing-HD,missing-it35')
    assert prepare.qualify(args) == 0
    frozen = live.read_json(output / 'qualified-suite.json')
    result = live.read_json(next((output / 'qualification').glob('*/result.json')))
    assert len(calls) == 11 and identifiers[6] not in calls and identifiers[9] in calls
    assert frozen['qualification_status'] == 'partial' and not frozen['teacher_qualification']['complete']
    assert frozen['teacher_qualification']['passed_placements'] == 10
    assert frozen['scenarios'][3]['source']['id'] == 'detour-point'
    assert len(result['unresolved']) == 2 and result['baseline_runs'] == 0
    market = dict(scenarios[7], prepare_commands=['wait 4000'])
    commands = live.commands(market, None, live.ACTION_SEEDS[0], dict(inputs, cache='cache'))
    assert commands.index('goto START') < commands.index('wait 4000') < commands.index('rec source')
    hero = dict(market, target='PC_HERO')
    commands = live.commands(hero, None, live.ACTION_SEEDS[0], dict(inputs, cache='cache'))
    assert commands.index('navdemos wait') < commands.index('hp PC_HERO') < commands.index('stop')
    assert 'hp PC_HERO' not in live.commands(market, None, live.ACTION_SEEDS[0], dict(inputs, cache='cache'))
    report = (output / 'README.md').read_text()
    assert '| v3b |' in report and '0/54' in report and identifiers[4] + ': unqualified' in report
    assert all(f'| {number}. Scene {number} |' in report for number in range(1, 11))
    frozen['semantic_confirmation_required'] = [identifiers[3]]
    next(row for row in frozen['teacher_qualification']['placements'] if row['id'] == identifiers[9])['passed'] = False
    snapshot = tmp_path / 'baseline-suite.json'
    live.write_json(snapshot, frozen)
    live.write_json(output / 'qualified-suite.json', dict(frozen, teacher_qualification={}))
    argv = ['baseline', '--suite', str(snapshot), '--output', str(output),
            '--runtime', str(runtime), '--weights', args.weights]
    baseline_calls, smoke_calls = [], []
    policies = [dict(name=name, sha256=name, weights=name) for name in ('HD', 'it35')]

    def smoke_gate(args, cases, models, passed=True):
        assert args.suite == snapshot and len(cases) == 8
        smoke_calls.append(passed)
        live.write_json(output / 'smoke.json', dict(complete=passed, suite_sha256=live.sha256(snapshot),
            binary_sha256=live.sha256(args.binary), model_sha256=models[0]['sha256']))
        return passed

    with monkeypatch.context() as subset:
        subset.setattr(live, 'models', lambda *unused: (policies, dict(name='bc4', sha256='bc4', weights='bc4')))
        subset.setattr(live, 'smoke', smoke_gate)
        subset.setattr(live, 'report', lambda *unused: False)
        subset.setattr(live, 'run_episode', lambda args, case, model, seed, *unused:
                       baseline_calls.append((case['id'], model['name'], seed)))
        assert live.main([*argv, '--shard', '0']) == 0
        assert live.main([*argv, '--shard', '1']) == 0
        assert smoke_calls == [True] and len(baseline_calls) == 24
        assert len(set(baseline_calls)) == 24 and not any(row[0] == identifiers[9] for row in baseline_calls)
        gate = live.read_json(output / 'smoke.json')
        live.write_json(output / 'smoke.json', dict(gate, suite_sha256='stale'))
        subset.setattr(live, 'smoke', lambda *values: smoke_gate(*values, passed=False))
        assert live.main([*argv, '--shard', '2']) == 2
        assert live.main([*argv, '--shard', '2']) == 2
        assert smoke_calls == [True, False] and len(baseline_calls) == 24
    (output / 'smoke.json').unlink()
    live.write_json(output / 'qualified-suite.json', frozen)

    # Eight accepted references remain on their original binary while four
    # numbered scenarios are replaced, splitting the door from rejected stairs.
    live_args = SimpleNamespace(suite=output / 'qualified-suite.json', output=output,
        binary=runtime / 'build/opengothic/Gothic2Notr', input_sha256={name: live.sha256(path) for name, path in inputs.items()})
    failed_cases.clear()
    frozen['scenarios'][6]['qualification_evidence'] = dict(probe=probe)
    cliff = next(case for case in live.suite_cases(frozen) if case['number'] == 7)
    frozen['unresolved_geometry'] = []
    episode(live_args, cliff, None, cliff['seeds'][0], live.teacher_directory(live_args, cliff))
    pocket = next(case for case in live.suite_cases(frozen) if case['number'] == 5)
    episode(live_args, pocket, None, pocket['seeds'][0], live.teacher_directory(live_args, pocket))
    frozen['teacher_qualification']['placements'] = [live.teacher_outcome(live_args, case)
                                                   for case in live.suite_cases(frozen)]
    live.write_json(output / 'qualified-suite.json', frozen)
    old_digest = live.sha256(live_args.binary)
    old_binary = runtime / 'binaries' / old_digest / 'Gothic2Notr'
    old_binary.parent.mkdir(parents=True)
    old_binary.write_bytes(live_args.binary.read_bytes())
    live_args.binary.write_text('new fixture binary')
    new_digest = live.sha256(live_args.binary)
    assert new_digest != old_digest

    current = live.read_json(suite)
    follow = current['scenarios'][2]
    follow.update(start='FOLLOW_START', goal_kind='follow', target='PC_HERO', live_target='player-walk',
                  prepare_commands=['immortal player on'])
    assert 'navdemos live-target player-walk' in live.commands(follow, None, live.ACTION_SEEDS[0], dict(inputs, cache='cache'))
    assert live.behavioral_case(follow) != live.behavioral_case(dict(follow, live_target='player'))
    current['scenarios'][8]['variants'] = [dict(id='door', goal='GROUND'),
        dict(id='stairs', goal='UPSTAIRS', qualification_rejection=dict(reason='No qualified indoor ascent'))]
    street = current['scenarios'][9]
    street.update(start='NW_CITY_UPTOWN_PATH_08', goal='NW_CITY_UPTOWN_PATH_12')
    graph = tmp_path / 'street-graph.json'
    live.write_json(graph, dict(node_columns=['name', 'x_cm', 'y_cm', 'z_cm'],
        edge_columns=['a', 'b', 'length_cm', 'major'],
        nodes=[[street['start'], 0, 900, 0], [street['goal'], 5000, 900, 0]], edges=[[0, 1, 5000, False]]))
    street['source'] = dict(graph=str(graph), street_nodes=[street['start'], street['goal']])
    validation = [dict(detour, id=f'detour-{index:03d}', geometry=dict(detour['geometry'],
        length_cm=200000 if index == 0 else 3000 + index)) for index in range(121)]
    (tmp_path / 'detour-val.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in validation))
    live.write_json(suite, current)
    probe_calls, physics_calls = [], []

    def physics(name, commands, directory, seed):
        physics_calls.append(name)
        order = live.read_json(directory / 'candidate-order.json')
        assert len(order) == 121 and any('physics dump' in command for command in commands)
        (directory / 'physics-results.jsonl').write_text(''.join(json.dumps(dict(kind='ray', id=index, hit=True, point=[500, 90, 0],
            sector='ROCK_PASS' if identifier == 'detour-120' else 'STONE')) + '\n'
            for index, identifier in enumerate(order)))

    def native(worker, candidates, *unused):
        probe_calls.append(candidates)
        return {case['probe_label']: dict(probe=dict(probe,
            direct_cm=5000, direct_sensor=dict(hit=True, kind='world'),
            detour_corners_cm=[[0, 0, 0], [5000, 0, 0]]), file='native.jsonl') for case in candidates}

    monkeypatch.setattr(prepare, 'Worker', lambda *unused: SimpleNamespace(close=lambda: None, scenario=physics))
    monkeypatch.setattr(prepare, 'probe_groups', native)
    before = {path: path.read_bytes() for path in (output / 'teacher').rglob('episode.json')}
    calls.clear()
    assert prepare.requalify(args) == 0
    expected = {identifiers[2], identifiers[3], identifiers[8] + '-door', identifiers[9]}
    assert set(calls) == expected and len(calls) == 4
    assert len(probe_calls) == 1 and len(physics_calls) == 1
    hero_legs = [case for case in probe_calls[0] if case['npc'] == 'PC_HERO']
    assert len(hero_legs) == 4 and all(case['goal_kind'] == 'point' for case in hero_legs)
    assert not any(case['id'] == identifiers[8] + '-stairs' for case in probe_calls[0])
    checked = live.read_json(output / 'qualified-suite.json')
    checked_cases = live.suite_cases(checked)
    assert len(checked_cases) == 13 and len(live.eligible_cases(checked, checked_cases)) == 12
    assert checked['scenarios'][3]['source']['id'] == 'detour-120'
    assert checked['scenarios'][3]['teacher_minimum_edge_cm'] == 100
    assert checked['scenarios'][9]['qualification_evidence']['street_route']['qualified']
    assert not (output / 'complete.json').exists()
    assert all(path.read_bytes() == contents for path, contents in before.items())
    mixed_args = SimpleNamespace(output=output, binary=Path(checked['evaluation_binary']['path']),
                                 input_sha256=live_args.input_sha256)
    preserved = [case for case in checked_cases if case['number'] not in (3, 4, 9, 10)]
    assert len(preserved) == 8
    for case in preserved:
        assert live.case_binary(mixed_args, case) == (old_binary, old_digest)
        directory = live.teacher_directory(mixed_args, case)
        assert live.actual_inputs(mixed_args, case, None, case['seeds'][0]) == live.read_json(directory / 'identity.json')['actual']
    rock = next(case for case in checked_cases if case['number'] == 4)
    assert live.case_binary(mixed_args, rock)[1] == new_digest
    rock_summary = live.teacher_directory(mixed_args, rock) / 'summary.json'
    summary = live.read_json(rock_summary)
    live.write_json(rock_summary, dict(summary, minimum_edge_distance_cm=99.9))
    assert not live.teacher_outcome(mixed_args, rock)['passed']
    live.write_json(rock_summary, dict(summary, minimum_edge_distance_cm=100))
    assert live.teacher_outcome(mixed_args, rock)['passed']
    monkeypatch.setattr(live, 'run_episode', original_run_episode)
    assert prepare.requalify(args) == 0
    assert len(calls) == 4 and len(physics_calls) == 1
    old_binary.write_text('corrupt preserved binary')
    with pytest.raises(ValueError, match='differs from its qualified reference'):
        prepare.requalify(args)
    old_binary.write_bytes(b'Gothic2Notr')

    # A fresh round uses the frozen authored placements, including the door
    # variant, without selecting a different routine, coast, terrain or time.
    fixed = json.loads(json.dumps(checked))
    fixed['places_frozen'] = True
    fixed.pop('evaluation_binary', None)
    fixed['scenarios'][7]['time'] = [17, 30]
    for scenario in fixed['scenarios']:
        scenario.pop('evaluation_binary', None)
        scenario.pop('teacher_reference', None)
        for variant in scenario.get('variants', []):
            variant.pop('evaluation_binary', None)
            variant.pop('teacher_reference', None)
    for number in (3, 4):
        fixed['scenarios'][number-1]['qualification_rejection'] = dict(reason='Frozen unqualified placement')
    stairs = fixed['scenarios'][8]['variants'][1]['qualification_rejection']
    stairs.update(start_cm=[0, 0, 0], goal_cm=[0, 400, 0], topdown_frame=str(output / 'stairs-frame.png'))
    fixed_suite, fixed_output = tmp_path / 'fixed-suite.json', tmp_path / 'next-round'
    live.write_json(fixed_suite, fixed)
    live.write_json(fixed_output / 'preparation/complete.json', dict(complete=True))
    fixed_probes = []

    def frozen_probe(worker, candidates, *unused):
        fixed_probes.extend(candidates)
        return {case['probe_label']: dict(probe=dict(probe, route_min_edge_clearance_cm=100,
            route_edge_clearance_censored=False), file='native.jsonl') for case in candidates}

    with monkeypatch.context() as frozen_round:
        frozen_round.setattr(prepare, 'probe_groups', frozen_probe)
        frozen_round.setattr(prepare, 'nearest_routine', lambda *unused: pytest.fail('Frozen routine was reselected'))
        frozen_round.setattr(prepare, 'water_pool', lambda *unused: pytest.fail('Frozen coast was reselected'))
        frozen_round.setattr(live, 'run_episode', episode)
        assert prepare.qualify(SimpleNamespace(suite=fixed_suite, output=fixed_output,
                                               runtime=runtime, weights=args.weights)) == 0
    original_fixed = {case['id']: case for case in live.suite_cases(fixed)}
    assert len(fixed_probes) == 10 and all(case['probe_label'] == case['id'] + '--fixed' for case in fixed_probes)
    assert all((case['start'], case['goal']) == (original_fixed[case['id']]['start'], original_fixed[case['id']]['goal'])
               for case in fixed_probes)
    frozen_result = live.read_json(fixed_output / 'qualified-suite.json')
    assert frozen_result['scenarios'][7]['time'] == [17, 30]
    assert frozen_result['scenarios'][8]['variants'][0]['goal'] == 'GROUND'
    assert '../round/stairs-frame.png' in (fixed_output / 'README.md').read_text()

    # Resume the accepted subset in one job: 36 existing records and 18 new
    # records, including both BC4 controls. Rejected places stay explicit.
    final = json.loads(json.dumps(checked))
    for outcome in final['teacher_qualification']['placements']:
        if outcome['id'] in (identifiers[2], identifiers[3]):
            outcome['passed'] = False
    final_suite = tmp_path / 'final-baseline-suite.json'
    live.write_json(final_suite, final)
    live.write_json(output / 'qualified-suite.json', final)
    final_cases = live.eligible_cases(final, live.suite_cases(final))
    bc4 = dict(name='bc4', sha256='bc4', weights='bc4')
    planned = list(live.runs(final_cases, policies, bc4))
    assert len(final_cases) == 10 and len(planned) == 54
    final_args = SimpleNamespace(output=output, binary=Path(final['evaluation_binary']['path']),
                                 input_sha256=live_args.input_sha256)
    added = []

    def learner(args, case, model, seed, directory, teacher=None, recorded=True):
        if not directory.is_relative_to(output / 'smoke'):
            added.append((case['id'], model['name'], seed))
        live.write_json(directory / 'episode.json', dict(success=True, elapsed_ms=1000, decisions=10))
        summary = dict(schema=1)
        if case['number'] == 3:
            index = live.ACTION_SEEDS.index(seed)
            summary.update(follow_band_fraction=(.2, .9, .6)[index], follow_mean_distance_m=(20, 2, 6)[index],
                           standing_seconds=(50, 2, 10)[index])
        live.write_json(directory / 'summary.json', summary)
        live.write_json(directory / 'identity.json', dict(actual=live.actual_inputs(args, case, model, seed, recorded)))
        for name in ('source.mp4', 'topdown.mp4', 'timeline.png', 'actions.txt', 'trajectory.txt',
                     'live-format.json', 'live-decisions.jsonl', 'live-observations.bin.gz'):
            (directory / name).write_bytes(b'fixture')
        if model['name'] == 'bc4':
            base_id = case['id'].rsplit('-', 1)[0] if case['number'] == 10 else case['id']
            assert teacher == live.teacher_directory(args, next(row for row in live.suite_cases(live.read_json(args.suite)) if row['id'] == base_id))

    for case, model, seed in planned[:36]:
        learner(final_args, case, model, seed, live.run_directory(output, case, model, seed))
    added.clear()
    ready = {path: path.read_bytes() for path in output.rglob('identity.json')}
    gate = dict(complete=True, suite_sha256=live.sha256(final_suite), binary_sha256=live.sha256(final_args.binary),
                model_sha256=policies[0]['sha256'], result='Same-seed and recording checks passed.')
    live.write_json(output / 'smoke.json', gate)
    with monkeypatch.context() as final_round:
        final_round.setattr(live, 'models', lambda *unused: (policies, bc4))
        final_round.setattr(live, 'run_episode', learner)
        assert live.main(['baseline', '--suite', str(final_suite), '--output', str(output),
                          '--runtime', str(runtime), '--weights', args.weights]) == 0
    marker = live.read_json(output / 'complete.json')
    assert marker['qualified_subset_complete'] and not marker['full_suite_qualified']
    assert marker['completed'] == marker['expected'] == 54
    assert marker['reused_runs'] == 36 and marker['newly_completed_runs'] == len(added) == 18
    assert len(marker['excluded_placements']) == 3 and all(path.read_bytes() == data for path, data in ready.items())
    extended = json.loads(json.dumps(final))
    extended['scenarios'][2].update(teacher_qualification_required=False, bc4_compare=True)
    extended['scenarios'][3]['bc4_compare'] = True
    follow = extended['scenarios'][2]
    reference = live.teacher_directory(final_args, follow)
    live.write_json(reference / 'episode.json', dict(success=False, failure='tracking_failed', goal_kind='follow',
                                                    elapsed_ms=90000, decisions=900))
    live.write_json(reference / 'summary.json', dict(schema=1, deep_water_entries=0, falls=0,
        follow_band_fraction=.282457, follow_mean_distance_m=14.954769, standing_seconds=20))
    follow_outcome = next(row for row in extended['teacher_qualification']['placements'] if row['id'] == identifiers[2])
    follow_outcome.update(passed=False, elapsed_ms=90000, decisions=900)
    assert len(list(live.runs(live.eligible_cases(extended, live.suite_cases(extended)), policies, bc4))) == 63
    next(row for row in extended['teacher_qualification']['placements'] if row['id'] == identifiers[3])['passed'] = True
    extended_suite = tmp_path / 'extended-suite.json'
    live.write_json(extended_suite, extended)
    live.write_json(output / 'qualified-suite.json', extended)
    assert len(list(live.runs(live.eligible_cases(extended, live.suite_cases(extended)), policies, bc4))) == 72
    ready = {path: path.read_bytes() for case, model, seed in planned
             for path in live.run_directory(output, case, model, seed).iterdir()}
    added.clear()
    teacher_checks = []
    with monkeypatch.context() as extension:
        extension.setattr(live, 'models', lambda *unused: (policies, bc4))
        extension.setattr(live, 'run_episode', learner)
        extension.setattr(live, 'teachers', lambda args, cases: teacher_checks.extend(case['id'] for case in cases) or [])
        assert live.main(['baseline', '--suite', str(extended_suite), '--output', str(output),
                          '--runtime', str(runtime), '--weights', args.weights]) == 0
    marker = live.read_json(output / 'complete.json')
    assert marker['completed'] == marker['expected'] == 72
    assert marker['reused_runs'] == 54 and marker['newly_completed_runs'] == len(added) == 18
    assert sum(model == 'bc4' for identifier, model, seed in added) == 6
    assert identifiers[2] not in teacher_checks and not marker['full_suite_qualified']
    assert all(path.read_bytes() == data for path, data in ready.items())
    row = next(line for line in (output / 'README.md').read_text().splitlines() if '| 3. ' in line)
    assert 'unqualified teacher reference' in row and '28.2457%' in row and '14.954769m' in row
    assert 'median [18102029' in row and 'worst [18102027' in row
    final_suite = extended_suite
    gate = live.read_json(output / 'smoke.json')
    for name in ('base-cache', '.venv', 'uv-cache', 'navmesh-cache', 'requalification-worker'):
        (runtime / name).mkdir(exist_ok=True)
    source = runtime / 'source/snapshot/phase.sh'
    source.parent.mkdir(parents=True)
    source.write_text('retained')
    live.write_json(output / 'smoke.json', dict(gate, complete=False))
    assert not live.cleanup(runtime, output, final_suite) and (runtime / 'build').is_dir()
    live.write_json(output / 'smoke.json', gate)
    remove = live.shutil.rmtree

    def recorded_cleanup(path):
        assert live.read_json(output / 'complete.json')['local_cleanup']['status'] == 'started'
        remove(path)
        if path.name == 'binaries':
            raise RuntimeError('Fixture cleanup interruption')

    with monkeypatch.context() as cleaned:
        cleaned.setattr(live.shutil, 'rmtree', recorded_cleanup)
        with pytest.raises(RuntimeError, match='Fixture cleanup interruption'):
            live.cleanup(runtime, output, final_suite)
    with monkeypatch.context() as resumed:
        resumed.setattr(live, 'models', lambda *unused: (policies, bc4))
        assert live.completed_round(runtime, output, args.weights)
        assert live.completed_round(runtime, output, args.weights)
        resumed.setattr(live, 'models', lambda *unused: ([dict(policies[0], sha256='changed')], bc4))
        with pytest.raises(ValueError, match='use a new round label'):
            live.completed_round(runtime, output, args.weights)
    assert live.cleanup(runtime, output, final_suite)
    assert source.read_text() == 'retained' and (output / 'README.md').is_file()
    assert not (runtime / 'build').exists() and not (runtime / 'binaries').exists()
    assert live.read_json(output / 'complete.json')['local_cleanup']['status'] == 'complete'


def test_live_qualification_resumes_partial_probes_and_rejects_blocked_capsule(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    import nav_live_prepare as prepare

    cases = [dict(npc="NPC", probe_label=str(index), start=f"START{index}", goal="GOAL") for index in range(2)]
    calls = []

    def scenario(name, commands, directory, seed):
        calls.append(name)
        (directory / "live-probes.jsonl").write_text("".join(
            json.dumps(dict(label=row["probe_label"], npc=row["npc"], start=row["start"], goal=row["goal"])) + "\n"
            for row in cases))

    worker = SimpleNamespace(scenario=scenario)
    measured = prepare.probe_groups(worker, cases, tmp_path, 7)
    path = Path(measured["0"]["file"])
    path.write_text(path.read_text().splitlines()[0] + '\n{"label":')
    assert len(prepare.probe_groups(worker, cases, tmp_path, 7)) == 2
    assert len(calls) == 2
    prepare.probe_groups(worker, cases, tmp_path, 7)
    assert len(calls) == 2

    support = dict(supported=True, dry=True, capsule_clear=True, ground_known=True,
                   height_delta_cm=0, ground_snap_cm=0)
    probe = dict(detour_complete=True, route_sample_count=2, route_deep_water_samples=0,
                 route_no_floor_samples=0, start_support=dict(support, capsule_clear=False), goal_support=support)
    assert not prepare.supported_route(probe)


def test_live_extension_checks_budget_and_compares_follow_with_a_safe_counter(tmp_path, monkeypatch, capsys):
    import time

    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    import nav_live as live
    import nav_live_prepare as prepare
    import nav_live_render as render

    monkeypatch.setenv('GOTHIC_DIR', str(tmp_path))
    output, runtime = tmp_path / 'round', tmp_path / 'runtime'
    inputs = {}
    for name in ('save', 'roads', 'familiar', 'links'):
        path = tmp_path / 'input' / name
        live.write_json(path, {})
        inputs[name] = str(path)
    binaries = []
    for content in ('old game', 'new game'):
        source = runtime / 'extension-binaries' / content / 'Gothic2Notr'
        source.parent.mkdir(parents=True)
        source.write_text(content)
        digest = live.sha256(source)
        pin = runtime / 'binaries' / digest / source.name
        pin.parent.mkdir(parents=True)
        pin.write_bytes(source.read_bytes())
        binaries.append(dict(path=str(pin), sha256=digest))
    rescued = runtime / 'extension-binaries'
    (rescued / 'gothic-body-runner').write_text('native runner')
    (rescued / 'navmesh-cache').mkdir()
    (rescued / 'navmesh-cache/world.navmesh').write_text('accepted mesh')
    identifiers = ['01-maleth-fence', '02-routine-return', '03-follow-hero', '04-visible-detour',
                   '05-wall-pocket', '06-harbour-water', '07-cliff-trail', '08-market-crowd',
                   '09-door-stairs', '10-local-stranger']
    scenarios = [dict(id=identifier, number=number, name=f'Scene {number}', npc='NPC',
        start=f'START{number}', goal=f'GOAL{number}', goal_kind='point', time=[10, 0], limit_ms=90000,
        training_eligible=False, split='validation', source='fixture', qualification_evidence=dict(checked=True),
        evaluation_binary=binaries[0] if number in (1, 2, 5, 6, 7, 8) else binaries[1])
        for number, identifier in enumerate(identifiers, 1)]
    scenarios[1]['variants'] = [dict(id=name, seeds=[seed]) for name, seed in
                               zip(('forest-tree', 'farm-edge', 'forest-pine'), live.ACTION_SEEDS)]
    scenarios[2].update(goal_kind='follow', target='PC_HERO', live_target='player-walk',
        route=['ROUTE0', 'ROUTE1', 'ROUTE2', 'ROUTE3'],
        target_route_evidence=dict(complete=True, legs=[dict(supported_route=True)] * 4),
        qualification_rejection=dict(reason='old teacher failed'))
    scenarios[3].update(npc='NONE_100_XARDAS', goal_kind='object')
    scenarios[8]['variants'] = [dict(id='door'), dict(id='stairs', qualification_rejection=dict(reason='No indoor ascent'))]
    native_root = output / 'qualification/previous'
    frozen = dict(schema_version=1, training_eligible=False, seeds=live.ACTION_SEEDS,
        inputs=inputs, scenarios=scenarios, evaluation_binary=binaries[1], qualification_source=str(native_root),
        semantic_confirmation_required=[identifiers[2], identifiers[3]],
        unresolved_geometry=[dict(id=identifiers[2], reason='old teacher failed')], qualified_suite_sha256='stale')
    actual_args = SimpleNamespace(binary=Path(binaries[1]['path']), output=output,
        input_sha256={name: live.sha256(path) for name, path in inputs.items()})
    outcomes = []
    for case in live.suite_cases(frozen):
        passed = case['number'] not in (3, 4) and not case['id'].endswith('-stairs')
        directory = live.teacher_directory(actual_args, case)
        live.write_json(directory / 'episode.json', dict(success=passed, elapsed_ms=180000, decisions=1800,
            goal_kind=case['goal_kind'], failure=None if passed else 'timeout'))
        live.write_json(directory / 'summary.json', dict(schema=1, deep_water_entries=0, falls=0,
                                                       minimum_edge_distance_cm=200))
        live.write_json(directory / 'identity.json', dict(actual=live.actual_inputs(actual_args, case, None, case['seeds'][0])))
        outcomes.append(dict(id=case['id'], passed=passed, elapsed_ms=180000, decisions=1800,
            falls=0, deep_water_entries=0, output=str(directory)))
    frozen['teacher_qualification'] = dict(complete=False, binary_sha256=binaries[1]['sha256'], placements=outcomes)
    live.write_json(output / 'qualified-suite.json', frozen)
    current = json.loads(json.dumps(frozen))
    current['scenarios'][2].update(teacher_qualification_required=False, bc4_compare=True)
    current['scenarios'][3].update(name='Visible fixed obstacle', bc4_compare=True)
    suite = tmp_path / 'suite.json'
    live.write_json(suite, current)
    for pin in binaries:
        Path(pin['path']).unlink()
    live.write_json(output / 'runtime.json', dict(wall_seconds=5000))
    args = SimpleNamespace(suite=suite, output=output, runtime=runtime, weights='HD,it35')
    monkeypatch.setattr(live, 'phase_deadline', lambda: time.monotonic() + 2700)
    assert prepare.extend(args) == 2
    budget = json.loads(capsys.readouterr().out)
    assert budget['needed_seconds'] == 2800 and budget['deficit_seconds'] > 0
    assert not any(Path(pin['path']).exists() for pin in binaries)
    monkeypatch.setattr(live, 'phase_deadline', lambda: time.monotonic() + 3300)
    monkeypatch.setattr(live, 'check_space', lambda unused: 0)
    monkeypatch.setattr(live, 'raw_complete', lambda directory, *unused: (directory / 'episode.json').is_file())
    monkeypatch.setattr(live, 'processed', lambda directory: (directory / 'summary.json').is_file())
    support = dict(supported=True, dry=True, capsule_clear=True, ground_known=True, ground_snap_cm=0, height_delta_cm=0)
    validation, probes = [], []
    for index in range(121):
        identifier = f'detour-{index:03d}'
        validation.append(dict(id=identifier, split='validation', start=f'cm:{1000 + index},750,0',
            goal='cm:0,800,0', goal_kind='object', target_type='house',
            obstacle_type='wall_or_fence' if index == 2 else 'unknown'))
        probes.append(dict(label='rock-' + identifier, npc='NONE_100_XARDAS',
            start=validation[-1]['start'], goal=validation[-1]['goal'], goal_visible=index < 79,
            direct_obstruction=dict(hit=True, label='COUNTER' if index == 0 else 'HOUSEWALL', point_cm=[100, 840, 0]),
            direct_sensor=dict(hit=True, kind='static_vob', point_cm=[100, 840, 0]), detour_complete=True,
            detour_length_cm=1500, route_sample_count=2, route_no_floor_samples=0, route_deep_water_samples=0,
            start_support=support, goal_support=dict(support, height_delta_cm=-50)))
    (tmp_path / 'input/detour-val.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in validation))
    native_file = native_root / 'native/group/live-probes.jsonl'
    native_file.parent.mkdir(parents=True)
    native_file.write_text(''.join(json.dumps(row) + '\n' for row in probes))
    live.write_json(native_root / 'rock-sectors/candidate-order.json', [row['id'] for row in validation])
    (native_root / 'rock-sectors/physics-results.jsonl').write_text(''.join(json.dumps(
        dict(kind='ray', id=index, sector='ARMORY')) + '\n' for index in range(121)))
    monkeypatch.setattr(prepare, 'test_pairs', lambda unused: set())
    worker_args, point_queries, teacher_calls, baseline_calls = [], [], [], []

    def worker(options, *unused):
        worker_args.append(options)
        return SimpleNamespace(close=lambda: None)

    def native(worker, cases, *unused):
        point_queries.extend(cases)
        return {case['probe_label']: dict(file='new-start.jsonl', probe=dict(probes[0],
            start_support=support, goal_support=support, route_min_edge_clearance_cm=200)) for case in cases}

    def teacher(options, case, model, seed, directory):
        assert model is None
        teacher_calls.append(case)
        live.write_json(directory / 'episode.json', dict(success=True, elapsed_ms=1000, decisions=10, failure=None))
        live.write_json(directory / 'summary.json', dict(schema=1, deep_water_entries=0, falls=0, minimum_edge_distance_cm=100))
        live.write_json(directory / 'identity.json', dict(actual=live.actual_inputs(options, case, model, seed)))

    def refresh(directory):
        summary = dict(live.read_json(directory / 'summary.json'), follow_mean_distance_m=42)
        live.write_json(directory / 'summary.json', summary)
        return summary

    monkeypatch.setattr(prepare, 'Worker', worker)
    monkeypatch.setattr(prepare, 'probe_groups', native)
    monkeypatch.setattr(live, 'run_episode', teacher)
    monkeypatch.setattr(render, 'refresh_follow_summary', refresh)
    monkeypatch.setattr(live, 'main', lambda argv: baseline_calls.append(argv) or 0)
    before = {path: path.read_bytes() for path in (output / 'teacher').rglob('identity.json')}
    assert prepare.extend(args) == 0
    assert len(worker_args) == 1 and worker_args[0].watchdog_seconds == 120
    assert len(point_queries) == len(teacher_calls) == 1
    assert point_queries[0]['start'] == point_queries[0]['goal'] == validation[0]['start']
    assert teacher_calls[0]['source']['id'] == validation[0]['id'] and teacher_calls[0]['limit_ms'] == 90000
    assert teacher_calls[0]['goal'] == validation[0]['goal'] and teacher_calls[0]['name'] == 'Visible fixed obstacle'
    assert all(path.read_bytes() == contents for path, contents in before.items())
    assert len(baseline_calls) == 1 and baseline_calls[0][0] == 'baseline' and '--shard' not in baseline_calls[0]
    checked = live.read_json(output / 'qualified-suite.json')
    assert checked['scenarios'][2]['teacher_qualification_required'] is False
    assert checked['scenarios'][2]['bc4_compare'] and checked['scenarios'][3]['bc4_compare']
    assert 'qualification_rejection' not in checked['scenarios'][2]
    assert not checked['full_suite_qualified'] and not checked['teacher_qualification']['complete']
    assert checked['semantic_confirmation_required'] == [] and 'qualified_suite_sha256' not in checked
    extension = next((output / 'extension').iterdir())
    assert live.read_json(extension / 'suite.json') == checked
    result = live.read_json(extension / 'result.json')
    assert result['validation_candidates'] == 121 and result['visible_goals'] == 79
    assert result['static_candidates'] == 1 and result['selected_static'] == validation[0]['id']
