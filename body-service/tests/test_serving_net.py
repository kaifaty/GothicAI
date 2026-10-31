"""TCP framing and episode parity with a synthetic, model-free runtime."""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from copy import deepcopy
import json
from pathlib import Path
import socket
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from gothic_body.runtime import MAX_REQUEST_BYTES, PolicyRuntime, shared_feature_packer
from gothic_body import serving_net
from gothic_body.serving_net import PolicyTCPServer, configure_serve_net_parser


class CounterBackend:
    metadata = {"sampling_version": "periodic-v1"}

    def __init__(self):
        self.calls = []

    def step(self, features, mask, state):
        self.calls.append((features, mask, state))
        # process may choose CONTINUE, while arbitrate must choose a primitive.
        return [20.0] + [float(index) for index in range(1, 12)], (state or 0) + 1


def make_runtime(**kwargs):
    return PolicyRuntime(
        backend=CounterBackend(), checkpoint_sha256="frozen",
        normalizer={"feature_names": ["dt"]},
        feature_packer=lambda row, normalizer, sequence_dt_game_ms: [float(sequence_dt_game_ms)],
        **kwargs,
    )


def observation(sequence=1, *, game_ms=None, entity=1, world=7, eligible=True):
    return {
        "session_epoch": 4, "world_epoch": world, "entity_id": entity,
        "control_generation": 1, "observation_seq": sequence * 10 + entity,
        "t_game_ms": 100 + sequence * 50 if game_ms is None else game_ms,
        "periodic": True, "sample_period_ms": 50,
        "phase": "post_simulation_post_animation", "can_decide": eligible,
        "native_decision_eligible": eligible, "action_mask_valid": True,
        "action_mask": [True] * 12, "availability_mask": [True] * 12,
    }


def step(row, request_id, *, now_game_ms=None):
    return {"id": request_id, "cmd": "step", "observation": row,
            "now_game_ms": row["t_game_ms"] if now_game_ms is None else now_game_ms}


def reset(request_id="reset", *, world=7, initial_game_ms=100):
    return {"id": request_id, "cmd": "reset", "session_epoch": 4,
            "world_epoch": world, "initial_game_ms": initial_game_ms}


class DirectRuntime:
    """Independent reference driver: call process and arbitrate without TCP code."""

    def __init__(self):
        self.policy = make_runtime()
        self.streams = Counter()

    def response(self, request):
        command = request["cmd"]
        if command == "reset":
            self.policy.begin_episode(request["session_epoch"], request["world_epoch"],
                                      initial_game_ms=request["initial_game_ms"])
            self.streams.clear()
            result = {"reason": "episode_reset", "hidden_reset": True}
        elif command == "step":
            row = request["observation"]
            identity = tuple(row[key] for key in ("session_epoch", "world_epoch", "entity_id", "control_generation"))
            self.streams[identity] += 1
            sequence = self.streams[identity]
            eligible = row.get("can_decide") is True and row.get("action_mask_valid") is True
            processed = self.policy.process({
                "stream_seq": sequence, "observation": deepcopy(row),
                "now_game_ms": request["now_game_ms"], "source": "periodic_observer",
                "checkpoint_sha256": self.policy.checkpoint_sha256,
                "command_enabled": eligible, "memory_only": not eligible,
            })
            processed.pop("inference_ms", None)
            processed.pop("backend_timing", None)
            arbitration = None
            if processed["status"] == "fallback":
                status, action, reason = "fallback", None, processed["reason"]
            elif eligible:
                arbitration = self.policy.arbitrate(row, stream_seq=sequence)
                status, action, reason = (arbitration[key] for key in ("status", "action_id", "reason"))
            else:
                status, action, reason = "locked", None, processed["reason"]
            result = {"process": processed, "arbitration": arbitration, "status": status,
                      "action_id": action, "reason": reason}
        elif command == "status":
            result = self.policy.status()
        elif command == "end_episode":
            result = self.policy.end_episode(request["session_epoch"], request["world_epoch"])
            if result["accepted"]:
                self.streams.clear()
        elif command == "close":
            result = {"reason": "closed"}
        else:
            raise AssertionError(command)
        return {"ok": True, **result, "id": request["id"], "schema_version": 1}


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("localhost", port), timeout=5)
        self.reader = self.socket.makefile("rb")

    def send(self, request):
        self.socket.sendall(json.dumps(request, separators=(",", ":"), allow_nan=False).encode() + b"\n")

    def receive(self):
        line = self.reader.readline(MAX_REQUEST_BYTES * 4)
        assert line.endswith(b"\n"), line
        return json.loads(line)

    def exchange(self, request):
        self.send(request)
        return self.receive()

    def close(self):
        self.reader.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


@contextmanager
def running_server(runtime_factory=make_runtime):
    server = PolicyTCPServer(("localhost", 0), runtime_factory)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        assert not thread.is_alive()


@pytest.mark.parametrize("age_ms", (0, 250))
def test_step_memory_only_forces_no_command_for_eligible_row_and_retains_order(age_ms):
    policy = make_runtime(allow_stale_memory_only=True)
    old = observation()
    request = {**step(old, "memory", now_game_ms=old["t_game_ms"] + age_ms), "memory_only": True}
    with running_server(lambda: policy) as port, Client(port) as client:
        assert client.exchange({**reset(), "include_capabilities": True})["memory_only_step"] is True
        response = client.exchange(request)
        assert response["ok"] and response["status"] == "locked"
        assert response["action_id"] is None and response["arbitration"] is None
        assert response["process"]["memory_only"] and response["process"]["model_processed"]
        assert response["process"]["stream_seq"] == 1
        fresh = client.exchange(step(observation(2), "fresh"))
    assert fresh["status"] == "decision" and fresh["action_id"] == 11
    assert fresh["process"]["stream_seq"] == 2
    assert [call[2] for call in policy.backend.calls] == [None, 1]
    assert old["can_decide"] is True


@pytest.mark.parametrize("flags,expected", (([], True), (["--allow-stale-memory-only"], True),
                                           (["--no-allow-stale-memory-only"], False)))
def test_serve_net_cli_controls_memory_only_age_bypass(tmp_path, monkeypatch, flags, expected):
    parser = argparse.ArgumentParser()
    configure_serve_net_parser(parser.add_subparsers(dest="command"))
    checkpoint = tmp_path / "placeholder.pt"
    checkpoint.write_bytes(b"synthetic-checkpoint")
    args = parser.parse_args(["serve-net", "--checkpoint", str(checkpoint), "--host", "localhost", *flags])
    created = []

    def runtime_factory(**kwargs):
        created.append(kwargs)
        return object()

    class Server:
        def __init__(self, address, factory):
            self.server_address = address
            factory()

        def __enter__(self):
            return self

        def __exit__(self, *_):
            pass

        def serve_forever(self):
            pass

    monkeypatch.setattr(serving_net.sys, "platform", "linux")
    monkeypatch.setattr(serving_net, "TorchBackend", lambda *args, **kwargs: CounterBackend())
    monkeypatch.setattr(serving_net, "PolicyRuntime", runtime_factory)
    monkeypatch.setattr(serving_net, "PolicyTCPServer", Server)
    assert serving_net.serve_net_main(args) == 0
    assert len(created) == 1 and created[0]["allow_stale_memory_only"] is expected


def test_network_replays_runtime_field_for_field_and_preserves_pipeline_order():
    continue_preferred = observation()
    continue_preferred["native_decision_eligible"] = False
    empty_mask = observation(2, eligible=False)
    empty_mask.update(action_mask=[False] * 12, availability_mask=[False] * 12)
    invalid_gate = observation(3)
    invalid_gate["action_mask_valid"] = False
    requests = [
        reset(), step(continue_preferred, "decision"), step(empty_mask, "empty-memory"),
        step(invalid_gate, "invalid-gate-memory"), step(observation(entity=2), "other-actor"),
        step(observation(4), "stale", now_game_ms=450),
        step(observation(5, game_ms=400), "gap"), step(observation(6, game_ms=450), "recovered"),
        {"cmd": "status", "id": "status"},
        {"cmd": "end_episode", "id": "end", "session_epoch": 4, "world_epoch": 7},
        step(observation(7, game_ms=500), "after-end"),
        reset("new-reset", world=8, initial_game_ms=117),
        step(observation(world=8), "new-episode"), {"cmd": "status", "id": "new-status"},
        {"cmd": "close", "id": "close"},
    ]
    direct = DirectRuntime()
    expected = [direct.response(request) for request in requests]
    with running_server() as port, Client(port) as client:
        for request in requests:
            client.send(request)
        responses = [client.receive() for _ in requests]
        assert client.reader.readline() == b""
    assert responses == expected
    assert [response["id"] for response in responses] == [request["id"] for request in requests]
    assert responses[1]["process"]["selected_action_id"] == 0
    assert responses[1]["action_id"] == 11
    assert responses[2]["process"]["memory_only"] and responses[2]["process"]["model_processed"]
    assert responses[2]["arbitration"] is None and responses[2]["action_id"] is None
    assert responses[3]["process"]["memory_only"] and responses[3]["process"]["model_processed"]
    assert responses[4]["process"]["stream_seq"] == 1
    assert responses[5]["status"] == "fallback" and responses[5]["reason"] == "observation_too_old"
    assert responses[6]["status"] == "fallback" and responses[6]["reason"] == "unexpected_periodic_gap"
    assert responses[7]["status"] == "decision"
    assert responses[10]["reason"] == "episode_ended" and responses[10]["action_id"] is None
    assert responses[12]["process"]["stream_seq"] == 1
    assert responses[12]["process"]["sequence_dt_game_ms"] == 33


def test_generation_change_starts_new_sequence_without_stale_rows_resetting_it():
    rows = [observation(index, game_ms=game_ms) for index, game_ms in
            enumerate((150, 200, 200, 250, 250, 300), 1)]
    for row, generation in zip(rows, (1, 2, 1, 2, 2, 2)):
        row["control_generation"] = generation
    rows[4]["world_epoch"] = 6
    # Explicit runtime inputs are independent of either adapter's counters.
    sequences = (1, 1, 1, 2, 1, 3)
    direct = make_runtime()
    direct.begin_episode(4, 7, initial_game_ms=100)
    expected = []
    for row, sequence in zip(rows, sequences):
        eligible = row["can_decide"] and row["action_mask_valid"]
        processed = direct.process({"stream_seq": sequence, "observation": row,
                                   "now_game_ms": row["t_game_ms"], "source": "periodic_observer",
                                   "checkpoint_sha256": direct.checkpoint_sha256,
                                   "command_enabled": eligible, "memory_only": not eligible})
        processed.pop("inference_ms", None)
        processed.pop("backend_timing", None)
        arbitration = direct.arbitrate(row, stream_seq=sequence) if processed["status"] != "fallback" else None
        expected.append((processed, arbitration))
    requests = [reset()] + [step(row, index) for index, row in enumerate(rows, 1)]
    with running_server() as port, Client(port) as client:
        for request in requests:
            client.send(request)
        assert client.receive()["reason"] == "episode_reset"
        responses = [client.receive() for _ in rows]
    assert [(response["process"], response["arbitration"]) for response in responses] == expected
    assert [response["id"] for response in responses] == list(range(1, 7))
    for index in (0, 1, 3, 5):
        assert responses[index]["status"] == "decision" and responses[index]["process"]["model_processed"]
    assert responses[1]["process"]["hidden_reset"] and responses[1]["process"]["sequence_dt_game_ms"] == 50
    assert responses[2]["reason"] == "stale_generation" and responses[4]["reason"] == "stale_epoch"
    assert [responses[index]["process"]["stream_seq"] for index in (0, 1, 3, 5)] == [1, 1, 2, 3]
    assert [call[2] for call in direct.backend.calls] == [None, None, 1, 2]


def test_clients_have_independent_runtime_episodes_with_simultaneous_requests():
    barrier = threading.Barrier(2)

    def client_episode(port, world):
        direct = DirectRuntime()
        requests = [reset(world=world), step(observation(world=world), "first"),
                    step(observation(2, world=world), "second"), {"cmd": "status", "id": "status"}]
        with Client(port) as client:
            actual = [client.exchange(requests[0])]
            barrier.wait(timeout=3)
            actual.extend(client.exchange(request) for request in requests[1:])
        assert actual == [direct.response(request) for request in requests]
        assert actual[1]["process"]["hidden_reset"]
        assert actual[3]["actors"] == 1 and actual[3]["counters"]["processed"] == 2
        return actual

    with running_server() as port:
        with ThreadPoolExecutor(max_workers=2) as executor:
            results = [executor.submit(client_episode, port, world) for world in (7, 8)]
            for result in results:
                result.result(timeout=10)


@pytest.mark.parametrize("raw,request_id", [
    (b'{"cmd":\n', None), (b'[]\n', None), (b'\xff\n', None),
    (b'{"cmd":"status","value":NaN}\n', None),
    (b'{"cmd":"status","id":1e999}\n', None),
    (b'{"cmd":"unknown","id":"bad"}\n', "bad"),
    (b'{"cmd":"step","id":"bad"}\n', "bad"),
    (b'{"cmd":"reset","session_epoch":true,"world_epoch":7,"initial_game_ms":0}\n', None),
])
def test_invalid_request_is_bounded_fallback_and_connection_recovers(raw, request_id):
    with running_server() as port, Client(port) as malformed, Client(port) as other:
        malformed.socket.sendall(raw)
        response = malformed.receive()
        assert response["ok"] is False and response["status"] == "fallback"
        assert response["reason"] == "invalid_request" and response["action_id"] is None
        assert response["model_processed"] is False and response["schema_version"] == 1
        assert response["id"] == request_id
        assert len(response["error"]) <= 512
        assert malformed.exchange(reset())["reason"] == "episode_reset"
        assert malformed.exchange(step(observation(), "recovered"))["status"] == "decision"
        assert other.exchange(reset())["ok"]
        assert other.exchange(step(observation(), "other-client"))["status"] == "decision"


def test_real_feature_packer_error_keeps_request_id_and_queued_replies():
    packer = shared_feature_packer()
    from body_trajectory_dataset import PERIODIC_FEATURE_VERSION, canonical_feature_names

    names = canonical_feature_names()
    normalizer = {"feature_version": PERIODIC_FEATURE_VERSION, "feature_names": names,
                  "mean": dict.fromkeys(names, 0.0), "std": dict.fromkeys(names, 1.0)}

    def real_packer_runtime():
        return PolicyRuntime(backend=CounterBackend(), checkpoint_sha256="frozen",
                             normalizer=normalizer, feature_packer=packer)

    valid = {**observation(), "schema_version": 1, "sim_step": 1, "dt_game_ms": 50,
             "mask_kind": "shared_melee_resolver", "self": {}, "target": {}}
    invalid = {**valid, "target": None}
    # Reproduce the actual helper exception without substituting a failing stub.
    with pytest.raises(AttributeError):
        packer(invalid, normalizer, sequence_dt_game_ms=50)
    requests = [reset(), step(invalid, "null-target"), {"cmd": "status", "id": "queued-status"},
                reset("recovery-reset"), step(valid, "valid-row"), {"cmd": "close", "id": "close"}]
    with running_server(real_packer_runtime) as port, Client(port) as client:
        client.socket.sendall(b"".join(json.dumps(request).encode() + b"\n" for request in requests))
        responses = [client.receive() for _ in requests]
        assert client.reader.readline() == b""
    assert [response["id"] for response in responses] == [request["id"] for request in requests]
    rejected = responses[1]
    assert rejected["ok"] is False and rejected["status"] == "fallback"
    assert rejected["reason"] == "invalid_request" and rejected["action_id"] is None
    assert rejected["model_processed"] is False and "NoneType" in rejected["error"]
    assert responses[2]["ok"] and responses[2]["counters"].get("processed", 0) == 0
    assert responses[3]["reason"] == "episode_reset"
    assert responses[4]["status"] == "decision" and responses[4]["process"]["model_processed"]
    assert responses[5]["reason"] == "closed"


def test_reader_limits_raw_bytes_drains_oversized_line_and_accepts_exact_limit():
    # Character count is below the limit; UTF-8 byte count is above it.
    oversized = b'{"cmd":"status","padding":"' + ("я" * (MAX_REQUEST_BYTES // 2)).encode() + b'"}\n'
    assert len(oversized) > MAX_REQUEST_BYTES
    assert len(oversized.decode()) < MAX_REQUEST_BYTES
    status = {"cmd": "status", "id": "after-oversize"}
    base = b'{"cmd":"status","id":"exact-limit","padding":""}\n'
    exact_limit = base[:-3] + b"x" * (MAX_REQUEST_BYTES - len(base)) + base[-3:]
    assert len(exact_limit) == MAX_REQUEST_BYTES
    with running_server() as port, Client(port) as client:
        client.socket.sendall(oversized + json.dumps(status).encode() + b"\n" + exact_limit)
        rejected, recovered, boundary = [client.receive() for _ in range(3)]
        assert rejected["ok"] is False and rejected["reason"] == "invalid_request"
        assert rejected["action_id"] is None
        assert recovered["ok"] and recovered["id"] == "after-oversize"
        assert boundary["ok"] and boundary["id"] == "exact-limit"


def test_incomplete_disconnect_does_not_break_other_clients_or_listener():
    with running_server() as port, Client(port) as survivor:
        with Client(port) as disconnected:
            disconnected.socket.sendall(b'{"cmd":"step","observation":')
        with Client(port) as newcomer:
            assert newcomer.exchange(reset())["ok"]
            assert newcomer.exchange(step(observation(), "newcomer"))["status"] == "decision"
        assert survivor.exchange(reset())["ok"]
        assert survivor.exchange(step(observation(), "survivor"))["status"] == "decision"
