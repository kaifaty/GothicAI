"""Training stop rules and split isolation, without a model or game process."""
import importlib.util
import json
from pathlib import Path
import signal
import sys
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location("nav_ppo_loop", Path(__file__).resolve().parents[1] / "nav_ppo_loop.py")
loop = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(loop)


def metrics(arrived, turn=.2):
    return dict(all=dict(n=60, arrived=arrived, arrival_fraction=arrived / 60,
                         action_fraction=dict(turn_only=turn), blocked_ms=100,
                         median_arrival_s=30))


def state(arrived=33, decisions=0):
    return dict(baseline_arrival_fraction=arrived / 60, best_arrival_fraction=arrived / 60,
                selected_arrival_fraction=arrived / 60,
                best_score=list(loop.validation_score(metrics(arrived))), best_iteration=0,
                best_policy="round2.navbc", best_checkpoint=None, decisions=decisions,
                bad_validations=0, stop_reason=None)


def test_stop_at_30m_requires_five_percentage_points_and_boundary_is_inclusive():
    updated, _ = loop.record_validation(state(decisions=30_000_000), metrics(35), 100, "new.navbc", "new.pt")
    assert updated["stop_reason"] == "no_5pp_validation_improvement_at_30m_decisions"
    updated, _ = loop.record_validation(state(decisions=30_000_000), metrics(36), 100, "new.navbc", "new.pt")
    assert updated["stop_reason"] is None
    # The rule concerns best validation, even when the latest score has noise.
    updated["decisions"] += 300_000
    updated, _ = loop.record_validation(updated, metrics(34), 105, "worse.navbc", "worse.pt")
    assert updated["stop_reason"] is None


def test_collapse_requires_two_consecutive_drops_strictly_over_15pp():
    current = state(arrived=48)
    current, _ = loop.record_validation(current, metrics(39), 5, "a", "a.pt")
    assert current["bad_validations"] == 0  # exactly 15 points
    current, _ = loop.record_validation(current, metrics(38), 10, "b", "b.pt")
    assert current["bad_validations"] == 1
    current, _ = loop.record_validation(current, metrics(48), 15, "c", "c.pt")
    assert current["bad_validations"] == 0
    current, _ = loop.record_validation(current, metrics(38), 20, "d", "d.pt")
    current, _ = loop.record_validation(current, metrics(37), 25, "e", "e.pt")
    assert current["stop_reason"] == "validation_collapse_two_consecutive_drops_gt_15pp"
    assert current["best_policy"] == "round2.navbc"


def test_arrival_selects_checkpoint_before_tie_breakers():
    current, improved = loop.record_validation(state(), metrics(34, turn=.4), 5, "a", "a.pt")
    assert improved and current["best_policy"] == "a"
    current, improved = loop.record_validation(current, metrics(34, turn=.1), 10, "b", "b.pt")
    assert improved and current["best_policy"] == "b"
    current, improved = loop.record_validation(current, metrics(33, turn=0), 15, "c", "c.pt")
    assert not improved and current["best_policy"] == "b"


def test_both_validation_goals_outrank_higher_arrival_with_excessive_turning():
    current = state(arrived=53)
    current["best_score"] = list(loop.validation_score(metrics(53, turn=.25)))
    current, improved = loop.record_validation(current, metrics(50, turn=.1), 5, "feasible", "feasible.pt")
    assert improved and current["best_policy"] == "feasible"
    assert current["best_arrival_fraction"] == 53 / 60
    assert current["selected_arrival_fraction"] == 50 / 60
    current, improved = loop.record_validation(current, metrics(51, turn=.1), 10, "more-arrivals", "more-arrivals.pt")
    assert improved and current["best_policy"] == "more-arrivals"
    assert current["best_arrival_fraction"] == 53 / 60
    assert current["selected_arrival_fraction"] == 51 / 60
    current, improved = loop.record_validation(current, metrics(55, turn=.16), 15, "turns-too-much", "other.pt")
    assert not improved and current["best_policy"] == "more-arrivals"
    assert current["best_arrival_fraction"] == 55 / 60
    assert current["selected_arrival_fraction"] == 51 / 60


def test_success_stop_needs_two_strong_validations_after_curriculum():
    current = state(decisions=4_999_999)
    current, _ = loop.record_validation(current, metrics(50, turn=.12), 5, "a", "a.pt")
    assert current["target_validations"] == 0
    current["decisions"] = 5_000_000
    current, _ = loop.record_validation(current, metrics(50, turn=.12), 10, "b", "b.pt")
    assert current["target_validations"] == 1 and current["stop_reason"] is None
    current, _ = loop.record_validation(current, metrics(51, turn=.13), 15, "c", "c.pt")
    assert current["target_validations"] == 0
    current, _ = loop.record_validation(current, metrics(50, turn=.1), 20, "d", "d.pt")
    current, _ = loop.record_validation(current, metrics(51, turn=.1), 25, "e", "e.pt")
    assert current["stop_reason"] == "validation_target_sustained"


def test_conservative_collection_bound_includes_resets_and_stays_below_ceiling():
    bound = loop.collection_decision_bound(40_000, 8, 100)
    assert bound == 1_291_200
    assert bound > 8 * 100 * 400
    assert 148_000_000 + bound <= 150_000_000
    assert 149_000_000 + bound > 150_000_000


def test_pilot_rejects_head_collapse_even_when_total_kl_is_small():
    base = {head: .5 for head in loop.HEADS}
    low = dict(coefficient=.03, stats=dict(kl_reference=.001, entropy_by_head=dict(base, forward=.1),
                                         reference_entropy_by_head=base))
    good = dict(coefficient=.1, stats=dict(kl_reference=.02, entropy_by_head=base, reference_entropy_by_head=base))
    high = dict(coefficient=.3, stats=dict(kl_reference=.005, entropy_by_head=base, reference_entropy_by_head=base))
    assert loop.select_pilot([low, high, good]) is good
    with pytest.raises(ValueError, match="no candidate"):
        loop.select_pilot([low])
    values = [loop.kl_coefficient(.1, step, 150_000_000) for step in (0, 30_000_000, 150_000_000)]
    assert values[0] == pytest.approx(.1)
    assert values[-1] == pytest.approx(.003)
    assert values[0] > values[1] > values[2]


def test_pilot_accepts_unchanged_actor_when_current_batch_entropy_is_lower_than_iteration_one():
    historical = {head: .5 for head in loop.HEADS}
    current_batch = {head: .05 for head in loop.HEADS}
    assert all(current_batch[head] < .75 * historical[head] for head in loop.HEADS)
    unchanged = dict(coefficient=.1, stats=dict(kl_reference=0., entropy_by_head=current_batch,
                                              reference_entropy_by_head=current_batch))
    assert loop.select_pilot([unchanged]) is unchanged
    collapsed = dict(coefficient=.1, stats=dict(kl_reference=0., entropy_by_head=dict(current_batch, turn=.02),
                                              reference_entropy_by_head=current_batch))
    with pytest.raises(ValueError, match="no candidate"):
        loop.select_pilot([collapsed])


def test_pilot_requires_current_batch_reference_entropy_metric():
    old_stats = dict(coefficient=.1, stats=dict(kl_reference=0., entropy_by_head={head: .5 for head in loop.HEADS}))
    with pytest.raises(KeyError, match="reference_entropy_by_head"):
        loop.select_pilot([old_stats])


def training_args(**changes):
    return SimpleNamespace(**(dict(lr=1e-4, batch=32, target_kl=.1, kl_start=.083,
        kl_floor=.01, kl_decay_decisions=10_000_000, max_decisions=150_000_000,
        pilot_kl=[.1, 1., 10.]) | changes))


def test_parameter_change_keeps_budget_best_checkpoint_and_absolute_kl_deadline():
    current = dict(state(arrived=41, decisions=8_643_602), iteration=27, initial_kl=.1,
                   current_policy="iteration27.navbc", current_checkpoint="iteration27.pt",
                   best_iteration=15, best_policy="iteration15.navbc", best_checkpoint="iteration15.pt")
    updated = loop.configure_training(current, training_args())
    for name in ("iteration", "decisions", "best_iteration", "best_policy", "best_checkpoint",
                 "current_policy", "current_checkpoint", "bad_validations", "baseline_arrival_fraction"):
        assert updated[name] == current[name]
    change = updated["parameter_changes"][0]
    assert change["after_iteration"] == 27 and change["effective_from_iteration"] == 28
    assert change["previous"]["actor_lr"] == 3e-5
    assert change["current"]["actor_lr"] == 1e-4
    assert change["current"]["batch_windows"] == 32
    assert change["current"]["target_kl"] == .1
    assert loop.training_coefficient(updated) == pytest.approx(.083)
    halfway = dict(updated, decisions=13_643_602)
    assert loop.training_coefficient(halfway) == pytest.approx((.083 * .01) ** .5)
    final = dict(updated, decisions=18_643_602)
    assert loop.training_coefficient(final) == pytest.approx(.01)
    assert loop.training_coefficient(dict(final, decisions=30_000_000)) == pytest.approx(.01)
    # Restarting after progress neither adds another change nor rebases the decay.
    resumed = loop.configure_training(halfway, training_args())
    assert resumed == halfway
    assert len(resumed["parameter_changes"]) == 1
    assert resumed["kl_schedule"]["start_decisions"] == 8_643_602


def test_interrupt_unwinds_active_wait_before_reaping(tmp_path, monkeypatch):
    args = SimpleNamespace(work=tmp_path, threads=1, workers=8, python=Path(sys.executable))
    driver = loop.Loop(args)
    calls = []
    class Process:
        returncode = None
        waiting = False
        def poll(self):
            return self.returncode
        def send_signal(self, value):
            calls.append(("signal", value))
        def wait(self):
            assert not self.waiting, "reentrant Popen.wait would deadlock its waitpid lock"
            calls.append("wait")
            if calls == ["wait"]:
                self.waiting = True
                try:
                    driver.interrupted(signal.SIGTERM, None)
                finally:
                    self.waiting = False
            self.returncode = 143
            return 143
    process = Process()
    def popen(command, **kwargs):
        separator = command.index("--")
        assert command[separator + 1:separator + 8] == [sys.executable, "-I",
            str(driver.script / "nav_job.py"), "--max-seconds", "7200", "--", sys.executable]
        assert command[separator + 8:separator + 10] == ["-X", "faulthandler"]
        return process
    monkeypatch.setattr(loop.subprocess, "Popen", popen)
    with pytest.raises(SystemExit) as result:
        driver.queued("test", [sys.executable, "never-executed"], tmp_path / "launcher.log")
    assert result.value.code == 143
    assert calls == ["wait", ("signal", signal.SIGTERM), "wait"]
    assert driver.process is None


def test_update_passes_actor_lr_and_window_batch_to_optimizer(tmp_path, monkeypatch):
    args = SimpleNamespace(work=tmp_path, threads=4, workers=8, python=Path("/venv/python"),
                           reference=Path("/round2.navbc"), lr=1e-4, batch=32)
    driver = loop.Loop(args)
    destination = tmp_path / "update"
    seen = []
    def queued(name, command, log, **resources):
        seen.extend(map(str, command))
        destination.mkdir()
        for name in ("last.pt", "policy.navbc"):
            (destination / name).touch()
        loop.write_json(destination / "stats.json", dict(peak_cuda_gb=.2))
    monkeypatch.setattr(driver, "queued", queued)
    driver.update(tmp_path / "collect", destination, Path("/iteration27.pt"), 28, .083, False)
    assert seen[seen.index("--lr") + 1] == "0.0001"
    assert seen[seen.index("--batch") + 1] == "32"
    assert seen[seen.index("--resume") + 1] == "/iteration27.pt"


def test_training_validation_reverse_overlap_is_rejected():
    training = [dict(region=region, start=f"{region}-a{i}", goal=f"{region}-b{i}",
                     detour_complete=True, detour_length_cm=5000)
                for region in sorted(loop.REGIONS) for i in range(200)]
    validation = [dict(region=region, start=f"{region}-val-a{i}", goal=f"{region}-val-b{i}",
                       final_test=True, teacher_success=True, teacher_version=2)
                  for region in sorted(loop.REGIONS) for i in range(10)]
    assert len(loop.validate_sets(training, validation)) == 1200
    validation[0].update(start=training[0]["goal"], goal=training[0]["start"])
    with pytest.raises(ValueError, match="overlaps"):
        loop.validate_sets(training, validation)


def test_shuffling_keeps_every_region_in_each_initial_block_and_changes_each_iteration():
    source = [dict(region=region, start=f"{region}-{i}") for region in sorted(loop.REGIONS) for i in range(200)]
    one = loop.balanced_routes(source, 1)
    assert one == loop.balanced_routes(source, 1)
    assert one != loop.balanced_routes(source, 2)
    assert len(one) == len(source)
    for offset in range(0, len(one), 6):
        assert {pair["region"] for pair in one[offset:offset + 6]} == loop.REGIONS


def test_raw_cleanup_keeps_episode_outcomes_and_checkpoint_is_not_in_collect(tmp_path):
    collect = tmp_path / "collect"
    episode = collect / "population/n100-r8-w0/episodes/00000"
    episode.mkdir(parents=True)
    for name in ("episode.json", "samples.jsonl", "samples.bin.gz", "bootstrap.navdemo", "actions.txt", "trajectory.txt"):
        (episode / name).write_text("{}")
    (collect / "population/n100-r8-w0/episodes.jsonl").write_text("{}\n")
    checkpoint = tmp_path / "update/last.pt"
    checkpoint.parent.mkdir()
    checkpoint.write_text("saved")
    loop.remove_raw(collect)
    assert sorted(path.name for path in collect.rglob("*") if path.is_file()) == ["episode.json", "episodes.jsonl"]
    assert checkpoint.read_text() == "saved"


def test_atomic_state_is_valid_json_without_nonfinite_values(tmp_path):
    path = tmp_path / "state.json"
    loop.write_json(path, dict(iteration=2, decisions=123))
    assert json.loads(path.read_text()) == dict(iteration=2, decisions=123)
    with pytest.raises(ValueError):
        loop.write_json(path, dict(decisions=float("nan")))
    assert json.loads(path.read_text()) == dict(iteration=2, decisions=123)


def test_validation_retains_curves_outcomes_actions_and_trajectories(tmp_path):
    for name in ("metrics.json", "episode.json", "actions.txt", "trajectory.txt", "samples.bin.gz", "samples.jsonl", "bootstrap.bin.gz"):
        (tmp_path / name).write_text("saved")
    loop.trim_validation(tmp_path)
    assert {path.name for path in tmp_path.iterdir()} == {"metrics.json", "episode.json", "actions.txt", "trajectory.txt"}


def test_cli_preserves_venv_python_symlink_while_resolving_data_paths(tmp_path, monkeypatch):
    base_python = tmp_path / "base-python"
    base_python.write_text("#!/usr/bin/env python3\n")
    venv_python = tmp_path / "venv/bin/python"
    venv_python.parent.mkdir(parents=True)
    venv_python.symlink_to(base_python)
    input_directory = tmp_path / "inputs"
    input_directory.mkdir()
    command = ["nav_ppo_loop.py", "--python", str(venv_python)]
    for name in ("runner", "assets", "save", "cache", "links", "roads", "reference", "training", "validation"):
        path = tmp_path / name
        path.write_text("{}\n")
        command += ["--" + name, str(input_directory / ".." / name)]
    captured = {}
    monkeypatch.setattr(loop.sys, "argv", command)
    monkeypatch.setattr(loop.sys, "platform", "linux")
    monkeypatch.setattr(loop.Loop, "run", lambda self: captured.update(args=self.args))
    loop.main()
    assert captured["args"].python == venv_python
    assert captured["args"].python.is_symlink()
    assert captured["args"].python != base_python.resolve()
    assert captured["args"].training == (tmp_path / "training").resolve()


@pytest.mark.parametrize("episodes,rejected,exceeded", [(1000, 0, False), (1000, 1, False),
                                                       (1001, 1, False), (999, 1, True), (1000, 2, True)])
def test_nonfinite_rate_uses_all_episodes_and_strict_point_one_percent(episodes, rejected, exceeded):
    health = loop.collection_health(dict(episodes=episodes, decisions=10000,
                                         rejected_nonfinite_episodes=rejected, rejected_nonfinite_decisions=rejected * 10))
    assert health["episodes"] == episodes
    assert health["accepted_episodes"] == episodes - rejected
    assert health["rejected_nonfinite_fraction"] == rejected / episodes
    assert health["limit_exceeded"] is exceeded


def test_rejected_episode_excludes_its_prefix_without_discarding_other_worker():
    result = dict(decisions=100_000, workers=[dict(episodes=500, decisions=50_000,
                  rejected_nonfinite_episodes=1, rejected_nonfinite_decisions=7),
                  dict(episodes=500, decisions=50_000)])
    health = loop.collection_health(result)
    assert not health["limit_exceeded"]
    # All 50,000 decisions in the unaffected worker remain in the update.
    stats = dict(decisions=99_993, collected_decisions=100_000, collected_episodes=1000,
                 rejected_nonfinite_episodes=1, rejected_nonfinite_decisions=7,
                 rejected_episode_details=[dict(termination="nonfinite_observation", directory="w0/ep7")])
    updated = loop.updated_collection_health(result, stats)
    assert updated["accepted_decisions"] == 49_993 + 50_000
    assert updated["rejected_nonfinite_diagnostics"] == stats["rejected_episode_details"]
    with pytest.raises(ValueError, match="accepted/rejected counts"):
        loop.updated_collection_health(result, dict(stats, decisions=49_993))


def test_zero_action_nonfinite_episode_counts_towards_threshold():
    health = loop.collection_health(dict(episodes=1, decisions=0, rejected_nonfinite_episodes=1,
                                         rejected_nonfinite_decisions=0))
    assert health["limit_exceeded"]
    assert health["rejected_nonfinite_fraction"] == 1
    assert health["accepted_decisions"] == 0


def test_updater_discovered_nonfinite_rejections_use_same_episode_threshold():
    result = dict(episodes=1000, decisions=100_000)
    stats = dict(collected_episodes=1000, collected_decisions=100_000, decisions=99_980,
                 rejected_nonfinite_episodes=2, rejected_nonfinite_decisions=20)
    health = loop.updated_collection_health(result, stats)
    assert health["limit_exceeded"]
    assert health["accepted_episodes"] == 998
    assert health["accepted_decisions"] == 99_980


def test_nonfinite_stop_is_durable_counts_all_decisions_and_preserves_clean_recordings(tmp_path, monkeypatch):
    collector = tmp_path / "iter-000004/collect"
    clean = collector / "population/w1/episodes/clean/samples.jsonl"
    clean.parent.mkdir(parents=True)
    clean.write_text("clean worker's data\n")
    health = loop.collection_health(dict(episodes=1000, decisions=100_000, rejected_nonfinite_episodes=2,
                                         rejected_nonfinite_decisions=20,
                                         rejected_nonfinite_diagnostics=[dict(directory="w0/bad")]))
    driver = loop.Loop(SimpleNamespace(work=tmp_path, threads=1, critic_warmup=3))
    previous = dict(state(decisions=900), iteration=3, current_policy="safe.navbc", current_checkpoint="safe.pt")
    loop.write_json(driver.state_path, previous)
    restored = []
    def fail_collection():
        raise loop.RejectedCollectionError(collector, 4, health)
    monkeypatch.setattr(driver, "_run", fail_collection)
    monkeypatch.setattr(driver, "restore_best", lambda value: restored.append(value))
    driver.run()
    stopped = loop.read_json(driver.state_path)
    assert stopped["stop_reason"] == "nonfinite_observation_rate_gt_0_1_percent"
    assert stopped["iteration"] == 4 and stopped["decisions"] == 100_900
    assert stopped["current_policy"] == "safe.navbc" and stopped["current_checkpoint"] == "safe.pt"
    assert stopped["collection_health"]["rejected_nonfinite_fraction"] == .002
    assert clean.read_text() == "clean worker's data\n"
    assert restored and restored[0]["stop_reason"] == stopped["stop_reason"]
    row = loop.read_json(collector.parent / "iteration.json")
    assert row["training"] is None and row["collection_health"]["rejected_nonfinite_episodes"] == 2
    # Re-entering the same failed phase cannot charge its decisions twice.
    driver.run()
    assert loop.read_json(driver.state_path)["decisions"] == 100_900


def test_existing_null_pilot_selection_stops_durably_without_losing_or_double_counting_budget(tmp_path, monkeypatch):
    collector = tmp_path / "iter-000004/collect"
    collector.mkdir(parents=True)
    selection_file = collector.parent / "pilot-selection.json"
    diagnosis = dict(selected=None, candidates=[dict(coefficient=100, stats=dict(kl_reference=.5))])
    loop.write_json(selection_file, diagnosis)
    health = loop.collection_health(dict(episodes=1500, decisions=320_000))
    driver = loop.Loop(SimpleNamespace(work=tmp_path, threads=1, critic_warmup=3))
    previous = dict(state(decisions=950_000), iteration=3,
                    current_policy="safe.navbc", current_checkpoint="safe.pt")
    loop.write_json(driver.state_path, previous)
    restored = []
    monkeypatch.setattr(driver, "_run", lambda: loop.load_pilot_selection(selection_file, collector, 4, health))
    monkeypatch.setattr(driver, "restore_best", lambda value: restored.append(value))
    driver.run()
    stopped = loop.read_json(driver.state_path)
    assert stopped["stop_reason"] == "kl_pilot_no_viable_candidate"
    assert stopped["iteration"] == 4 and stopped["decisions"] == 1_270_000
    assert stopped["current_policy"] == "safe.navbc" and stopped["current_checkpoint"] == "safe.pt"
    assert stopped["best_policy"] == previous["best_policy"]
    assert stopped["pilot_diagnosis"] == dict(iteration=4, **diagnosis)
    row = loop.read_json(collector.parent / "iteration.json")
    assert row["training"] is None and row["iteration_decisions"] == 320_000
    assert row["pilot_diagnosis"] == stopped["pilot_diagnosis"]
    assert restored and (tmp_path / "curves.jsonl").is_file()
    # Even an old explicit reset followed by the same saved null selection is
    # safe: it stops again, with no None indexing and no duplicate env charge.
    stopped["stop_reason"] = None
    loop.write_json(driver.state_path, stopped)
    driver.run()
    resumed = loop.read_json(driver.state_path)
    assert resumed["stop_reason"] == "kl_pilot_no_viable_candidate"
    assert resumed["decisions"] == 1_270_000 and resumed["iteration"] == 4
