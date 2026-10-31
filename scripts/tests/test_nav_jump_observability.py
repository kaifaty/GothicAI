"""Sensor privacy, ray geometry, fall labels and held-out threshold diagnostics."""
import gzip
import json
import csv
import subprocess
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_jump_observability as visibility

demos = visibility.nav_demos


def fixture_records(count=3):
    records = np.zeros(count, dtype=demos.RECORD_V2_DTYPE)
    records["time_ms"] = np.arange(1, count + 1)*100
    records["keys"]["forward"] = 1
    records["keys"]["walk"] = 1
    records["body"]["velocity_cm_s"][:, 2] = 100
    records["roads"]["carrot_direction"][:, 2] = 1
    layout = np.tile([0., 0., 1000.], (demos.RAY_COUNT, 1))
    layout[:3, :2] = [[-10, -45], [0, -45], [10, -45]]
    layout[3, 1] = -45
    records["eye"]["distance_cm"] = 1000
    records["eye"]["distance_cm"][:, :3] = 180*np.sqrt(2)
    records["eye"]["distance_cm"][:, 3] = 100*np.sqrt(2)
    records["eye"]["hit"][:, :4] = 1
    records["eye"]["kind"][:, :4] = 1
    records["eye"]["normal_y"][:, :3] = 1
    for name in ("memory_near", "memory_far"):
        records[name]["height_valid"] = 1
        records[name]["ground_height_cm"] = 12345
    return records, layout


def write_episode(path, split="train", site="place", records=None):
    if records is None:
        records, layout = fixture_records()
        records["keys"]["jump"][0] = 1
        records["eye"]["hit"][1:, 3] = 0
    else:
        _, layout = fixture_records()
    path.mkdir(parents=True)
    metadata = dict(start=path.name, goal="goal", split=split, site_id=site,
                    transition_id=site + "-link", eye_layout=layout.tolist())
    (path / "episode.json").write_text(json.dumps(metadata))
    with gzip.open(path / "samples.bin.gz", "wb") as stream:
        stream.write(demos.HEADER.pack(demos.MAGIC, 2, demos.RECORD_BYTES,
                                      demos.RAY_COUNT, demos.NEAR_SIZE, demos.FAR_SIZE, demos.PERIOD_MS))
        stream.write(records.tobytes())
    return path


def test_geometry_uses_movement_frame_and_only_learner_inputs():
    records, layout = fixture_records(1)
    records["body"]["head_yaw_deg"] = 90
    records["body"]["velocity_cm_s"] = [100, 0, 0]
    records["memory_near"]["channels"][0, 32, 39, 3] = 200
    expected = visibility.sensor_features(demos.learner_input(records), layout)
    row = expected[0]
    assert row["movement_deg"] == pytest.approx(90)
    assert row["carrot_movement_angle_deg"] == pytest.approx(-90)
    assert row["eye_height_proxy_cm"] == pytest.approx(180, abs=.01)
    assert row["obstacle_distance_cm"] == pytest.approx(100, abs=.01)
    assert row["visible_hit_height_cm"] == pytest.approx(80, abs=.01)
    assert row["obstacle_ray"] == 3
    assert row["near_ledge"] == pytest.approx(200/255)
    records["position_cm"] = [99999, -99999, 99999]
    records["body_yaw_deg"] = 123
    records["teacher_hint"]["path_cm"] = 99999
    records["keys"]["climb"] = 1
    for name in ("memory_near", "memory_far"):
        records[name]["ground_height_cm"] += 20000
    actual = visibility.sensor_features(demos.learner_input(records), layout)
    for key, value in row.items():
        if isinstance(value, float) and np.isnan(value):
            assert np.isnan(actual[0][key])
        else:
            assert actual[0][key] == value
    with pytest.raises(ValueError, match="learner_input"):
        visibility.sensor_features({**demos.learner_input(records), "teacher_hint": records["teacher_hint"]}, layout)


def test_rotation_composition_is_not_simple_yaw_pitch_addition():
    layout = np.array([[90., 0., 1000.], [0., 0., 1000.], [0., 90., 1000.]])
    directions = visibility.ray_directions(layout, 90, 30)
    np.testing.assert_allclose(np.linalg.norm(directions, axis=1), 1)
    np.testing.assert_allclose(directions[0], [0, 0, -1], atol=1e-6)
    np.testing.assert_allclose(directions[1], [np.sqrt(3)/2, .5, 0], atol=1e-6)
    np.testing.assert_allclose(directions[2], [-.5, np.sqrt(3)/2, 0], atol=1e-6)


def test_missing_ground_or_cached_rays_never_fabricates_height():
    records, layout = fixture_records(2)
    records["memory_near"]["height_valid"][0] = 0
    records["memory_far"]["height_valid"][0] = 0
    records["eye"]["age_ms"][1, :4] = 500
    rows = visibility.sensor_features(demos.learner_input(records), layout)
    assert np.isnan(rows[0]["eye_height_proxy_cm"])
    assert np.isnan(rows[0]["visible_hit_height_cm"])
    assert rows[0]["ahead_surface_distance_cm"] == pytest.approx(100)
    assert rows[1]["ahead_hit_rays"] == 0
    assert np.isnan(rows[1]["obstacle_distance_cm"])


def test_drop_is_last_grounded_decision_before_uncommanded_substantial_fall():
    times = np.arange(8)*100
    heights = np.array([100, 100, 95, 70, 35, 30, 30, 30])
    flags = np.zeros((8, 5), dtype=np.uint8)
    flags[2:5, :2] = 1
    jump = np.zeros(8, dtype=bool)
    moving = np.ones(8, dtype=bool)
    result = visibility.drop_onsets(times, heights, flags, jump, jump.copy(), moving)
    np.testing.assert_array_equal(np.flatnonzero(result), [1])
    # A real jump, a staircase (no airborne state), and a shallow step are not drops.
    jump[1] = True
    assert not visibility.drop_onsets(times, heights, flags, jump, np.zeros(8, dtype=bool), moving).any()
    assert not visibility.drop_onsets(times, heights, np.zeros_like(flags), jump*False, jump*False, moving).any()
    assert not visibility.drop_onsets(times, heights/10, flags, jump*False, jump*False, moving).any()


def feature_row(split, distance, height, label):
    return dict(split=split, obstacle_distance_cm=distance, visible_hit_height_cm=height,
                jump=label, climb=False, ordinary_walk=not label)


def test_rule_thresholds_fit_train_only_and_penalize_walking_false_positives():
    training = [feature_row("train", 80, 80, True), feature_row("train", 90, 80, True),
                feature_row("train", 150, 80, False), feature_row("train", 80, 200, False)]
    held_out = [feature_row("test", 590, 290, True)]*50
    rule = visibility.fit_rule(training)
    assert visibility.fit_rule(training + held_out) == rule
    assert rule["distance_cm"] == 100
    assert rule["max_height_cm"] == 100
    assert rule["train_f1"] == 1
    assert not visibility.predict_rule(held_out, rule).any()
    assert visibility.fit_rule([feature_row("unsplit", 80, 80, True)]) is None


def test_analysis_streaming_labels_cross_chunk_boundaries_and_does_not_rewrite(tmp_path):
    records, _ = fixture_records(8)
    records["position_cm"][:, 1] = [100, 100, 95, 70, 35, 30, 30, 30]
    records["body"]["flags"][2:5, :2] = 1
    source = write_episode(tmp_path / "episode", records=records)
    before = {path.name: path.read_bytes() for path in source.iterdir()}
    report1, rows1 = visibility.analyze([source], chunk_records=1)
    report3, rows3 = visibility.analyze([source], chunk_records=3)
    assert report1["counts"]["drop_onset"] == 1
    assert rows1[1]["drop_onset"]
    assert visibility._clean(rows1) == visibility._clean(rows3)
    assert report1 == report3
    assert {path.name: path.read_bytes() for path in source.iterdir()} == before
    assert not report1["geometry"]["exact"]
    assert "omits eye origin" in report1["limitations"][0]


def test_external_site_split_is_used_and_leakage_rejected(tmp_path):
    train = write_episode(tmp_path / "a", split="unsplit", site="")
    test = write_episode(tmp_path / "b", split="unsplit", site="")
    metadata = tmp_path / "routes.json"
    rows = [dict(episode="a", site_id="train-site", transition_id="train-link", split="train"),
            dict(start="b", goal="goal", site_id="test-site", transition_id="test-link", split="test")]
    metadata.write_text(json.dumps(dict(routes=rows)))
    report, decisions = visibility.analyze([train, test], metadata)
    assert report["rule"]["fit_split"] == "train"
    assert {row["split"] for row in decisions} == {"train", "test"}
    rows[1]["site_id"] = "train-site"
    metadata.write_text(json.dumps(rows))
    with pytest.raises(ValueError, match="crosses splits"):
        visibility.analyze([train, test], metadata)


def test_duplicate_recordings_and_input_budget_are_rejected(tmp_path):
    source = write_episode(tmp_path / "episode")
    with pytest.raises(ValueError, match="duplicate recording"):
        visibility.analyze([source, source / "samples.bin.gz"])
    with pytest.raises(ValueError, match="input budget"):
        visibility.analyze([source], max_input_gb=1e-12)


def test_cli_writes_portable_json_and_csv_and_protects_sources(tmp_path):
    source = write_episode(tmp_path / "episode")
    output, csv_path = tmp_path / "analysis.json", tmp_path / "decisions.csv"
    command = [sys.executable, "-I", visibility.__file__, str(source),
               "--output", str(output), "--csv", str(csv_path)]
    completed = subprocess.run(command, text=True, capture_output=True)
    assert completed.returncode == 0, completed.stderr
    payload = json.loads(output.read_text())
    assert payload["counts"]["jump"] == 1
    assert "NaN" not in output.read_text()
    with csv_path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 3
    assert rows[0]["jump"] == "True"
    command[command.index("--output") + 1] = str(source / "episode.json")
    rejected = subprocess.run(command, text=True, capture_output=True)
    assert rejected.returncode != 0
    assert "outside recording source" in rejected.stderr
    assert json.loads((source / "episode.json").read_text())["start"] == "episode"


def patch_metadata(path, **values):
    source = path / "episode.json"
    metadata = json.loads(source.read_text())
    metadata.update(values)
    source.write_text(json.dumps(metadata))


def test_successful_only_uses_original_transition_outcome_and_criterion_not_raw_goal(tmp_path, monkeypatch):
    accepted = write_episode(tmp_path / "accepted", site="a")
    goal_only = write_episode(tmp_path / "goal-only", site="b")
    old_criterion = write_episode(tmp_path / "old", site="c")
    no_goal = write_episode(tmp_path / "no-goal", site="d")
    for path in (accepted, goal_only, old_criterion, no_goal):
        patch_metadata(path, success=True, transition_success=True, success_criterion=visibility.DEMO_CRITERION)
    patch_metadata(goal_only, transition_success=False)
    patch_metadata(old_criterion, success_criterion="old_probe_60cm")
    patch_metadata(no_goal, success=False)
    external = tmp_path / "routes.json"
    external.write_text(json.dumps([dict(episode="goal-only", success=True, transition_success=True,
                                         success_criterion=visibility.DEMO_CRITERION)]))
    original = visibility.sensor_features
    calls = []
    def count_sensor_calls(inputs, *args):
        calls.append(len(inputs["body"]))
        return original(inputs, *args)
    monkeypatch.setattr(visibility, "sensor_features", count_sensor_calls)
    report, rows = visibility.analyze([accepted, goal_only, old_criterion, no_goal], external, successful_only=True)
    assert report["tally_all"]["episodes"] == 4 and report["tally_all"]["decisions"] == 12
    assert report["tally_all"]["counts"]["jump"] == 4
    assert report["tally_all"]["goal_success_episodes"] == 3
    assert report["tally_selected"]["episodes"] == 1 and report["decisions"] == 3
    assert report["counts"]["jump"] == 1 and calls == [3]
    assert {Path(row["episode"]).name for row in rows} == {"accepted"}
    assert report["selection"]["exclusion_reasons"] == dict(transition_not_successful=1, success_criterion_mismatch=1, goal_not_reached=1)


def drop_row(split, positive, near_distances):
    return dict(split=split, drop_onset=positive, ordinary_walk=not positive,
                drop_distance_cm=np.nan, drop_height_cm=np.nan,
                **{f"near_drop_distance_{token}_cm": distance
                   for (_, token), distance in zip(visibility.MEMORY_THRESHOLDS, near_distances)})


def test_drop_rule_fits_train_only_and_uses_walking_controls():
    training = [drop_row("train", True, (80, 80, 80)), drop_row("train", False, (80, 500, 500))]
    held_out = [drop_row("validation", True, (400, 400, 400))]*30
    rule = visibility.fit_drop_rule(training)
    assert visibility.fit_drop_rule(training + held_out) == rule
    assert rule["fit_split"] == "train" and rule["mode"] == "memory"
    assert rule["memory_min_probability"] == .5 and rule["distance_cm"] == 100
    assert rule["train_f1"] == 1
    np.testing.assert_array_equal(visibility.predict_drop_rule(training, rule), [True, False])
    assert not visibility.predict_drop_rule(held_out, rule).any()
    assert visibility.fit_drop_rule([drop_row("test", True, (80, 80, 80))]) is None


def test_memory_rule_does_not_pair_near_weak_cell_with_far_strong_cell():
    records, layout = fixture_records(1)
    memory = records["memory_near"]
    memory["channels"][0, 36, 32, 4] = 40
    memory["channels"][0, 48, 32, 4] = 220
    features = visibility.sensor_features(demos.learner_input(records), layout)
    assert features[0]["near_drop"] > .5
    assert features[0]["near_drop_distance_cm"] < 200
    assert features[0]["near_drop_distance_p50_cm"] > 400
    rule = dict(mode="memory", min_drop_cm=50, distance_cm=200, memory_min_probability=.5)
    assert not visibility.predict_drop_rule(features, rule)[0]


def test_native_drop_label_survives_missing_100ms_flags_and_never_enters_sensor_inputs(tmp_path):
    records, _ = fixture_records(12)
    source = write_episode(tmp_path / "drop", records=records)
    patch_metadata(source, type="drop", success=True, transition_success=True, success_criterion=visibility.DEMO_CRITERION,
                   transition_evidence=dict(native_dynamics=True, event_ms=50))
    frames = [dict(elapsed_ms=40, transition_id="place-link", type="drop", in_air=True, falling=True,
                   native_jump=False, native_climb=False)]
    frames.insert(0, dict(frames[0], elapsed_ms=0, in_air=False, falling=False))
    frames.append(dict(frames[-1], elapsed_ms=60, in_air=False, falling=False))
    (source / "transition-frames.jsonl").write_text("".join(json.dumps(frame) + "\n" for frame in frames))
    report, rows = visibility.analyze([source], successful_only=True)
    assert report["counts"]["drop_onset"] == 1
    assert report["counts"]["drop_native_onset"] == 1 and report["counts"]["drop_navdemo_onset"] == 0
    assert rows[0]["drop_onset_source"] == "native_frames"
    assert not rows[0]["ordinary_walk"] and rows[-1]["ordinary_walk"]
    assert report["tally_all"]["counts"]["drop_onset"] == 1
    assert report["by_split"]["train"]["groups"]["drop_onset"]["decisions"] == 1


def test_held_out_walking_false_positive_rate_is_reported_without_refitting(tmp_path):
    train = write_episode(tmp_path / "train", site="train-site")
    records, _ = fixture_records(3) # All three walking decisions have the same nearby obstacle.
    validation = write_episode(tmp_path / "val", split="validation", site="val-site", records=records)
    report, _ = visibility.analyze([train, validation])
    metric = report["by_split"]["validation"]["rule"]
    assert metric["controls"] == 3 and metric["walking_false_positive_rate"] == 1
    assert report["rule"]["fit_split"] == "train" and report["rule"]["train_f1"] == 1
    assert report["by_split"]["validation"]["groups"]["ordinary_walk"]["features"]["near_ledge"]["known"] == 3


def test_cli_success_filter_reports_all_tallies_even_when_no_episode_is_selected(tmp_path):
    source = write_episode(tmp_path / "episode")
    patch_metadata(source, success=True, transition_success=False, success_criterion=visibility.DEMO_CRITERION)
    output, decisions = tmp_path / "selected.json", tmp_path / "selected.csv"
    completed = subprocess.run([sys.executable, "-I", visibility.__file__, str(source), "--successful-only",
                                "--success-criterion", visibility.DEMO_CRITERION,
                                "--output", str(output), "--csv", str(decisions)], capture_output=True, text=True)
    assert completed.returncode == 0, completed.stderr
    report = json.loads(output.read_text())
    assert report["decisions"] == 0 and report["tally_all"]["decisions"] == 3
    assert report["tally_all"]["counts"]["jump"] == 1
    assert report["rule"] is None and report["drop_rule"] is None
    with decisions.open(newline="") as stream:
        assert list(csv.DictReader(stream)) == []


def write_native_frames(path, frames):
    (path / "transition-frames.jsonl").write_text("".join(json.dumps(frame) + "\n" for frame in frames))


def test_delayed_jump_air_flag_after_one_second_is_not_drop_or_walking_control(tmp_path):
    records, _ = fixture_records(22)
    records["keys"]["jump"][1] = 1 # Press at100ms, well before delayed inAir at1200ms.
    records["position_cm"][:, 1] = [100]*12 + [95, 70, 35, 30] + [30]*6
    records["body"]["flags"][12:16, :2] = 1
    source = write_episode(tmp_path / "delayed-jump", records=records)
    patch_metadata(source, type="jump")
    frames = [dict(elapsed_ms=moment, transition_id="place-link", type="jump",
                   in_air=200 <= moment < 1600, falling=1200 <= moment < 1600,
                   native_jump=100 <= moment < 1600, native_climb=False)
              for moment in range(0, 2200, 100)]
    write_native_frames(source, frames)
    report, rows = visibility.analyze([source])
    assert report["counts"]["drop_navdemo_onset"] == 1 # Raw diagnostic remains explicit.
    assert report["counts"]["drop_navdemo_rejected_native_action"] == 1
    assert report["counts"]["drop_onset"] == 0
    assert rows[0]["ordinary_walk"] # Normal approach before pressing jump is a control.
    assert rows[11]["native_motion"] and not rows[11]["ordinary_walk"]
    assert not rows[11]["jump"] and not records["body"]["flags"][11].any()


def test_native_drop_uses_run_containing_event_and_deduplicates_late_diagnostic(tmp_path):
    records, _ = fixture_records(25)
    records["position_cm"][:, 1] = [200]*14 + [195, 170, 135, 130, 125] + [125]*6
    records["body"]["flags"][14:19, :2] = 1
    source = write_episode(tmp_path / "drop", records=records)
    patch_metadata(source, type="drop", transition_evidence=dict(native_dynamics=True, event_ms=1500))
    # A tiny prep air phase must not attract the authoritative label. The real
    # fall starts at500ms and lasts to1900ms; NAVDEMO first sees air at1400ms.
    frames = [dict(elapsed_ms=moment, transition_id="place-link", type="drop",
                   in_air=moment == 100 or 500 <= moment < 1900,
                   falling=500 <= moment < 1900, native_jump=False, native_climb=False)
              for moment in range(0, 2500, 100)]
    write_native_frames(source, frames)
    report, rows = visibility.analyze([source])
    assert report["counts"]["drop_native_onset"] == 1
    assert report["counts"]["drop_navdemo_onset"] == 1
    assert report["counts"]["drop_navdemo_matched_native"] == 1
    assert report["counts"]["drop_onset"] == 1
    assert rows[4]["drop_onset"] and rows[4]["drop_onset_source"] == "native_frames"
    assert not rows[0]["drop_onset"] # Prep air at100ms is not the witnessed event.
    assert rows[13]["drop_navdemo_onset"] and not rows[13]["drop_onset"] # 900ms displacement, same fall-run.


def test_event_at1500_maps_to_second_physical_run_not_initial_prep_air(tmp_path):
    records, _ = fixture_records(24)
    records["position_cm"][:, 1] = [200]*14 + [195, 170, 135, 130] + [130]*6
    records["body"]["flags"][14:18, :2] = 1
    source = write_episode(tmp_path / "drop", records=records)
    patch_metadata(source, type="drop", transition_evidence=dict(native_dynamics=True, event_ms=1500))
    frames = [dict(elapsed_ms=moment, transition_id="place-link", type="drop",
                   in_air=moment == 100 or 1300 <= moment < 1800,
                   falling=1300 <= moment < 1800, native_jump=False, native_climb=False)
              for moment in range(0, 2400, 100)]
    write_native_frames(source, frames)
    report, rows = visibility.analyze([source])
    assert report["counts"]["drop_onset"] == 1
    assert rows[12]["drop_onset"] and not rows[0]["drop_onset"]
    assert report["counts"]["drop_navdemo_matched_native"] == 1


def test_distinct_uncommanded_physical_falls_are_not_merged_by_episode(tmp_path):
    records, _ = fixture_records(30)
    records["position_cm"][:, 1] = [200]*3 + [195, 170, 130] + [130]*15 + [125, 100, 60] + [60]*6
    records["body"]["flags"][3:6, :2] = 1
    records["body"]["flags"][21:24, :2] = 1
    source = write_episode(tmp_path / "two-falls", records=records)
    frames = [dict(elapsed_ms=moment, transition_id="place-link", type="drop",
                   in_air=300 <= moment < 600 or 2100 <= moment < 2400,
                   falling=300 <= moment < 600 or 2100 <= moment < 2400,
                   native_jump=False, native_climb=False) for moment in range(0, 3000, 100)]
    write_native_frames(source, frames)
    report, rows = visibility.analyze([source])
    assert report["counts"]["drop_onset"] == 2
    assert rows[2]["drop_onset"] and rows[20]["drop_onset"]


def test_witnessed_drop_run_is_authoritative_after_an_unrelated_prior_jump(tmp_path):
    records, _ = fixture_records(12)
    records["keys"]["jump"][0] = 1
    source = write_episode(tmp_path / "drop-after-jump", records=records)
    patch_metadata(source, type="drop", transition_evidence=dict(native_dynamics=True, event_ms=650))
    frames = [dict(elapsed_ms=moment, transition_id="place-link", type="drop",
                   in_air=moment == 100 or 600 <= moment < 900,
                   falling=600 <= moment < 900, native_jump=moment == 100, native_climb=False)
              for moment in [0, 100, 200, 300, 400, 500, 600, 650, 700, 800, 900, 1000, 1100]]
    write_native_frames(source, frames)
    report, rows = visibility.analyze([source])
    assert report["counts"]["drop_native_onset"] == 1
    assert report["counts"]["drop_onset"] == 1 and rows[5]["drop_onset"]


def test_stationary_direction_is_body_forward_not_the_carrot():
    records, layout = fixture_records(1)
    records["body"]["velocity_cm_s"] = 0
    records["roads"]["carrot_direction"] = [1, 0, 0]
    row = visibility.sensor_features(demos.learner_input(records), layout)[0]
    assert row["movement_source"] == "body_forward_stationary"
    assert row["movement_deg"] == pytest.approx(0)
    assert row["carrot_angle_deg"] == pytest.approx(90)
    assert row["carrot_movement_angle_deg"] == pytest.approx(90)
    assert row["obstacle_ray"] == 3
    assert row["obstacle_distance_cm"] == pytest.approx(100)
