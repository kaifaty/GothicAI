"""Recurrent PPO math, episode boundaries, bootstrap and actor compatibility.

Model tests run on Ryzen under gq; never launch them on the development Mac.
"""
from __future__ import annotations

import gc
import gzip
import json
from pathlib import Path
import random
import sys
from types import SimpleNamespace
import weakref

import numpy as np
import pytest
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import nav_ppo as ppo


@pytest.fixture(autouse=True)
def single_thread():
    torch.set_num_threads(1)


def test_cuda_allocator_device_is_explicit_without_initializing_a_gpu(monkeypatch):
    calls = []
    monkeypatch.setattr(torch.cuda, "current_device", lambda: calls.append(True) or 2)
    assert ppo.resolve_device("cuda") == "cuda:2"
    assert calls == [True]
    calls.clear()
    assert ppo.resolve_device("cuda:3") == "cuda:3"
    assert ppo.resolve_device("cpu") == "cpu"
    assert calls == []


def sample(action=(2, 1, 1, 1, 1, 0), valid=True):
    return dict(path_valid=valid, action_indices=list(action),
                rewards=dict(progress_cm=125, arrived=1, blocked=1, fall_damage=2,
                             death=1, jump=action[4], climb=action[5], time_ms=100))


def test_reward_weights_units_press_edges_and_invalid_intervals():
    components = ppo.reward_components(sample())
    assert components == pytest.approx(dict(progress_cm=1.25, arrived=10, blocked=-.05,
             fall_damage=-.2, death=-10, jump=-.1, climb=0, time=-.01))
    held = ppo.reward_components(sample(), previous_action=[2, 1, 1, 1, 1, 0])
    assert held["jump"] == 0
    assert not any(ppo.reward_components(sample(valid=False)).values())
    shorter = sample()
    shorter["rewards"]["time_ms"] = 20
    assert ppo.reward_components(shorter)["time"] == -.01


def test_gae_bootstraps_truncation_and_resets_terminal_and_invalid_intervals():
    advantage, returns = ppo.generalized_advantage([1., 2.], [3., 4.], [True, True],
                                                   [False, False], bootstrap=10., gamma=.5, lam=1.)
    np.testing.assert_allclose(advantage, [1.5, 3.])
    np.testing.assert_allclose(returns, [4.5, 7.])
    _, terminal = ppo.generalized_advantage([1., 2.], [3., 4.], [True, True],
                                           [False, True], bootstrap=1000., gamma=.5, lam=1.)
    np.testing.assert_allclose(terminal, [2., 2.])
    _, masked = ppo.generalized_advantage([1., 100000., 2.], [0., 500., 0.],
                      [True, False, True], [False, False, True], bootstrap=0., gamma=.5, lam=1.)
    np.testing.assert_allclose(masked, [251., 0., 2.])
    # Invalid marks an interval, not its entry state: t=0 ends at valid V(s1)=4.
    _, boundary = ppo.generalized_advantage([1., 100000.], [3., 4.],
                      [True, False], [False, False], bootstrap=1000., gamma=.5, lam=1.)
    np.testing.assert_allclose(boundary, [3., 0.])


def test_independent_heads_exact_kl_entropy_and_clipped_surrogate():
    logits = {name: torch.zeros(1, 2, classes) for name, classes in zip(ppo.HEADS, ppo.CLASSES)}
    actions = torch.zeros(1, 2, 6, dtype=torch.int64)
    reference = torch.zeros(1, 2, sum(ppo.CLASSES))
    logp, entropy, kl = ppo.policy_terms(logits, actions, reference)
    torch.testing.assert_close(logp, torch.full((1, 2), -sum(np.log(ppo.CLASSES))))
    assert torch.count_nonzero(kl) == 0
    for name, classes in zip(ppo.HEADS, ppo.CLASSES):
        torch.testing.assert_close(entropy[name], torch.full((1, 2), float(np.log(classes))))
    loss, ratio = ppo.clipped_policy_loss(torch.log(torch.tensor([2., .5])), torch.zeros(2), torch.tensor([1., -1.]))
    torch.testing.assert_close(ratio, torch.tensor([2., .5]))
    torch.testing.assert_close(loss, torch.tensor([-1.2, .8]))


def test_privileged_input_is_normalized_separately_from_actor():
    actual = ppo.privileged(dict(path_cm=1234., first_direction=[0., 0., 1.], blocked=True, in_air=False))
    np.testing.assert_allclose(actual, [1.234, 0., 0., 1., 1., 0.])
    with pytest.raises(ValueError, match="first_direction"):
        ppo.privileged(dict(path_cm=12., first_direction=[float("nan"), 0, 0], blocked=False, in_air=False))


def args(**kwargs):
    return SimpleNamespace(**(dict(segment=2, burn_in=1, device="cpu", replay_tolerance=1e-4,
           gamma=.995, gae_lambda=.95, batch=2, critic_only=False, value_coef=.5,
           max_grad_norm=.5, clip=.2, kl_coef=.1, entropy_coef=.003) | kwargs))


def make_episode(tmp_path, truncated=False, count=6):
    directory = tmp_path / "episode"
    directory.mkdir()
    records = np.zeros(count + 1, dtype=ppo.nav_demos.RECORD_V2_DTYPE)
    records["time_ms"] = np.arange(count + 1) * 100
    records["roads"]["carrot_direction"][:, 2] = 1
    records["roads"]["goal_direction"][:, 2] = 1
    records["roads"]["carrot_distance_cm"] = 500
    records["roads"]["goal_distance_cm"] = 1000
    records["body"]["velocity_cm_s"][:, 2] = np.arange(count + 1) * 5
    layout = np.zeros((ppo.nav_demos.RAY_COUNT, 3), np.float32)
    layout[:, 2] = 1000
    for name, data in (("samples.bin.gz", records[:count]), ("bootstrap.bin.gz", records[count:])):
        if name.startswith("bootstrap") and not truncated:
            continue
        with gzip.open(directory / name, "wb") as stream:
            stream.write(ppo.nav_demos.HEADER.pack(ppo.nav_demos.MAGIC, 2, ppo.nav_demos.RECORD_BYTES,
                            ppo.nav_demos.RAY_COUNT, ppo.nav_demos.NEAR_SIZE, ppo.nav_demos.FAR_SIZE, 100))
            stream.write(data.tobytes())
    (directory / "episode.json").write_text(json.dumps(dict(decisions=count, success=not truncated, eye_layout=layout.tolist())))
    torch.manual_seed(123)
    actor = ppo.NavPolicy(ppo.VECTOR_SIZE, hidden_size=16).eval()
    features = ppo.encode_features(ppo.nav_demos.learner_input(records[:count]), layout)
    with torch.no_grad():
        logits, _ = actor(**{key: torch.from_numpy(value[None]) for key, value in features.items()})
    rows = []
    for index in range(count):
        action = [2, 1, 1, 1, int(index < 2), 0]
        flat = [float(value) for head in ppo.HEADS for value in logits[head][0, index]]
        logp = ppo.categorical_log_probs(flat, np.array(action))
        critic = dict(path_cm=max(100, 1000-index*4), first_direction=[0, 0, 1], blocked=False, in_air=False, valid=True)
        row = sample(action)
        row.update(time_ms=index * 100, next_time_ms=(index + 1) * 100,
                   log_probs=logp.tolist(), log_prob=float(logp.sum()), logits=flat,
                   terminated=index == count-1 and not truncated, truncated=index == count-1 and truncated,
                   critic_before=critic, critic_after=critic)
        row["rewards"].update(arrived=int(index == count-1 and not truncated), death=0)
        rows.append(row)
    (directory / "samples.jsonl").write_text("".join(json.dumps(row)+"\n" for row in rows))
    reference = tmp_path / "round2.navbc"
    preprocessing = dict(ppo.PREPROCESSING, eye_layout=layout.tolist())
    ppo.export_weights(actor, reference, preprocessing)
    return directory, actor, reference


def test_replay_recovers_exact_hidden_at_burn_in_and_resets_each_episode(tmp_path):
    directory, actor, _ = make_episode(tmp_path)
    episode, bootstrap, summary = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    critic = ppo.NavCritic(actor).eval()
    error, logit_error = ppo.replay_episode(episode, bootstrap, actor, critic, actor, args())
    assert error < 1e-5 and logit_error < 1e-5
    assert summary["jump_presses"] == 1
    assert summary["climb_presses"] == 0
    windows = [1, 2]
    burn = ppo.batch_arrays(episode, windows, args(), burn=True)
    training = ppo.batch_arrays(episode, windows, args())
    initial = torch.from_numpy(episode["actor_hidden"][windows][None])
    with torch.no_grad():
        _, hidden = actor(**ppo.features_of(burn), hidden=initial)
        replayed, _ = actor(**ppo.features_of(training), hidden=hidden)
        whole, _ = actor(**{key: torch.from_numpy(episode[key][None]) for key in ("near", "far", "vector")})
    for row, window in enumerate(windows):
        start = episode["starts"][window]
        for head in ppo.HEADS:
            torch.testing.assert_close(replayed[head][row], whole[head][0, start:start+2], rtol=1e-5, atol=1e-6)
    # Calling replay again must start the new episode at zeros, independent of the old final state.
    again, bootstrap, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    ppo.replay_episode(again, bootstrap, actor, critic, actor, args())
    np.testing.assert_array_equal(again["actor_hidden"], episode["actor_hidden"])
    assert not episode["actor_hidden"][0].any()


def test_replay_rejects_wrong_collector_policy_before_optimization(tmp_path):
    directory, actor, _ = make_episode(tmp_path)
    episode, bootstrap, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    episode["recorded_logits"][3, 0] += .1
    with pytest.raises(ValueError, match="policy replay mismatch"):
        ppo.replay_episode(episode, bootstrap, actor, ppo.NavCritic(actor), actor, args())


def test_truncated_bootstrap_uses_final_observation_and_carried_critic_state(tmp_path):
    directory, actor, _ = make_episode(tmp_path, truncated=True)
    episode, bootstrap, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    critic = ppo.NavCritic(actor).eval()
    with torch.no_grad():
        critic.value.weight.fill_(.1)
        features = {key: torch.from_numpy(episode[key][None]) for key in ("near", "far", "vector")}
        values, hidden = critic(**features, priv=torch.from_numpy(episode["priv"][None]))
        final_features = {key: torch.from_numpy(value[None]) for key, value in bootstrap.items()}
        final, _ = critic(**final_features, hidden=hidden)
    ppo.replay_episode(episode, bootstrap, actor, critic, actor, args())
    expected = episode["rewards"][-1] + .995 * float(final[0, 0])
    assert episode["returns"][-1] == pytest.approx(expected, abs=1e-6)
    (directory / "bootstrap.bin.gz").unlink()
    with pytest.raises(ValueError, match="requires final actor observation"):
        ppo.read_episode(directory, ppo.REWARD_WEIGHTS)


def test_critic_warmup_changes_only_critic_and_preserves_portable_actor(tmp_path):
    directory, actor, reference = make_episode(tmp_path)
    episode, bootstrap, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    critic = ppo.NavCritic(actor).eval()
    options = args(critic_only=True)
    ppo.replay_episode(episode, bootstrap, actor, critic, actor, options)
    actor_before = {key: value.detach().clone() for key, value in actor.state_dict().items()}
    critic_before = critic.value.weight.detach().clone()
    actor_optimizer = torch.optim.Adam(actor.parameters(), lr=1e-3)
    critic_optimizer = torch.optim.Adam(critic.parameters(), lr=1e-3)
    measures, count = ppo.train_episode(episode, actor, critic, actor_optimizer, critic_optimizer,
                                       options, random.Random(3), 0., 1.)
    assert count == 6 and measures["value_loss"] > 0
    for key, value in actor.state_dict().items():
        torch.testing.assert_close(value, actor_before[key], rtol=0, atol=0)
    assert not torch.equal(critic.value.weight, critic_before)
    imported, preprocessing = ppo.load_actor(reference, "cpu")
    exported = tmp_path / "updated.navbc"
    ppo.export_weights(imported, exported, preprocessing)
    assert reference.read_bytes() == exported.read_bytes()


def test_actor_ppo_update_uses_sampled_actions_and_changes_actor(tmp_path):
    directory, actor, _ = make_episode(tmp_path)
    episode, bootstrap, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    critic = ppo.NavCritic(actor).eval()
    ppo.replay_episode(episode, bootstrap, actor, critic, actor, args())
    initial = actor.head_forward.weight.detach().clone()
    measures, count = ppo.train_episode(episode, actor, critic,
        torch.optim.Adam(actor.parameters(), lr=1e-3), torch.optim.Adam(critic.parameters(), lr=1e-3),
        args(), random.Random(5), 0., 1.)
    assert count == 6 and np.isfinite(measures["policy_loss"])
    assert not torch.equal(actor.head_forward.weight, initial)


def synthetic_training_episode(episode_id, count, options):
    """Tiny identifiable arrays test packing without needing an actor forward."""
    values = (episode_id * 1000 + np.arange(count)).astype(np.float32)
    episode = {name: values.copy() for name in
               ("old_logp", "advantage", "returns", "old_values")}
    episode.update(near=values[:, None, None, None].copy(),
                   far=(values + .5)[:, None, None, None],
                   vector=np.column_stack((np.full(count, episode_id), np.arange(count))).astype(np.float32),
                   priv=np.tile(values[:, None], (1, ppo.PRIVILEGED_SIZE)),
                   actions=np.tile(np.array([2, 1, 1, 1, 0, 0], np.int64), (count, 1)),
                   reference_logits=np.tile(values[:, None], (1, sum(ppo.CLASSES))),
                   valid=np.ones(count, bool), starts=np.arange(0, count, options.segment))
    episode["valid"][count // 2] = False
    starts = episode["starts"].astype(np.float32)
    episode["actor_hidden"] = np.column_stack((np.full(len(starts), episode_id), starts, starts + 10)).astype(np.float32)
    episode["critic_hidden"] = episode["actor_hidden"] + 100
    return episode


def test_cross_episode_batches_visit_each_window_once_and_preserve_own_hidden_burn_and_padding():
    options = args(batch=4)
    episodes = [synthetic_training_episode(index + 1, count, options)
                for index, count in enumerate((5, 7, 4, 3))]
    by_id = {index + 1: episode for index, episode in enumerate(episodes)}
    batches = list(ppo.iter_training_batches(iter(episodes), options, random.Random(17)))
    assert all(1 <= len(batch) <= options.batch for batch in batches)
    assert any(len(batch) == options.batch and len({int(row["batch"]["vector"][0, 0]) for row in batch}) > 1
               for batch in batches)
    seen = []
    padded = 0
    for rows in batches:
        assert len({row["burn_length"] for row in rows}) == 1
        batch, burn, actor_h, critic_h = ppo.training_batch_tensors(rows, options)
        assert actor_h.shape == critic_h.shape == (1, len(rows), 3)
        for row_index, row in enumerate(rows):
            episode_id, start = map(int, row["batch"]["vector"][0])
            seen.append((episode_id, start))
            original = by_id[episode_id]
            window = int(np.flatnonzero(original["starts"] == start)[0])
            real_length = min(options.segment, len(original["valid"]) - start)
            for name in ppo.BATCH_FIELDS:
                expected = original[name][start:start+real_length]
                np.testing.assert_array_equal(row["batch"][name][:real_length], expected)
                np.testing.assert_array_equal(row["batch"][name][real_length:],
                                              np.zeros_like(row["batch"][name][real_length:]))
                assert not np.shares_memory(row["batch"][name], original[name])
                np.testing.assert_array_equal(batch[name][row_index].numpy(), row["batch"][name])
            if real_length < options.segment:
                padded += 1
                assert not batch["valid"][row_index, real_length:].any()
            np.testing.assert_array_equal(actor_h[0, row_index].numpy(), original["actor_hidden"][window])
            np.testing.assert_array_equal(critic_h[0, row_index].numpy(), original["critic_hidden"][window])
            assert not np.shares_memory(row["actor_hidden"], original["actor_hidden"])
            assert not np.shares_memory(row["critic_hidden"], original["critic_hidden"])
            length = min(options.burn_in, start)
            assert row["burn_length"] == length
            if length:
                for name in ppo.BURN_FIELDS:
                    np.testing.assert_array_equal(burn[name][row_index].numpy(), original[name][start-length:start])
                    assert not np.shares_memory(row["burn"][name], original[name])
            else:
                assert burn is None
    expected = {(index + 1, int(start)) for index, episode in enumerate(episodes) for start in episode["starts"]}
    assert len(seen) == len(set(seen)) == len(expected)
    assert set(seen) == expected
    assert padded == 3


def test_cross_episode_queues_are_bounded_independently_of_episode_count():
    options = args(batch=32)
    buffer_stats = {}
    source = (synthetic_training_episode(index + 1, 5, options) for index in range(100))
    windows = 0
    for batch in ppo.iter_training_batches(source, options, random.Random(3), buffer_stats):
        assert len(batch) <= options.batch
        windows += len(batch)
    assert windows == 300
    assert buffer_stats["buffered_windows_per_group_max"] == options.batch
    # Equal-length queues hold fewer than one batch each until the ready queue
    # reaches its bound and is immediately yielded. There are two burn groups.
    assert options.batch <= buffer_stats["buffered_windows_max"] <= 2 * options.batch - 1


def test_cross_episode_queues_do_not_retain_consumed_episode_arrays():
    options = args(batch=2)
    references = {}
    def source():
        for index in range(1, 4):
            episode = synthetic_training_episode(index, 5, options)
            references[index] = weakref.ref(episode["near"])
            yield episode
            del episode
    batches = ppo.iter_training_batches(source(), options, random.Random(3))
    first = next(batches)  # The two burn-in windows from episode 1.
    second = next(batches)  # Initial windows from episodes 1 and 2.
    gc.collect()
    assert references[1]() is None
    assert references[2]() is not None  # The actively streamed episode only.
    assert len(first) == len(second) == options.batch
    assert {int(row["batch"]["vector"][0, 0]) for row in second} == {1, 2}
    batches.close()


def test_actor_optimizer_uses_full_32_window_cross_episode_batch(tmp_path):
    directories = [tmp_path / name for name in ("one", "two")]
    for directory in directories:
        directory.mkdir()
    episodes = []
    actor = None
    for directory in directories:
        recorded, actor, _ = make_episode(directory, count=2)
        episode, bootstrap, _ = ppo.read_episode(recorded, ppo.REWARD_WEIGHTS)
        episodes.append((episode, bootstrap))
    critic = ppo.NavCritic(actor).eval()
    options = args(batch=32)
    for episode, bootstrap in episodes:
        ppo.replay_episode(episode, bootstrap, actor, critic, actor, options)
    # Thirty-two independently copied episode windows share one real update.
    source = (episodes[index % len(episodes)][0] for index in range(options.batch))
    batches = ppo.iter_training_batches(source, options, random.Random(5))
    measures, count = ppo.train_batches(batches, actor, critic,
        torch.optim.Adam(actor.parameters(), lr=1e-3), torch.optim.Adam(critic.parameters(), lr=1e-3),
        options, 0., 1.)
    assert count == 64
    assert measures["optimizer_steps"] == 1
    assert measures["batch_windows_max"] == measures["batch_windows_sum"] == 32
    assert measures["batch_windows_sum"] / measures["optimizer_steps"] == 32
    assert np.isfinite(measures["policy_loss"])


def rejected_metadata(count=0, **extra):
    return dict(decisions=count, training_valid=False, termination="nonfinite_observation",
                nonfinite_observation=dict(stage="decision", diagnostic="head yaw is nan",
                                           fields=[dict(name="body.head_yaw_deg", value="nan")]), **extra)


@pytest.mark.parametrize("marker", ["training_valid", "termination"])
def test_whole_marked_episode_is_skipped_before_payload_including_initial_failure(tmp_path, monkeypatch, marker):
    clean, actor, _ = make_episode(tmp_path)
    bad = tmp_path / "bad"
    bad.mkdir()
    metadata = rejected_metadata(13)
    del metadata["termination" if marker == "training_valid" else "training_valid"]
    (bad / "episode.json").write_text(json.dumps(metadata))
    (bad / "samples.bin.gz").write_bytes(b"deliberately unreadable rejected prefix")
    (bad / "samples.jsonl").write_text("deliberately unreadable rejected transitions")
    initial = tmp_path / "bad-initial"
    initial.mkdir()
    (initial / "episode.json").write_text(json.dumps(rejected_metadata()))
    # Test filtering independently of the separately tested production rate limit.
    monkeypatch.setattr(ppo, "MAX_REJECTED_FRACTION", 1.)
    cache = tmp_path / "cache"
    cache.mkdir()
    state_before = {key: value.detach().clone() for key, value in actor.state_dict().items()}
    preparation = ppo.prepare_rollouts(args(source=tmp_path, output=tmp_path), actor,
                                       ppo.NavCritic(actor).eval(), actor, cache)
    rejection = preparation["rejection"]
    assert rejection["collected_episodes"] == 3
    assert rejection["collected_decisions"] == 19
    assert rejection["rejected_nonfinite_episodes"] == 2
    assert rejection["rejected_nonfinite_decisions"] == 13
    assert preparation["total"]["decisions"] == 6
    assert len(preparation["prepared"]) == 1
    assert len(list(cache.iterdir())) == 1
    assert rejection["rejected_episode_details"][0]["detail"]["fields"][0]["value"] == "nan"
    for name, value in actor.state_dict().items():
        torch.testing.assert_close(value, state_before[name], rtol=0, atol=0)


def test_rejection_threshold_is_strictly_greater_than_one_tenth_percent(tmp_path):
    metadata = dict(decisions=3)
    inventory = [(Path(f"episode-{index}"), metadata, index == 0) for index in range(1000)]
    rejected = [dict(episode="episode-0", decisions=3)]
    summary = ppo.summarize_rejections(inventory, rejected, tmp_path)
    assert summary["rejected_nonfinite_fraction"] == .001
    with pytest.raises(ppo.RejectionRateError, match="no optimizer update"):
        ppo.summarize_rejections(inventory[:-1], rejected, tmp_path)
    persisted = json.loads((tmp_path / "rejected-episodes.json").read_text())
    assert persisted["collected_episodes"] == 999
    assert persisted["collected_decisions"] == 2997
    assert persisted["rejected_nonfinite_episodes"] == 1
    assert persisted["rejected_nonfinite_decisions"] == 3


def test_nonfinite_raw_input_is_rejected_before_preprocessing_can_clip_it():
    records = np.zeros(2, dtype=ppo.nav_demos.RECORD_V2_DTYPE)
    records["time_ms"] = [100, 200]
    records["body"]["velocity_cm_s"][1, 0] = np.inf
    layout = np.zeros((ppo.nav_demos.RAY_COUNT, 3), np.float32)
    layout[:, 2] = 1000
    with pytest.raises(ppo.NonFiniteObservation, match="velocity_cm_s") as rejected:
        ppo.actor_observation_features(records, layout, "decision")
    assert rejected.value.detail["time_ms"] == 200
    assert rejected.value.detail["fields"][0]["value"] == "inf"


def test_unmarked_nonfinite_bootstrap_rejects_entire_clean_prefix_before_training(tmp_path):
    directory, actor, _ = make_episode(tmp_path, truncated=True)
    endpoint = directory / "bootstrap.bin.gz"
    records = next(ppo.nav_demos.iter_chunks(endpoint)).copy()
    records["body"]["head_yaw_deg"] = np.nan
    with gzip.open(endpoint, "wb") as stream:
        stream.write(ppo.nav_demos.HEADER.pack(ppo.nav_demos.MAGIC, 2, ppo.nav_demos.RECORD_BYTES,
                ppo.nav_demos.RAY_COUNT, ppo.nav_demos.NEAR_SIZE, ppo.nav_demos.FAR_SIZE, 100))
        stream.write(records.tobytes())
    cache = tmp_path / "cache"
    cache.mkdir()
    with pytest.raises(ppo.RejectionRateError, match="1/1"):
        ppo.prepare_rollouts(args(source=tmp_path, output=tmp_path), actor,
                             ppo.NavCritic(actor).eval(), actor, cache)
    assert not list(cache.iterdir())
    assert not (tmp_path / "last.pt").exists()
    assert not (tmp_path / "policy.navbc").exists()
    rejection = json.loads((tmp_path / "rejected-episodes.json").read_text())
    assert rejection["rejected_nonfinite_decisions"] == 6
    assert rejection["rejected_episode_details"][0]["detected_by"] == "updater"
    assert rejection["rejected_episode_details"][0]["detail"]["stage"] == "bootstrap"


def test_corrupt_log_probability_is_a_fatal_error_and_never_quarantined(tmp_path):
    directory, actor, _ = make_episode(tmp_path)
    path = directory / "samples.jsonl"
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    rows[0]["log_prob"] = float("nan")
    path.write_text("".join(json.dumps(row)+"\n" for row in rows))
    cache = tmp_path / "cache"
    cache.mkdir()
    with pytest.raises(ValueError, match="Corrupt sampled log probabilities") as error:
        ppo.prepare_rollouts(args(source=tmp_path, output=tmp_path), actor,
                             ppo.NavCritic(actor).eval(), actor, cache)
    assert not isinstance(error.value, ppo.NonFiniteObservation)
    assert not (tmp_path / "rejected-episodes.json").exists()


def legacy_boundary_replay(episode, bootstrap, actor, critic, options):
    """Original 16-step boundary replay is an independent oracle for long chunks."""
    count = len(episode["actions"])
    starts = np.arange(0, count, options.segment)
    burn_starts = np.maximum(0, starts-options.burn_in)
    boundaries = sorted(set([0, count, *burn_starts.tolist(), *starts.tolist()]))
    actor_states, critic_states = {}, {}
    actor_h = critic_h = None
    logits = np.empty((count, sum(ppo.CLASSES)), np.float32)
    values = np.empty(count, np.float32)
    with torch.no_grad():
        for start, stop in zip(boundaries, boundaries[1:]):
            actor_states[start] = (np.zeros(actor.hidden_size, np.float32) if actor_h is None
                                   else actor_h[0, 0].cpu().numpy().copy())
            critic_states[start] = (np.zeros(critic.hidden_size, np.float32) if critic_h is None
                                    else critic_h[0, 0].cpu().numpy().copy())
            features = ppo.tensor_features(episode, start, stop, options.device)
            heads, actor_h = actor(**features, hidden=actor_h)
            value, critic_h = critic(**features,
                    priv=torch.from_numpy(episode["priv"][None, start:stop]), hidden=critic_h)
            logits[start:stop] = torch.cat([heads[name] for name in ppo.HEADS], -1)[0].numpy()
            values[start:stop] = value[0].numpy()
        final = float(critic(**{key: torch.from_numpy(value[None]) for key, value in bootstrap.items()},
                              hidden=critic_h)[0][0, 0])
    return dict(logits=logits, values=values, bootstrap=final,
                actor_hidden=np.stack([actor_states[int(start)] for start in burn_starts]),
                critic_hidden=np.stack([critic_states[int(start)] for start in burn_starts]))


def test_large_replay_chunks_match_original_boundary_hidden_logits_and_bootstrap(tmp_path):
    directory, actor, _ = make_episode(tmp_path, truncated=True, count=150)
    episode, bootstrap, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    critic = ppo.NavCritic(actor).eval()
    with torch.no_grad():
        critic.value.weight.fill_(.03)
    options = args(segment=16, burn_in=16)
    expected = legacy_boundary_replay(episode, bootstrap, actor, critic, options)
    expected_advantage, expected_returns = ppo.generalized_advantage(
          episode["rewards"], expected["values"], episode["valid"], episode["terminated"],
          expected["bootstrap"], options.gamma, options.gae_lambda)
    np.testing.assert_allclose(expected["logits"], episode["recorded_logits"], rtol=0, atol=1e-5)
    actor_hooks, critic_hooks = len(actor.gru._forward_hooks), len(critic.gru._forward_hooks)
    error, logits_error = ppo.replay_episode(episode, bootstrap, actor, critic, actor, options)
    assert error < 1e-4 and logits_error < 1e-4
    assert len(actor.gru._forward_hooks) == actor_hooks
    assert len(critic.gru._forward_hooks) == critic_hooks
    for name in ("actor_hidden", "critic_hidden"):
        np.testing.assert_allclose(episode[name], expected[name], rtol=0, atol=1e-5)
    np.testing.assert_allclose(episode["old_values"], expected["values"], rtol=0, atol=1e-5)
    np.testing.assert_allclose(episode["reference_logits"], expected["logits"], rtol=0, atol=1e-5)
    np.testing.assert_allclose(episode["advantage"], expected_advantage, rtol=0, atol=1e-5)
    np.testing.assert_allclose(episode["returns"], expected_returns, rtol=0, atol=1e-5)


def test_recurrent_capture_hook_is_removed_if_model_forward_fails(tmp_path, monkeypatch):
    directory, actor, _ = make_episode(tmp_path)
    episode, _, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    features = ppo.tensor_features(episode, 0, 2, "cpu")
    original_forward = actor.forward
    def broken_forward(**inputs):
        original_forward(**inputs)
        raise RuntimeError("injected downstream failure")
    monkeypatch.setattr(actor, "forward", broken_forward)
    original_hooks = len(actor.gru._forward_hooks)
    with pytest.raises(RuntimeError, match="injected downstream failure"):
        ppo.forward_with_recurrent_states(actor, **features)
    assert len(actor.gru._forward_hooks) == original_hooks


def test_reference_entropy_uses_current_batch_and_same_valid_mask(tmp_path):
    directory, actor, _ = make_episode(tmp_path)
    episode, _, _ = ppo.read_episode(directory, ppo.REWARD_WEIGHTS)
    episode["valid"] = np.array([True, False, True, False, True, False])
    episode["reference_logits"] = np.zeros((6, sum(ppo.CLASSES)), np.float32)
    offset = 0
    for classes in ppo.CLASSES:
        # Excluded intervals have nearly zero entropy; accepted ones are uniform.
        episode["reference_logits"][~episode["valid"], offset] = 10000.
        offset += classes
    path = tmp_path / "episode-cache.npz"
    np.savez_compressed(path, **episode)
    measured = ppo.post_update_statistics([path], actor, args())
    assert measured["reference_entropy_by_head"] == pytest.approx(
        {name: np.log(classes) for name, classes in zip(ppo.HEADS, ppo.CLASSES)}, abs=1e-6)
    assert set(measured["entropy_by_head"]) == set(measured["reference_entropy_by_head"])


def test_lossless_fast_cache_roundtrip_preserves_all_training_shapes_and_dtypes(tmp_path):
    rng = np.random.default_rng(2718)
    floating = lambda shape: rng.standard_normal(shape).astype(np.float32)
    arrays = dict(near=floating((3, 9, 32, 32)), far=floating((3, 9, 25, 25)),
                  vector=floating((3, ppo.VECTOR_SIZE)), priv=floating((3, ppo.PRIVILEGED_SIZE)),
                  actions=np.array([[2, 1, 0, 1, 0, 0], [1, 2, 1, 0, 1, 0], [0, 0, 2, 1, 0, 1]], np.int64),
                  old_logp=floating((3,)), rewards=floating((3,)), valid=np.array([True, False, True]),
                  terminated=np.array([False, False, True]), advantage=floating((3,)), returns=floating((3,)),
                  old_values=floating((3,)), reference_logits=floating((3, sum(ppo.CLASSES))),
                  starts=np.array([0, 2], np.int64), actor_hidden=floating((2, 256)),
                  critic_hidden=floating((2, 256)))
    path = tmp_path / "training-cache.npz"
    ppo.save_cache(path, arrays)
    with ppo.zipfile.ZipFile(path) as archive:
        assert archive.namelist() == [f"{name}.npy" for name in arrays]
        assert all(member.compress_type == ppo.zipfile.ZIP_DEFLATED for member in archive.infolist())
    with np.load(path, allow_pickle=False) as independent:
        assert independent.files == list(arrays)
        for name, expected in arrays.items():
            actual = independent[name]
            assert actual.shape == expected.shape and actual.dtype == expected.dtype
            np.testing.assert_array_equal(actual, expected)
            assert actual.tobytes() == expected.tobytes()
    restored = ppo.load_cache(path)
    assert list(restored) == list(arrays)
    for name, expected in arrays.items():
        np.testing.assert_array_equal(restored[name], expected)
