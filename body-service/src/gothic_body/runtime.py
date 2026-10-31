"""Bounded recurrent policy runtime; Torch is loaded only by a Linux backend.

The caller supplies an actor-local stream sequence. The native observer's
global sequence interleaves actors and must never be used to infer a gap.
One process owns a frozen checkpoint, and each identity owns independent state.
"""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import threading
import time
from typing import Any, Callable

IDENTITY_KEYS = ("session_epoch", "world_epoch", "entity_id", "control_generation")
SAMPLING_VERSION = "periodic-v1"
MAX_REQUEST_BYTES = 256 * 1024


def digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                     allow_nan=False).encode()).hexdigest()


def actor_identity(row: dict[str, Any]) -> tuple[int, ...]:
    values = tuple(row.get(key) for key in IDENTITY_KEYS)
    if any(type(value) is not int or value < 0 for value in values):
        raise ValueError("invalid observation identity")
    return values


def shared_feature_packer() -> Callable:
    # The package lives beside scripts in the source tree. Distribution entry
    # points may explicitly pass an installed packer instead.
    scripts = Path(__file__).resolve().parents[3] / "scripts"
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    from body_trajectory_dataset import pack_features
    return pack_features


def shared_policy_mask() -> Callable:
    scripts = Path(__file__).resolve().parents[3] / "scripts"
    if str(scripts) not in sys.path:
        sys.path.insert(0, str(scripts))
    from body_trajectory_dataset import policy_action_mask
    return policy_action_mask


def configure_inference_cpu_threads(torch_module, inference_cpu_threads: int = 2) -> dict[str, int]:
    """Bound the private actor's intra-op pool; preserve the historical default."""
    if type(inference_cpu_threads) is not int or inference_cpu_threads not in (1, 2):
        raise ValueError("inference CPU threads must be 1 or 2")
    torch_module.set_num_threads(inference_cpu_threads)
    actual = int(torch_module.get_num_threads())
    if actual != inference_cpu_threads:
        raise RuntimeError("Torch inference thread limit was not applied")
    return {"inference_cpu_threads": inference_cpu_threads, "torch_intraop_threads": actual,
            "torch_interop_threads": int(torch_module.get_num_interop_threads())}


class TorchBackend:
    """A real actor backend. Constructing it on the development Mac is refused."""
    def __init__(self, checkpoint: Path | str, *, device: str = "cpu", inference_cpu_threads: int = 2):
        if sys.platform != "linux":
            raise RuntimeError("models run only on dev-ubuntu, never on the Mac")
        import torch
        self.execution_profile = configure_inference_cpu_threads(torch, inference_cpu_threads)
        from .checkpoint import load_checkpoint
        loaded = load_checkpoint(Path(checkpoint), device=device)
        self.model, self.normalizer = loaded["model"], loaded["normalizer"]
        self.metadata = loaded.get("metadata", {})
        if self.metadata.get("training", {}).get("deployment_candidate") is False:
            raise ValueError("train-only diagnostic checkpoint cannot control a live body")
        self.torch, self.device = torch, device
        self.last_step_timing = {}
        self.model.eval()

    def step(self, features: list[float], mask: list[bool], state: Any) -> tuple[list[float], Any]:
        torch = self.torch
        wall, thread_cpu, process_cpu = time.perf_counter_ns(), time.thread_time_ns(), time.process_time_ns()
        try:
            self.execution_profile = {**self.execution_profile,
                                      "torch_intraop_threads": int(torch.get_num_threads()),
                                      "torch_interop_threads": int(torch.get_num_interop_threads())}
            if self.execution_profile["torch_intraop_threads"] != self.execution_profile["inference_cpu_threads"]:
                raise RuntimeError("Torch inference thread profile changed after startup")
            with torch.inference_mode():
                values = torch.tensor([features], dtype=torch.float32, device=self.device)
                masks = torch.tensor([mask], dtype=torch.bool, device=self.device)
                logits, state = self.model.step(values, masks, state=state)
                return logits[0].detach().cpu().tolist(), state
        finally:
            self.last_step_timing = {"wall_ms": (time.perf_counter_ns()-wall)/1e6,
                                    "main_thread_cpu_ms": (time.thread_time_ns()-thread_cpu)/1e6,
                                    "process_cpu_ms": (time.process_time_ns()-process_cpu)/1e6}


@dataclass
class ActorState:
    sequence: int = 0
    game_ms: int | None = None
    fingerprint: str = ""
    hidden: Any = None
    response: dict[str, Any] | None = None
    scores: list[float] | None = None
    arbitration_fingerprint: Any = ""
    arbitration_response: dict[str, Any] | None = None
    observation: dict[str, Any] | None = None


class PolicyRuntime:
    """Serial inference with cached retries and explicit stale/gap fallbacks.

    ``max_age_game_ms`` uses the engine's own game clock. Local service timing
    is diagnostic; monotonic timestamps from different machines are not mixed.
    A gap discards memory and the decision. A later contiguous input restarts
    from zero, and the caller can release/reacquire its native lease meanwhile.

    ``lean`` is for training collectors that send each periodic row once: payload
    fingerprints become (sequence, source, memory flag) tuples, the arbitrated row is
    compared to the processed one directly, and no JSON digests or request-size
    encodings are computed. Decisions, memory and fallbacks are the same.
    """
    def __init__(self, checkpoint: Path | str | None = None, *, device: str = "cpu", backend=None,
                 checkpoint_sha256: str | None = None, normalizer: dict[str, Any] | None = None,
                 feature_packer: Callable | None = None, max_actors: int = 16, max_age_game_ms: int = 100,
                 max_dt_game_ms: int = 100, inference_cpu_threads: int = 2, lean: bool = False,
                 allow_stale_memory_only: bool = False):
        if type(max_actors) is not int or not 1 <= max_actors <= 256:
            raise ValueError("invalid actor bound")
        if type(inference_cpu_threads) is not int or inference_cpu_threads not in (1, 2):
            raise ValueError("inference CPU threads must be 1 or 2")
        if backend is None:
            if checkpoint is None:
                raise ValueError("checkpoint is required")
            backend = TorchBackend(checkpoint, device=device, inference_cpu_threads=inference_cpu_threads)
        self.backend = backend
        self.inference_cpu_threads = inference_cpu_threads
        self.normalizer = normalizer if normalizer is not None else backend.normalizer
        self.metadata = getattr(backend, "metadata", {})
        self.packer = feature_packer or shared_feature_packer()
        self.masker = shared_policy_mask()
        self.checkpoint_sha256 = checkpoint_sha256 or hashlib.sha256(Path(checkpoint).read_bytes()).hexdigest()
        self.normalizer_sha256 = self.metadata.get("normalization_sha256", digest(self.normalizer))
        self.max_actors, self.max_age_game_ms, self.max_dt_game_ms = max_actors, max_age_game_ms, max_dt_game_ms
        self.states: dict[tuple[int, ...], ActorState] = {}
        self.active: dict[int, tuple[int, ...]] = {}
        self.actor_clocks: dict[int, tuple[int, int, int]] = {}
        self.expected_epochs: tuple[int, int] | None = None
        self.initial_game_ms: int | None = None
        self.episode_closed = False
        self.counters: Counter = Counter()
        self.lock = threading.RLock()
        self.lean = lean
        # The live network pilot can replay a frame's backlog through memory.
        # Other callers retain the existing age gate unless explicitly opted in.
        self.allow_stale_memory_only = allow_stale_memory_only
        # Features of the last row the backend consumed (a collector stores them as training input).
        self.last_features: list[float] | None = None

    def begin_episode(self, session_epoch: int, world_epoch: int, *, initial_game_ms: int = 0) -> None:
        if any(type(value) is not int or value < 0 for value in (session_epoch, world_epoch)):
            raise ValueError("invalid episode epochs")
        if type(initial_game_ms) is not int or initial_game_ms < 0:
            raise ValueError("invalid episode game-time boundary")
        with self.lock:
            self.states.clear()
            self.active.clear()
            self.actor_clocks.clear()
            self.expected_epochs = (session_epoch, world_epoch)
            self.initial_game_ms = initial_game_ms
            self.episode_closed = False
            self.counters["episode_resets"] += 1

    def end_episode(self, session_epoch: int, world_epoch: int) -> dict[str, Any]:
        with self.lock:
            if self.expected_epochs != (session_epoch, world_epoch):
                return {"accepted": False, "reason": "stale_episode_end", "hidden_reset": False,
                        "model_processed": False}
            before = self.status()
            self.states.clear()
            self.active.clear()
            self.actor_clocks.clear()
            self.expected_epochs, self.initial_game_ms, self.episode_closed = None, None, True
            self.counters["episode_ends"] += 1
            return {"accepted": True, "reason": "episode_ended", "hidden_reset": True,
                    "model_processed": False, "runtime_before_clear": before,
                    "runtime_after_clear": self.status()}

    def reset_actor(self, identity: tuple[int, ...], reason: str = "explicit_reset") -> None:
        with self.lock:
            self.states.pop(identity, None)
            self.counters[reason] += 1

    def status(self) -> dict[str, Any]:
        with self.lock:
            return {"checkpoint_sha256": self.checkpoint_sha256, "normalizer_sha256": self.normalizer_sha256,
                    "sampling_version": self.metadata.get("sampling_version", SAMPLING_VERSION),
                    "actors": len(self.states), "counters": dict(self.counters),
                    "expected_epochs": list(self.expected_epochs) if self.expected_epochs else None,
                    "initial_game_ms": self.initial_game_ms, "episode_closed": self.episode_closed,
                    "execution_profile": dict(getattr(self.backend, "execution_profile", {}))}

    def process(self, request: dict[str, Any], *, now_game_ms: int | None = None) -> dict[str, Any]:
        with self.lock:
            return self._process(request, now_game_ms=now_game_ms)

    def _process(self, request, *, now_game_ms):
        row = request.get("observation")
        if not isinstance(row, dict):
            raise ValueError("observation object required")
        identity = actor_identity(row)
        sequence = request.get("stream_seq")
        game_ms, native_sequence = row.get("t_game_ms"), row.get("observation_seq")
        if any(type(value) is not int or value < 0 for value in (sequence, game_ms, native_sequence)) or sequence == 0:
            raise ValueError("invalid observation sequence or game time")
        if not self.lean and len(json.dumps(request, allow_nan=False)) > MAX_REQUEST_BYTES:
            raise ValueError("request exceeds bounded reader")
        if now_game_ms is None:
            now_game_ms = request.get("now_game_ms", game_ms)
        if type(now_game_ms) is not int:
            raise ValueError("now_game_ms must come from the engine clock")
        base = {"schema_version": 1, "identity": dict(zip(IDENTITY_KEYS, identity)), "stream_seq": sequence,
                "observation_seq": native_sequence, "t_game_ms": game_ms, "phase": row.get("phase"),
                "source": request.get("source", "periodic_observer"), "checkpoint_sha256": self.checkpoint_sha256,
                "normalizer_sha256": self.normalizer_sha256, "sampling_version": self.metadata.get("sampling_version", SAMPLING_VERSION),
                "action_id": None, "model_processed": False, "hidden_reset": False, "status": "fallback"}

        def fallback(reason, reset=False):
            if reset and identity in self.states:
                self.states[identity].hidden = None
                self.states[identity].scores = None
            self.counters[reason] += 1
            return {**base, "reason": reason, "hidden_reset": reset}

        if request.get("checkpoint_sha256", self.checkpoint_sha256) != self.checkpoint_sha256:
            return fallback("checkpoint_changed_mid_episode")
        if self.episode_closed:
            return fallback("episode_ended")
        if row.get("periodic") is not True or row.get("phase") != "post_simulation_post_animation":
            return fallback("unsupported_sampling_phase")
        if row.get("sample_period_ms") != 50:
            return fallback("unsupported_sampling_rate")
        if self.expected_epochs is not None and identity[:2] != self.expected_epochs:
            return fallback("stale_epoch")
        previous = self.active.get(identity[2])
        if previous is not None and previous != identity:
            if (identity[0], identity[1], identity[3]) < (previous[0], previous[1], previous[3]):
                return fallback("stale_generation")
            self.states.pop(previous, None)
        state = self.states.get(identity)
        if state is None:
            if len(self.states) >= self.max_actors or identity[2] not in self.active and len(self.active) >= self.max_actors:
                return fallback("actor_capacity")
            state = ActorState()
            self.states[identity] = state
            self.active[identity[2]] = identity
            base["hidden_reset"] = True
        if self.lean:
            fingerprint = (native_sequence, request.get("source", "periodic_observer"), request.get("memory_only") is True)
        else:
            fingerprint = digest({"observation": row, "source": request.get("source", "periodic_observer"),
                                  "memory_only": request.get("memory_only") is True})
        if sequence == state.sequence:
            if fingerprint != state.fingerprint:
                return fallback("duplicate_payload_changed", True)
            self.counters["cached"] += 1
            return {**state.response, "status": "cached", "model_processed": False, "cached_model_processed":
                    state.response["model_processed"], "hidden_reset": False}
        if sequence < state.sequence:
            return fallback("stale_sequence")
        clock = self.actor_clocks.get(identity[2])
        previous_game = clock[2] if clock is not None and clock[:2] == identity[:2] else None
        if previous_game is not None and game_ms < previous_game or now_game_ms < game_ms:
            return fallback("clock_regression", True)
        boundary = self.initial_game_ms if self.initial_game_ms is not None else request.get("initial_game_ms")
        if previous_game is None and (type(boundary) is not int or boundary < 0 or boundary > game_ms):
            return fallback("missing_reset_time_boundary", True)
        dt = game_ms-boundary if previous_game is None else game_ms-previous_game
        base["sequence_dt_game_ms"] = dt
        gap = sequence != state.sequence+1
        bucket_gap = previous_game is not None and game_ms//50 > previous_game//50+1
        memory_only = request.get("memory_only") is True
        valid_memory_only = memory_only and (row.get("can_decide") is False or
                                             request.get("command_enabled") is False)
        check_age = not (self.allow_stale_memory_only and valid_memory_only)
        reason = ("unexpected_gap" if gap or request.get("gap") is True else
                  "unexpected_periodic_gap" if bucket_gap else
                  "observation_too_old" if check_age and now_game_ms-game_ms > self.max_age_game_ms else
                  "unexpected_time_gap" if previous_game is not None and dt > self.max_dt_game_ms else None)
        if reason:
            state.sequence, state.game_ms, state.fingerprint = sequence, game_ms, fingerprint
            self.actor_clocks[identity[2]] = (*identity[:2], game_ms)
            state.response = fallback(reason, True)
            return state.response.copy()
        mask = row.get("action_mask", row.get("availability_mask"))
        if not isinstance(mask, list) or len(mask) != 12 or any(type(value) is not bool for value in mask):
            return fallback("invalid_action_mask", True)
        if memory_only and row.get("can_decide") is not False and request.get("command_enabled") is not False:
            return fallback("invalid_memory_only_context", True)
        if not any(mask) and not memory_only:
            return fallback("empty_raw_mask_requires_memory_only", True)
        eligible = row.get("can_decide") is True and row.get("action_mask_valid") is True and not memory_only
        # Locked observations still update memory, without forcing an unavailable
        # physical action or a NaN softmax over an all-false mask.
        try:
            model_mask = self.masker(row)
        except (ValueError, TypeError, KeyError) as error:
            return fallback("invalid_action_mask", True)
        if not any(model_mask):
            return fallback("invalid_action_mask", True)
        if eligible and not any(model_mask[1:]):
            return fallback("no_available_primitive", True)
        try:
            features = self.packer(row, self.normalizer, sequence_dt_game_ms=dt)
            if any(not math.isfinite(value) for value in features):
                return fallback("invalid_features", True)
            self.last_features = features
            started = time.perf_counter_ns()
            logits, hidden = self.backend.step(features, model_mask, state.hidden)
            infer_ms = (time.perf_counter_ns()-started)/1_000_000
            if len(logits) != 12 or any(not math.isfinite(value) for value in logits):
                return fallback("invalid_logits", True)
            state.hidden = hidden
            state.scores = logits
            state.arbitration_fingerprint, state.arbitration_response = "", None
            action = max((index for index, allowed in enumerate(model_mask) if allowed), key=lambda index: logits[index])
            response = {**base, "status": "processed", "reason": "decision" if eligible and action else "locked",
                        "action_id": action if eligible and action else None, "model_processed": True,
                        "features_sha256": None if self.lean else digest(features), "feature_count": len(features),
                        "effective_action_mask": model_mask, "inference_ms": infer_ms,
                        "backend_execution_profile": dict(getattr(self.backend, "execution_profile", {})),
                        "backend_timing": dict(getattr(self.backend, "last_step_timing", {})),
                        "logits": logits, "logits_semantics": "masked_float32_finite_min",
                        "observation_sha256": None if self.lean else digest(row), "raw_action_mask": mask,
                        "memory_only": memory_only, "selected_action_id": action, "new_decision": bool(eligible and action)}
            state.sequence, state.game_ms, state.fingerprint, state.response = sequence, game_ms, fingerprint, response
            state.observation = row if self.lean else None
            self.actor_clocks[identity[2]] = (*identity[:2], game_ms)
            self.counters["processed"] += 1
            self.counters["decisions" if response["action_id"] else "locked"] += 1
            return response.copy()
        except (ValueError, TypeError, KeyError, RuntimeError, OverflowError) as error:
            response = fallback("backend_error", True)
            response["error"] = str(error)[:512]
            return response

    def arbitrate(self, row: dict[str, Any], *, stream_seq: int, max_anchor_age_game_ms: int = 67) -> dict[str, Any]:
        """Read existing periodic preferences at a current physical command gate.

        This method never reads features or advances memory. The current native
        gate must be the same periodic row which supplied the cached scores.
        """
        with self.lock:
            identity = actor_identity(row)
            state = self.states.get(identity)
            base = {"schema_version": 1, "identity": dict(zip(IDENTITY_KEYS, identity)), "stream_seq": stream_seq,
                    "observation_seq": row.get("observation_seq"), "t_game_ms": row.get("t_game_ms"),
                    "phase": row.get("phase"), "checkpoint_sha256": self.checkpoint_sha256,
                    "normalizer_sha256": self.normalizer_sha256,
                    "sampling_version": self.metadata.get("sampling_version", SAMPLING_VERSION),
                    "action_id": None, "model_processed": False, "hidden_reset": False, "status": "fallback"}
            if state is None or state.scores is None or state.sequence != stream_seq:
                return {**base, "reason": "no_current_periodic_state"}
            if state.response.get("memory_only") is True:
                return {**base, "status": "locked", "reason": "memory_only"}
            fingerprint = ((row.get("observation_seq"), row.get("t_game_ms"), stream_seq) if self.lean else
                           digest({"observation": row, "stream_seq": stream_seq}))
            if fingerprint == state.arbitration_fingerprint:
                return {**state.arbitration_response, "status": "cached"}
            base.update(periodic_observation_seq=state.response["observation_seq"], periodic_game_ms=state.game_ms,
                        features_sha256=state.response["features_sha256"])
            now = row.get("t_game_ms")
            if type(now) is not int or not 0 <= now-state.game_ms <= max_anchor_age_game_ms:
                return {**base, "reason": "periodic_anchor_too_old"}
            same_row = (row is state.observation or row == state.observation) if self.lean else \
                digest(row) == state.response["observation_sha256"]
            if row.get("observation_seq") != state.response["observation_seq"] or now != state.game_ms or \
                    row.get("periodic") is not True or not same_row:
                return {**base, "reason": "periodic_anchor_mismatch"}
            mask = row.get("action_mask")
            if row.get("can_decide") is not True or row.get("action_mask_valid") is not True:
                return {**base, "status": "locked", "reason": "locked"}
            if not isinstance(mask, list) or len(mask) != 12 or any(type(item) is not bool for item in mask) or not any(mask[1:]):
                return {**base, "reason": "invalid_arbitration_mask"}
            try:
                model_mask = self.masker(row)
            except (ValueError, KeyError, TypeError):
                return {**base, "reason": "invalid_arbitration_mask"}
            action = max((index for index in range(1, 12) if model_mask[index]), key=lambda index: state.scores[index])
            response = {**base, "status": "decision", "reason": "decision", "action_id": action,
                        "effective_action_mask": model_mask, "raw_action_mask": mask}
            state.arbitration_fingerprint, state.arbitration_response = fingerprint, response
            return response.copy()


class LatestMailbox:
    """One inference in flight, at most one pending row per bounded actor.

    Replacing a row is observable and becomes a recurrent gap. Replies from an
    old sequence or generation are discarded before the caller can apply them.
    This gateway provides load safety, not a guarantee of healthy sampling.
    """
    def __init__(self, runtime: PolicyRuntime, *, max_actors: int = 16):
        self.runtime, self.max_actors = runtime, max_actors
        self.condition = threading.Condition()
        self.pending: dict[int, dict[str, Any]] = {}
        self.latest: dict[int, tuple[tuple[int, ...], int]] = {}
        self.results: dict[int, dict[str, Any]] = {}
        self.counters: Counter = Counter()
        self.closed = False
        self.worker = threading.Thread(target=self._work, name="body-policy", daemon=True)
        self.worker.start()

    def submit(self, request: dict[str, Any]) -> dict[str, Any]:
        if len(json.dumps(request, allow_nan=False).encode()) > MAX_REQUEST_BYTES:
            raise ValueError("request exceeds bounded mailbox")
        request = copy.deepcopy(request)
        identity = actor_identity(request["observation"])
        sequence, entity = request.get("stream_seq"), identity[2]
        if type(sequence) is not int or sequence <= 0:
            raise ValueError("invalid stream sequence")
        with self.condition:
            if self.closed:
                raise RuntimeError("mailbox closed")
            if entity not in self.latest and len(self.latest) >= self.max_actors:
                raise ValueError("mailbox actor capacity exceeded")
            previous = self.latest.get(entity)
            if previous is not None and previous[0] == identity and sequence < previous[1]:
                return {"accepted": False, "reason": "stale_sequence"}
            if previous is not None and previous[0] != identity and (identity[0], identity[1], identity[3]) < (previous[0][0], previous[0][1], previous[0][3]):
                return {"accepted": False, "reason": "stale_generation"}
            if entity in self.pending and self.pending[entity]["stream_seq"] != sequence:
                if actor_identity(self.pending[entity]["observation"]) == identity:
                    request["gap"] = True
                self.counters["coalesced"] += 1
            self.pending[entity] = request
            self.latest[entity] = (identity, sequence)
            self.results.pop(entity, None)
            self.condition.notify()
            return {"accepted": True, "reason": "queued"}

    def poll(self, entity: int) -> dict[str, Any] | None:
        with self.condition:
            return self.results.pop(entity, None)

    def _work(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.closed or self.pending)
                if self.closed:
                    return
                entity = next(iter(self.pending))
                request = self.pending.pop(entity)
            try:
                response = self.runtime.process(request)
            except (ValueError, KeyError, TypeError, RuntimeError) as error:
                response = {"status": "fallback", "reason": "invalid_request", "error": str(error)[:512],
                            "action_id": None, "model_processed": False}
            with self.condition:
                current = self.latest.get(entity)
                identity = actor_identity(request["observation"])
                if current != (identity, request["stream_seq"]):
                    self.counters["discarded_old_reply"] += 1
                else:
                    self.results[entity] = response
                self.condition.notify_all()

    def close(self):
        with self.condition:
            self.closed = True
            self.pending.clear()
            self.condition.notify_all()
        self.worker.join(timeout=5)
        if self.worker.is_alive():
            raise RuntimeError("inference worker did not finish within shutdown budget")
