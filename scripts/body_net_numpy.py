#!/usr/bin/env python3
"""NumPy reference for the P30 actor and its periodic decision path.

Version 1 is a JSON header line followed by contiguous little-endian float32
tensors. Tensor offsets count bytes from the first byte after the newline.
GRUCell gates follow PyTorch's reset, update, new order and reset-after formula.

Only NumPy and stdlib are required, including the existing stdlib feature packer.
The runtime omits TCP request-size limits, request checkpoint-SHA checks, the
16-actor capacity gate, duplicate-request caches and arbitration retry caches.
Duplicate stream sequences are explicitly unsupported. Timing, fingerprints,
service counters and execution-profile diagnostics are not reproduced.
"""
from __future__ import annotations

import copy
from dataclasses import dataclass
import math
from pathlib import Path
import re
from typing import Any

import numpy as np

from body_fast_features import pack_features
from body_trajectory_dataset import (ACTIONS, PERIODIC_FEATURE_VERSION,
                                     canonical_feature_names, policy_action_mask, strict_json)

IDENTITY_KEYS = ("session_epoch", "world_epoch", "entity_id", "control_generation")


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


class BodyNet:
    """Read actor-only weights and evaluate one step with explicit GRU state."""

    def __init__(self, path: Path | str):
        with Path(path).open("rb") as stream:
            line = stream.readline(1024 * 1024)
            _require(line.endswith(b"\n"), "bodynet has no bounded JSON header line")
            try:
                header = strict_json(line.decode("utf-8"))
            except (ValueError, UnicodeError) as error:
                raise ValueError(f"invalid bodynet JSON header: {error}") from error
            _require(isinstance(header, dict), "bodynet header must be an object")
            _require(header.get("format") == "gothic-body-net" and type(header.get("version")) is int
                     and header["version"] == 1,
                     "unsupported bodynet format/version")
            _require(header.get("byte_order") == "little", "unsupported bodynet byte order")
            _require(header.get("offset_origin") == "first byte after JSON header newline",
                     "unsupported bodynet offset origin")
            payload = stream.read()
        _require(type(header.get("data_nbytes")) is int and header["data_nbytes"] == len(payload),
                 "bodynet data_nbytes differs from payload length (truncated or extra data)")
        _require(header.get("actions") == list(ACTIONS), "bodynet action order mismatch")
        _require(header.get("feature_version") == PERIODIC_FEATURE_VERSION,
                 "unsupported bodynet feature version")
        names = canonical_feature_names(PERIODIC_FEATURE_VERSION)
        _require(len(names) == 184 and header.get("feature_names") == names,
                 "bodynet feature order mismatch")
        _require(isinstance(header.get("checkpoint_sha256"), str) and
                 re.fullmatch(r"[0-9a-f]{64}", header["checkpoint_sha256"]) is not None,
                 "invalid bodynet checkpoint SHA256")
        entries = header.get("tensors")
        _require(isinstance(entries, list), "bodynet tensor list required")
        tensors = {}
        offset = 0
        for entry in entries:
            _require(isinstance(entry, dict), "invalid bodynet tensor descriptor")
            name, shape = entry.get("name"), entry.get("shape")
            _require(isinstance(name, str) and name not in tensors, "invalid/duplicate bodynet tensor name")
            _require(isinstance(shape, list) and bool(shape) and
                     all(type(size) is int and size > 0 for size in shape), f"invalid bodynet shape: {name}")
            _require(entry.get("dtype") == "float32", f"unsupported bodynet dtype: {name}")
            nbytes = math.prod(shape) * 4
            _require(type(entry.get("offset")) is int and entry["offset"] == offset,
                     f"invalid bodynet tensor offset: {name}")
            _require(offset + nbytes <= len(payload), f"truncated bodynet tensor: {name}")
            if "nbytes" in entry:
                _require(entry["nbytes"] == nbytes, f"invalid bodynet tensor nbytes: {name}")
            value = np.frombuffer(payload, dtype="<f4", count=nbytes // 4, offset=offset).reshape(shape).copy()
            _require(bool(np.isfinite(value).all()), f"nonfinite bodynet tensor: {name}")
            tensors[name] = value
            offset += nbytes
        _require(offset == len(payload), "bodynet data_nbytes differs from tensor byte count")
        _require("encoder.0.bias" in tensors and "head.0.bias" in tensors,
                 "bodynet encoder/head tensors missing")
        encoder, head = tensors["encoder.0.bias"].size, tensors["head.0.bias"].size
        shapes = {"encoder.0.weight": (encoder, 184), "encoder.0.bias": (encoder,),
                  "memory.weight_ih": (768, encoder), "memory.weight_hh": (768, 256),
                  "memory.bias_ih": (768,), "memory.bias_hh": (768,),
                  "head.0.weight": (head, 256), "head.0.bias": (head,),
                  "head.2.weight": (12, head), "head.2.bias": (12,),
                  "normalizer.mean": (184,), "normalizer.std": (184,)}
        _require(set(tensors) == set(shapes), "bodynet actor tensor names mismatch")
        for name, shape in shapes.items():
            _require(tensors[name].shape == shape, f"bodynet tensor shape mismatch: {name}")
        _require(bool((tensors["normalizer.std"] > 0).all()), "bodynet normalizer std must be positive")
        self.header, self.tensors = header, tensors
        self.normalizer = {"feature_version": PERIODIC_FEATURE_VERSION, "feature_names": names,
                           "mean": dict(zip(names, tensors["normalizer.mean"].tolist())),
                           "std": dict(zip(names, tensors["normalizer.std"].tolist()))}

    def step(self, features, mask, state=None) -> tuple[np.ndarray, np.ndarray]:
        values = np.asarray(features, dtype=np.float32)
        allowed = np.asarray(mask)
        hidden = np.zeros(256, dtype=np.float32) if state is None else np.asarray(state, dtype=np.float32)
        _require(values.shape == (184,) and bool(np.isfinite(values).all()), "expected 184 finite body features")
        _require(allowed.shape == (12,) and allowed.dtype == np.bool_ and bool(allowed.any()),
                 "expected nonempty boolean body action mask")
        _require(hidden.shape == (256,) and bool(np.isfinite(hidden).all()), "expected 256 finite GRU state values")
        weights = self.tensors

        def linear(value, name):
            return weights[name + ".weight"] @ value + weights[name + ".bias"]

        def sigmoid(value):
            # Evaluate sigmoid without overflow, retaining float32 arithmetic.
            exp = np.exp(-np.abs(value))
            return np.where(value >= 0, np.float32(1) / (np.float32(1) + exp), exp / (np.float32(1) + exp))

        encoded = np.maximum(linear(values, "encoder.0"), np.float32(0))
        input_gates = weights["memory.weight_ih"] @ encoded + weights["memory.bias_ih"]
        hidden_gates = weights["memory.weight_hh"] @ hidden + weights["memory.bias_hh"]
        ir, iz, inn = np.split(input_gates, 3)
        hr, hz, hn = np.split(hidden_gates, 3)
        reset, update = sigmoid(ir + hr), sigmoid(iz + hz)
        new = np.tanh(inn + reset * hn)
        hidden = new + update * (hidden - new)
        logits = linear(np.maximum(linear(hidden, "head.0"), np.float32(0)), "head.2")
        logits = np.where(allowed, logits, np.finfo(np.float32).min)
        return logits, hidden


def actor_identity(row: dict[str, Any]) -> tuple[int, ...]:
    identity = tuple(row.get(key) for key in IDENTITY_KEYS)
    _require(all(type(value) is int and value >= 0 for value in identity), "invalid observation identity")
    return identity


@dataclass
class ActorState:
    sequence: int = 0
    game_ms: int | None = None
    hidden: Any = None
    scores: Any = None
    response: Any = None
    observation: Any = None


class NumpyRuntime:
    """Periodic P30 memory and arbitration, independent of the Torch runtime."""

    def __init__(self, network: BodyNet, *, max_age_game_ms: int = 100, max_dt_game_ms: int = 100,
                 allow_stale_memory_only: bool = False):
        self.network = network
        self.checkpoint_sha256 = network.header["checkpoint_sha256"]
        self.normalizer = network.normalizer
        self.max_age_game_ms, self.max_dt_game_ms = max_age_game_ms, max_dt_game_ms
        self.allow_stale_memory_only = allow_stale_memory_only
        self.states: dict[tuple[int, ...], ActorState] = {}
        self.active: dict[int, tuple[int, ...]] = {}
        self.actor_clocks: dict[int, tuple[int, int, int]] = {}
        self.expected_epochs = None
        self.initial_game_ms = None
        self.episode_closed = False
        self.last_features = None

    def begin_episode(self, session_epoch, world_epoch, *, initial_game_ms=0):
        _require(all(type(value) is int and value >= 0 for value in (session_epoch, world_epoch)),
                 "invalid episode epochs")
        _require(type(initial_game_ms) is int and initial_game_ms >= 0, "invalid episode game-time boundary")
        self.states.clear()
        self.active.clear()
        self.actor_clocks.clear()
        self.expected_epochs = (session_epoch, world_epoch)
        self.initial_game_ms, self.episode_closed = initial_game_ms, False

    def end_episode(self, session_epoch, world_epoch):
        if self.expected_epochs != (session_epoch, world_epoch):
            return {"accepted": False, "reason": "stale_episode_end", "hidden_reset": False, "model_processed": False}
        self.states.clear()
        self.active.clear()
        self.actor_clocks.clear()
        self.expected_epochs, self.initial_game_ms, self.episode_closed = None, None, True
        return {"accepted": True, "reason": "episode_ended", "hidden_reset": True, "model_processed": False}

    def reset_actor(self, identity, reason="explicit_reset"):
        self.states.pop(identity, None)

    def process(self, request, *, now_game_ms=None):
        row = request.get("observation")
        _require(isinstance(row, dict), "observation object required")
        identity = actor_identity(row)
        sequence = request.get("stream_seq")
        game_ms, native_sequence = row.get("t_game_ms"), row.get("observation_seq")
        _require(all(type(value) is int and value >= 0 for value in (sequence, game_ms, native_sequence)) and sequence > 0,
                 "invalid observation sequence or game time")
        now_game_ms = request.get("now_game_ms", game_ms) if now_game_ms is None else now_game_ms
        _require(type(now_game_ms) is int, "now_game_ms must come from the engine clock")
        base = {"action_id": None, "selected_action_id": None, "model_processed": False,
                "hidden_reset": False, "status": "fallback", "observation_seq": native_sequence,
                "stream_seq": sequence, "t_game_ms": game_ms}

        def fallback(reason, reset=False):
            if reset and identity in self.states:
                self.states[identity].hidden = None
                self.states[identity].scores = None
            return {**base, "reason": reason, "hidden_reset": reset}

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
            state = ActorState()
            self.states[identity] = state
            self.active[identity[2]] = identity
            base["hidden_reset"] = True
        if sequence == state.sequence:
            raise ValueError("NumPy body runtime does not support duplicate stream sequences")
        if sequence < state.sequence:
            return fallback("stale_sequence")
        clock = self.actor_clocks.get(identity[2])
        previous_game = clock[2] if clock is not None and clock[:2] == identity[:2] else None
        if previous_game is not None and game_ms < previous_game or now_game_ms < game_ms:
            return fallback("clock_regression", True)
        boundary = self.initial_game_ms if self.initial_game_ms is not None else request.get("initial_game_ms")
        if previous_game is None and (type(boundary) is not int or boundary < 0 or boundary > game_ms):
            return fallback("missing_reset_time_boundary", True)
        dt = game_ms - boundary if previous_game is None else game_ms - previous_game
        base["sequence_dt_game_ms"] = dt
        gap = sequence != state.sequence + 1
        bucket_gap = previous_game is not None and game_ms // 50 > previous_game // 50 + 1
        memory_only = request.get("memory_only") is True
        valid_memory_only = memory_only and (row.get("can_decide") is False or request.get("command_enabled") is False)
        check_age = not (self.allow_stale_memory_only and valid_memory_only)
        reason = ("unexpected_gap" if gap or request.get("gap") is True else
                  "unexpected_periodic_gap" if bucket_gap else
                  "observation_too_old" if check_age and now_game_ms - game_ms > self.max_age_game_ms else
                  "unexpected_time_gap" if previous_game is not None and dt > self.max_dt_game_ms else None)
        if reason:
            state.sequence, state.game_ms = sequence, game_ms
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
        try:
            model_mask = policy_action_mask(row)
        except (ValueError, TypeError, KeyError):
            return fallback("invalid_action_mask", True)
        if not any(model_mask):
            return fallback("invalid_action_mask", True)
        if eligible and not any(model_mask[1:]):
            return fallback("no_available_primitive", True)
        try:
            features = pack_features(row, self.normalizer, sequence_dt_game_ms=dt)
            if any(not math.isfinite(value) for value in features):
                return fallback("invalid_features", True)
            self.last_features = features
            logits, hidden = self.network.step(features, model_mask, state.hidden)
            if len(logits) != 12 or not bool(np.isfinite(logits).all()):
                return fallback("invalid_logits", True)
            state.hidden, state.scores = hidden, logits
            action = max((index for index, allowed in enumerate(model_mask) if allowed), key=lambda index: logits[index])
            response = {**base, "status": "processed", "reason": "decision" if eligible and action else "locked",
                        "action_id": action if eligible and action else None, "selected_action_id": action,
                        "model_processed": True, "features_sha256": None, "feature_count": len(features),
                        "logits": logits.tolist(), "effective_action_mask": model_mask,
                        "raw_action_mask": mask, "memory_only": memory_only, "new_decision": bool(eligible and action)}
            state.sequence, state.game_ms, state.response = sequence, game_ms, response
            state.observation = copy.deepcopy(row)
            self.actor_clocks[identity[2]] = (*identity[:2], game_ms)
            return response.copy()
        except (ValueError, TypeError, KeyError, RuntimeError, OverflowError) as error:
            return {**fallback("backend_error", True), "error": str(error)[:512]}

    def arbitrate(self, row, *, stream_seq, max_anchor_age_game_ms=67):
        identity = actor_identity(row)
        state = self.states.get(identity)
        base = {"action_id": None, "model_processed": False, "hidden_reset": False, "status": "fallback"}
        if state is None or state.scores is None or state.sequence != stream_seq:
            return {**base, "reason": "no_current_periodic_state"}
        if state.response.get("memory_only") is True:
            return {**base, "status": "locked", "reason": "memory_only"}
        now = row.get("t_game_ms")
        if type(now) is not int or not 0 <= now - state.game_ms <= max_anchor_age_game_ms:
            return {**base, "reason": "periodic_anchor_too_old"}
        if (row.get("observation_seq") != state.response["observation_seq"] or now != state.game_ms or
                row.get("periodic") is not True or row != state.observation):
            return {**base, "reason": "periodic_anchor_mismatch"}
        if row.get("can_decide") is not True or row.get("action_mask_valid") is not True:
            return {**base, "status": "locked", "reason": "locked"}
        mask = row.get("action_mask")
        if (not isinstance(mask, list) or len(mask) != 12 or any(type(value) is not bool for value in mask) or
                not any(mask[1:])):
            return {**base, "reason": "invalid_arbitration_mask"}
        try:
            model_mask = policy_action_mask(row)
        except (ValueError, KeyError, TypeError):
            return {**base, "reason": "invalid_arbitration_mask"}
        action = max((index for index in range(1, 12) if model_mask[index]), key=lambda index: state.scores[index])
        return {**base, "status": "decision", "reason": "decision", "action_id": action,
                "effective_action_mask": model_mask, "raw_action_mask": mask}
