"""Ordered TCP JSONL transport with one recurrent episode per connection."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import socket
import socketserver
import sys
import threading
from typing import Callable

from .runtime import MAX_REQUEST_BYTES, PolicyRuntime, TorchBackend, actor_identity


def configure_serve_net_parser(subparsers) -> None:
    parser = subparsers.add_parser("serve-net", help="TCP JSONL policy runtime with isolated client episodes")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=47835)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--inference-cpu-threads", type=int, choices=(1, 2), default=2)
    parser.add_argument("--max-actors", type=int, default=16)
    parser.add_argument("--max-age-game-ms", type=int, default=100)
    parser.add_argument("--max-dt-game-ms", type=int, default=100)
    parser.add_argument("--allow-stale-memory-only", action=argparse.BooleanOptionalAction, default=True,
                        help="update memory for old rows when commands are disabled (default: enabled)")


def invalid_request(error: Exception, request_id=None) -> dict:
    return {"ok": False, "status": "fallback", "reason": "invalid_request", "action_id": None,
            "model_processed": False, "error": str(error)[:512], "id": request_id, "schema_version": 1}


class PolicySession:
    """The synchronous gate adapter; neither drops nor coalesces periodic rows.

    Only local elapsed-time diagnostics are omitted from the process receipt so
    that recorded network and in-process receipts can be compared exactly.
    """
    def __init__(self, runtime: PolicyRuntime):
        self.runtime = runtime
        # Retain only the current identity/sequence per actor, bounding memory
        # across arbitrarily many control generations.
        self.stream: dict[int, tuple[tuple[int, ...], int]] = {}

    def handle(self, request) -> dict:
        request_id = request.get("id") if isinstance(request, dict) else None
        try:
            if not isinstance(request, dict):
                raise ValueError("object request required")
            command = request.get("cmd")
            if command == "reset":
                self.runtime.begin_episode(request["session_epoch"], request["world_epoch"],
                                           initial_game_ms=request["initial_game_ms"])
                self.stream.clear()
                response = {"reason": "episode_reset", "hidden_reset": True}
                if request.get("include_capabilities") is True:
                    response["memory_only_step"] = True
            elif command == "step":
                response = self.step(request)
            elif command == "status":
                response = self.runtime.status()
            elif command == "end_episode":
                epochs = [request[key] for key in ("session_epoch", "world_epoch")]
                if any(type(value) is not int or value < 0 for value in epochs):
                    raise ValueError("invalid episode epochs")
                response = self.runtime.end_episode(*epochs)
                if response["accepted"]:
                    self.stream.clear()
            elif command == "close":
                response = {"reason": "closed"}
            else:
                raise ValueError("unknown service command")
            return {"ok": True, **response, "id": request_id, "schema_version": 1}
        except Exception as error:
            # Nested JSON fields can also fail inside policy helpers. Contain
            # every request failure here so its ID and queued replies survive.
            return invalid_request(error, request_id)

    def step(self, request: dict) -> dict:
        row = request.get("observation")
        if not isinstance(row, dict):
            raise ValueError("observation object required")
        identity = actor_identity(row)
        entity = identity[2]
        now = request.get("now_game_ms")
        if type(now) is not int or now < 0:
            raise ValueError("now_game_ms must come from the engine clock")
        if entity not in self.stream and len(self.stream) >= self.runtime.max_actors:
            raise ValueError("actor stream capacity exceeded")
        previous = self.stream.get(entity)
        sequence = previous[1] + 1 if previous is not None and previous[0] == identity else 1
        eligible = (row.get("can_decide") is True and row.get("action_mask_valid") is True and
                    request.get("memory_only") is not True)
        processed = self.runtime.process({"stream_seq": sequence, "observation": row,
            "source": "periodic_observer", "now_game_ms": now,
            "checkpoint_sha256": self.runtime.checkpoint_sha256,
            "command_enabled": eligible, "memory_only": not eligible})
        # A new generation owns a fresh recurrent sequence. Rejected old
        # generations/epochs must not displace the current actor's counter.
        if self.runtime.active.get(entity) == identity:
            self.stream[entity] = (identity, sequence)
        arbitration = None
        # A process fallback must keep its original cause, and cannot use old scores.
        if processed["status"] != "fallback" and eligible:
            arbitration = self.runtime.arbitrate(row, stream_seq=sequence)
        result = arbitration or processed
        return {"process": {key: value for key, value in processed.items()
                            if key not in ("inference_ms", "backend_timing")},
                "arbitration": arbitration, "action_id": result["action_id"],
                "status": result["status"] if arbitration or result["status"] == "fallback" else "locked",
                "reason": result["reason"]}


def _reject_constant(value):
    raise ValueError("non-finite JSON number")


def _finite_float(value):
    number = float(value)
    if not math.isfinite(number):
        raise ValueError("non-finite JSON number")
    return number


class _PolicyHandler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.server.inference_lock:
            session = PolicySession(self.server.runtime_factory())
        try:
            while True:
                line = self.rfile.readline(MAX_REQUEST_BYTES + 1)
                if not line:
                    return
                request = None
                oversized = len(line) > MAX_REQUEST_BYTES
                try:
                    if oversized or not line.endswith(b"\n"):
                        raise ValueError("JSONL request exceeds bounded reader or lacks newline")
                    request = json.loads(line.decode("utf-8"), parse_constant=_reject_constant,
                                         parse_float=_finite_float)
                    # Weights are shared, but all hidden state belongs to this session.
                    # Serial forwards also bound aggregate CPU use across connections.
                    with self.server.inference_lock:
                        response = session.handle(request)
                except (ValueError, UnicodeError, RecursionError) as error:
                    response = invalid_request(error)
                try:
                    encoded = json.dumps(response, allow_nan=False, separators=(",", ":")).encode()
                except (ValueError, TypeError, RecursionError) as error:
                    encoded = json.dumps(invalid_request(error), separators=(",", ":")).encode()
                self.wfile.write(encoded + b"\n")
                self.wfile.flush()
                if isinstance(request, dict) and request.get("cmd") == "close":
                    return
                if oversized:
                    # Discard the remainder in bounded chunks, with exactly one reply
                    # for the rejected line. The next line retains its own ID/order.
                    while not line.endswith(b"\n"):
                        line = self.rfile.readline(MAX_REQUEST_BYTES + 1)
                        if not line:
                            return
        except (OSError, ConnectionError):
            # Disconnects affect only this client's episode.
            return


class PolicyTCPServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], runtime_factory: Callable[[], PolicyRuntime]):
        self.runtime_factory = runtime_factory
        self.inference_lock = threading.Lock()
        super().__init__(address, _PolicyHandler)


def serve_net_main(args: argparse.Namespace) -> int:
    if sys.platform != "linux":
        raise RuntimeError("models run only on dev-ubuntu, never on the Mac")
    backend = TorchBackend(args.checkpoint, device=args.device,
                           inference_cpu_threads=args.inference_cpu_threads)
    checkpoint_sha256 = hashlib.sha256(args.checkpoint.read_bytes()).hexdigest()

    def runtime_factory():
        return PolicyRuntime(backend=backend, checkpoint_sha256=checkpoint_sha256,
                             max_actors=args.max_actors, max_age_game_ms=args.max_age_game_ms,
                             max_dt_game_ms=args.max_dt_game_ms,
                             inference_cpu_threads=args.inference_cpu_threads,
                             allow_stale_memory_only=args.allow_stale_memory_only)

    with PolicyTCPServer((args.host, args.port), runtime_factory) as server:
        print(f"gothic-body listening on {args.host}:{server.server_address[1]}", file=sys.stderr, flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0
