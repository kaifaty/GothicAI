"""Protocol/state tests use a counter backend, never a local neural model."""
from __future__ import annotations

from copy import deepcopy
from io import StringIO
import json
from pathlib import Path
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from gothic_body.runtime import LatestMailbox, PolicyRuntime, TorchBackend
from gothic_body.serving import serve_stream


class CounterBackend:
    metadata = {"sampling_version": "periodic-v1"}

    def __init__(self):
        self.calls = []

    def step(self, features, mask, state):
        self.calls.append((features, mask, state))
        return [float(index) for index in range(12)], (state or 0)+1


def runtime(backend=None, **kwargs):
    return PolicyRuntime(backend=backend or CounterBackend(), normalizer={"feature_names": ["dt"]},
                         checkpoint_sha256="frozen", feature_packer=lambda row, normalizer, sequence_dt_game_ms:
                         [float(sequence_dt_game_ms)], **kwargs)


def request(sequence=1, *, game_ms=None, entity=1, generation=1, world=7, eligible=True):
    return {"stream_seq": sequence, "initial_game_ms": 100, "observation": {"session_epoch": 4, "world_epoch": world,
            "entity_id": entity, "control_generation": generation, "observation_seq": sequence*2+entity,
            "t_game_ms": 100+sequence*50 if game_ms is None else game_ms, "periodic": True,
            "sample_period_ms": 50,
            "phase": "post_simulation_post_animation", "can_decide": eligible,
            "native_decision_eligible": eligible, "action_mask_valid": True,
            "availability_mask": [True]+([True]*11 if eligible else [False]*11),
            "action_mask": [True]+([True]*11 if eligible else [False]*11)}}


def test_duplicate_is_cached_without_hidden_advance_and_changed_payload_is_rejected():
    policy = runtime()
    first = policy.process(request())
    assert first["model_processed"] and first["hidden_reset"]
    retry = policy.process(request())
    assert retry["status"] == "cached" and not retry["model_processed"]
    assert len(policy.backend.calls) == 1
    changed = request()
    changed["observation"]["action_mask"][11] = False
    assert policy.process(changed)["reason"] == "duplicate_payload_changed"
    assert len(policy.backend.calls) == 1


def test_actor_generation_and_episode_states_are_isolated():
    policy = runtime()
    policy.begin_episode(4, 7, initial_game_ms=100)
    for entity in (1, 2):
        policy.process(request(entity=entity))
        policy.process(request(2, entity=entity))
    assert [call[2] for call in policy.backend.calls] == [None, 1, None, 1]
    policy.process(request(generation=2, game_ms=250))
    assert policy.backend.calls[-1][2] is None
    assert policy.process(request(3, generation=1))["reason"] == "stale_generation"
    policy.begin_episode(4, 8, initial_game_ms=100)
    assert policy.process(request(world=7))["reason"] == "stale_epoch"
    policy.process(request(world=8))
    assert policy.backend.calls[-1][2] is None


def test_global_native_sequence_interleaving_does_not_count_as_gap():
    policy = runtime()
    first, second = request(), request(2)
    first["observation"]["observation_seq"] = 17
    second["observation"]["observation_seq"] = 49
    assert policy.process(first)["status"] == "processed"
    assert policy.process(second)["status"] == "processed"
    assert policy.backend.calls[-1][2] == 1


def test_gap_is_explicit_fallback_and_next_contiguous_row_starts_at_zero():
    policy = runtime()
    policy.process(request())
    gap = policy.process(request(3))
    assert gap["reason"] == "unexpected_gap" and gap["hidden_reset"] and gap["action_id"] is None
    assert len(policy.backend.calls) == 1
    assert policy.process(request(4))["model_processed"]
    assert policy.backend.calls[-1][2] is None


def test_clock_regression_and_stale_sequences_never_advance_state():
    policy = runtime()
    policy.process(request())
    assert policy.process(request(2, game_ms=149))["reason"] == "clock_regression"
    assert len(policy.backend.calls) == 1
    policy.process(request(2))
    assert policy.process(request())["reason"] == "stale_sequence"
    assert len(policy.backend.calls) == 2


@pytest.mark.parametrize("mutate,reason", [
    (lambda req: req.update(now_game_ms=300), "observation_too_old"),
    (lambda req: req.update(checkpoint_sha256="other"), "checkpoint_changed_mid_episode"),
    (lambda req: req["observation"].update(phase="pre_native_primitive"), "unsupported_sampling_phase"),
    (lambda req: req["observation"].update(periodic=False), "unsupported_sampling_phase"),
    (lambda req: req["observation"].update(action_mask=[False]*12, availability_mask=[False]*12), "empty_raw_mask_requires_memory_only"),
    (lambda req: req["observation"].update(action_mask=[1]*12), "invalid_action_mask"),
])
def test_invalid_or_stale_input_produces_no_action(mutate, reason):
    policy, req = runtime(), request()
    mutate(req)
    response = policy.process(req)
    assert response["reason"] == reason and response["action_id"] is None
    assert not response["model_processed"] and not policy.backend.calls


def test_locked_periodic_rows_update_memory_with_continue_only_mask():
    policy = runtime()
    response = policy.process(request(eligible=False))
    assert response["model_processed"] and response["action_id"] is None and response["reason"] == "locked"
    assert policy.backend.calls[0][1] == [True]+[False]*11
    assert response["effective_action_mask"] == [True]+[False]*11
    policy.process(request(2))
    assert policy.backend.calls[-1][0] == [50.0] and policy.backend.calls[-1][2] == 1


def test_stale_memory_only_rows_update_hidden_without_actions_when_enabled():
    policy = runtime(allow_stale_memory_only=True)
    policy.process(request())
    old = request(2)
    old.update(memory_only=True, command_enabled=False, now_game_ms=450)
    response = policy.process(old)
    assert response["status"] == "processed" and response["model_processed"]
    assert response["memory_only"] and response["action_id"] is None and not response["new_decision"]
    assert not response["hidden_reset"] and policy.backend.calls[-1][2] == 1
    assert policy.arbitrate(old["observation"], stream_seq=2)["reason"] == "memory_only"
    assert policy.process(request(3))["model_processed"]
    assert policy.backend.calls[-1][2] == 2


def test_stale_actionable_rows_still_fall_back_when_memory_age_bypass_enabled():
    policy = runtime(allow_stale_memory_only=True)
    policy.process(request())
    old = request(2)
    old.update(now_game_ms=301, command_enabled=True)
    response = policy.process(old)
    assert response["reason"] == "observation_too_old" and response["hidden_reset"]
    assert response["action_id"] is None and not response["model_processed"]
    assert len(policy.backend.calls) == 1


@pytest.mark.parametrize("options", ({}, {"allow_stale_memory_only": False}))
def test_memory_only_age_gate_preserves_previous_default(options):
    policy = runtime(**options)
    old = request()
    old.update(memory_only=True, command_enabled=False, now_game_ms=300)
    response = policy.process(old)
    assert response["reason"] == "observation_too_old" and response["hidden_reset"]
    assert not response["model_processed"] and not policy.backend.calls


def test_stale_memory_only_cannot_bypass_context_validation():
    policy = runtime(allow_stale_memory_only=True)
    old = request()
    old.update(memory_only=True, command_enabled=True, now_game_ms=300)
    response = policy.process(old)
    assert response["reason"] == "observation_too_old" and not response["model_processed"]
    assert not policy.backend.calls


def test_actor_limit_bounds_memory():
    policy = runtime(max_actors=1)
    policy.process(request(entity=1))
    assert policy.process(request(entity=2))["reason"] == "actor_capacity"
    assert len(policy.states) == 1


def test_backend_error_fails_closed():
    class Broken(CounterBackend):
        def step(self, features, mask, state):
            raise RuntimeError("unavailable")
    response = runtime(Broken()).process(request())
    assert response["reason"] == "backend_error" and response["hidden_reset"]
    assert response["action_id"] is None


def test_fresh_gate_arbitration_uses_cached_periodic_scores_without_advancing_hidden():
    policy = runtime()
    req = request()
    row = req["observation"]
    row["action_mask"][11] = False
    row["availability_mask"][11] = False
    processed = policy.process(req)
    first = policy.arbitrate(row, stream_seq=1)
    assert first["action_id"] == 10 and first["status"] == "decision"
    assert first["features_sha256"] == processed["features_sha256"]
    assert first["periodic_observation_seq"] == processed["observation_seq"]
    assert not first["model_processed"] and len(policy.backend.calls) == 1
    assert policy.arbitrate(row, stream_seq=1)["status"] == "cached"
    row["t_game_ms"] += 68
    assert policy.arbitrate(row, stream_seq=1)["reason"] == "periodic_anchor_too_old"


def test_mailbox_one_inflight_coalesces_and_drops_obsolete_answer():
    entered, release = threading.Event(), threading.Event()

    class Slow(CounterBackend):
        def step(self, features, mask, state):
            entered.set()
            assert release.wait(2)
            return super().step(features, mask, state)

    gateway = LatestMailbox(runtime(Slow()), max_actors=1)
    try:
        gateway.submit(request())
        assert entered.wait(2)
        gateway.submit(request(2))
        gateway.submit(request(3))
        with pytest.raises(ValueError, match="capacity"):
            gateway.submit(request(entity=2))
        release.set()
        with gateway.condition:
            assert gateway.condition.wait_for(lambda: gateway.results, timeout=2)
        response = gateway.poll(1)
        assert response["stream_seq"] == 3 and response["reason"] == "unexpected_gap"
        assert gateway.counters["coalesced"] == 1 and gateway.counters["discarded_old_reply"] == 1
        assert len(gateway.runtime.backend.calls) == 1
    finally:
        release.set()
        gateway.close()


def test_jsonl_service_freezes_checkpoint_and_journals_processed_requests():
    policy = runtime()
    reset = {"id": 1, "cmd": "reset", "session_epoch": 4, "world_epoch": 7, "initial_game_ms": 100}
    req = {"id": 2, **request()}
    stream = StringIO("\n".join(json.dumps(item) for item in (reset, req, {"id": 3, "cmd": "close"}))+"\n")
    output, journal = StringIO(), StringIO()
    assert serve_stream(policy, stream, output, journal=journal) == 0
    responses = [json.loads(line) for line in output.getvalue().splitlines()]
    assert responses[1]["checkpoint_sha256"] == "frozen" and responses[1]["model_processed"]
    assert [json.loads(line)["request"] for line in journal.getvalue().splitlines()] == [reset, req, {"id": 3, "cmd": "close"}]


def test_torch_backend_guard_precedes_import(monkeypatch):
    monkeypatch.setattr(sys, "platform", "darwin")
    with pytest.raises(RuntimeError, match="never on the Mac"):
        TorchBackend("does-not-exist")


def test_first_periodic_dt_uses_native_reset_boundary():
    policy = runtime()
    policy.begin_episode(4, 7, initial_game_ms=117)
    first = policy.process(request(game_ms=150))
    assert first["sequence_dt_game_ms"] == 33
    assert policy.backend.calls[0][0] == [33.0]
    policy.process(request(2, game_ms=217))
    assert policy.backend.calls[1][0] == [67.0]


def test_generation_resets_hidden_but_retains_actor_periodic_clock():
    policy = runtime()
    policy.begin_episode(4, 7, initial_game_ms=117)
    policy.process(request(game_ms=150))
    response = policy.process(request(generation=2, game_ms=217))
    assert response["hidden_reset"] and response["sequence_dt_game_ms"] == 67
    assert policy.backend.calls[-1][0] == [67.0] and policy.backend.calls[-1][2] is None
    policy.begin_episode(4, 8, initial_game_ms=200)
    response = policy.process(request(world=8, game_ms=233))
    assert response["sequence_dt_game_ms"] == 33 and policy.backend.calls[-1][2] is None


def test_skipped_periodic_bucket_is_detected_even_with_contiguous_stream_and_100ms_dt():
    policy = runtime()
    policy.process(request(game_ms=150))
    response = policy.process(request(2, game_ms=250))
    assert response["reason"] == "unexpected_periodic_gap" and response["sequence_dt_game_ms"] == 100
    assert response["hidden_reset"] and not response["model_processed"]
    assert len(policy.backend.calls) == 1


def test_terminal_barrier_clears_memory_rejects_followup_and_stale_end_cannot_clear_new_episode():
    policy = runtime()
    policy.begin_episode(4, 7, initial_game_ms=100)
    policy.process(request())
    response = policy.end_episode(4, 7)
    assert response["accepted"] and response["hidden_reset"] and not response["model_processed"]
    assert response["runtime_before_clear"]["actors"] == 1
    assert not policy.states and not policy.actor_clocks and policy.status()["episode_closed"]
    assert policy.process(request(2))["reason"] == "episode_ended"
    assert len(policy.backend.calls) == 1
    policy.begin_episode(4, 8, initial_game_ms=100)
    policy.process(request(world=8))
    assert policy.end_episode(4, 7)["reason"] == "stale_episode_end"
    assert policy.process(request(2, world=8))["model_processed"]
    assert policy.backend.calls[-1][2] == 1


def test_empty_raw_terminal_mask_updates_memory_only_then_terminal_clears():
    policy = runtime()
    policy.begin_episode(4, 7, initial_game_ms=100)
    req = request()
    req["memory_only"] = True
    req["observation"].update(action_mask=[False]*12, availability_mask=[False]*12,
                               native_decision_eligible=False, can_decide=False)
    original = deepcopy(req["observation"])
    response = policy.process(req)
    assert response["model_processed"] and response["selected_action_id"] == 0
    assert response["action_id"] is None and not response["new_decision"] and response["memory_only"]
    assert response["effective_action_mask"] == [True]+[False]*11
    assert response["raw_action_mask"] == [False]*12 and req["observation"] == original
    assert policy.process(req)["status"] == "cached" and len(policy.backend.calls) == 1
    assert policy.end_episode(4, 7)["accepted"] and not policy.states


def test_live_locked_empty_mask_updates_memory_until_legal_periodic_decision_returns():
    policy = runtime()
    policy.begin_episode(4, 7, initial_game_ms=100)
    locked = request(eligible=False)
    locked.update(memory_only=True, command_enabled=True)
    locked["observation"].update(action_mask=[False]*12, availability_mask=[False]*12)
    original = deepcopy(locked["observation"])
    response = policy.process(locked)
    assert response["status"] == "processed" and response["model_processed"]
    assert response["action_id"] is None and not response["new_decision"]
    assert locked["observation"] == original and response["raw_action_mask"] == [False]*12
    assert not policy.counters["empty_raw_mask_requires_memory_only"]
    legal = request(2)
    response = policy.process(legal)
    assert response["model_processed"] and policy.backend.calls[-1][2] == 1
    decision = policy.arbitrate(legal["observation"], stream_seq=2)
    assert decision["status"] == "decision" and decision["action_id"] == 11


def test_lean_runtime_makes_the_same_decisions_without_digests():
    def run(lean):
        policy = runtime(lean=lean)
        policy.begin_episode(4, 7, initial_game_ms=100)
        trace = []
        for sequence in range(1, 6):
            first = request(sequence, eligible=sequence % 2 == 1)
            response = policy.process(first)
            row = deepcopy(first["observation"])
            decision = policy.arbitrate(row, stream_seq=sequence)
            trace.append((response["status"], response["action_id"], response["reason"], decision["status"],
                          decision.get("action_id"), policy.last_features))
        changed = request(6)
        policy.process(changed)
        moved = deepcopy(changed["observation"])
        moved["can_decide"] = False
        trace.append(policy.arbitrate(moved, stream_seq=6)["reason"])
        return policy, trace
    lean, lean_trace = run(True)
    full, full_trace = run(False)
    assert lean_trace == full_trace
    assert lean_trace[-1] == "periodic_anchor_mismatch"
    assert [call[2] for call in lean.backend.calls] == [call[2] for call in full.backend.calls]
    assert lean.states[next(iter(lean.states))].response["observation_sha256"] is None
    assert full.states[next(iter(full.states))].response["observation_sha256"] is not None
