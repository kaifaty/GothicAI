#!/usr/bin/env python3
"""Bounded real IPC backlog probe; no model, paused clock or Python physics."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time

from body_arena_env import ArenaInfrastructureError, ArenaRunner, EpisodeEnv, LEASE_KEYS, PROVIDER
from body_headless_suite import rng_environment, sha256, write_json
from body_observe_suite import provenance,native_worker_arguments

PROFILE = {"mode": "realtime", "task": "reset_stress", "seed": 223, "side": 0,
           "distance_cm": 400, "timeout_ms": 1500, "batch_size": 64, "query_batch_size": 8,
           "maximum_batches": 29, "maximum_terminal_polls": 64,
           "maximum_action_attempts": 4, "action_id": 11,
           "command_ttl_ms": 100, "poll_ms": 5, "max_ready_commands_per_due_tick": 1,
           "model": None, "quality_credit": False}


def require(condition, message):
    if not condition:
        raise ArenaInfrastructureError(message)


def lines(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines() if line]


def helper_inventory():
    return {name: {"path": str(Path(sys.modules[name].__file__).resolve()),
                   "sha256": sha256(Path(sys.modules[name].__file__))}
            for name in ("body_arena_env", "body_headless_suite", "body_observe_suite")}


def write_flood(leases):
    entities = list(leases)
    require(len(entities) == 2, "write flood requires two actual distinct physical leases")
    return [{"cmd": "body_heartbeat", **leases[entities[index%2]], "provider": PROVIDER}
            for index in range(PROFILE["batch_size"])]


class ProbeRunner(ArenaRunner):
    def __init__(self, args):
        require(sys.platform == "linux", "native probes run only on dev-ubuntu")
        self.args, self.buffer, self.sequence, self.samples = args, b"", 0, []
        self.stderr = (args.output/"runner-stderr.log").open("wb")
        self.ipc = (args.output/"ipc.jsonl").open("x")
        self.transport = (args.output/"collector-transport.jsonl").open("x")
        workdir = args.output/"run"; workdir.mkdir()
        (workdir/"Gothic.ini").write_bytes((args.reference/"run/Gothic.ini").read_bytes())
        argv = [str(args.runner), "-g", str(args.assets), "-devmode", "--save", str(args.save),
                "--scenario", str(args.scenario), "--output", str(args.output/"initial"),
                "--mode", "realtime", "--stdio", "--scheduling-diagnostics"]
        if args.compact_replies:
            argv.append("--compact-replies")
        argv.extend(native_worker_arguments(args))
        self.process = subprocess.Popen(argv, cwd=workdir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr, env=rng_environment("isolated"))

    def record(self, direction, value):
        now = time.monotonic_ns()
        self.ipc.write(json.dumps({"direction": direction, "wall_monotonic": now/1e9,
                                   "wall_monotonic_ns": now, "value": value}, separators=(",", ":"))+"\n")
        self.ipc.flush()
        self.transport.write(json.dumps({"direction": direction, "wall_monotonic_ns": now,
                                         "id": value.get("id")}, separators=(",", ":"))+"\n")
        self.transport.flush()

    def call(self, command):
        if command.get("cmd") in ("reset", "body_acquire"):
            command = {**command, "mode": "realtime"}
        return super().call(command)

    def batch(self, commands):
        require(1 <= len(commands) <= PROFILE["batch_size"], "IPC backlog exceeds 64 complete frames")
        requests = []
        for command in commands:
            self.sequence += 1
            request = {"id": self.sequence, **command}
            self.record("request", request)
            requests.append(request)
        # Submit only ordinary complete commands; then immediately read every
        # response. No sleeping reader or simulated deadline creates a backlog.
        self.process.stdin.write("".join(json.dumps(row)+"\n" for row in requests).encode())
        self.process.stdin.flush()
        responses = []
        deadline = time.monotonic()+10
        for request in requests:
            while b"\n" not in self.buffer:
                require(time.monotonic() < deadline, "bounded batch response watchdog expired")
                ready, _, _ = select.select([self.process.stdout], [], [], min(.5, max(0, deadline-time.monotonic())))
                if not ready:
                    require(self.process.poll() is None, "native runner exited during backlog")
                    continue
                chunk = os.read(self.process.stdout.fileno(), 65536)
                require(bool(chunk), "native runner closed its response stream")
                self.buffer += chunk
                require(len(self.buffer) <= 16*1024*1024, "native response exceeded 16 MiB bound")
            packet, self.buffer = self.buffer.split(b"\n", 1)
            response = json.loads(packet)
            self.record("response", response)
            require(response.get("id") == request["id"] and response.get("schema_version") == 1 and
                    response.get("ok") is not False, "native backlog response schema/ID/error differs")
            responses.append(response)
        return list(zip(requests, responses))

    def terminate(self):
        try:
            super().terminate()
        finally:
            self.transport.close()


class ProbeEnv(EpisodeEnv):
    def consume(self, response):
        if isinstance(response.get("arena"), dict):
            self._arena(response)
        rows = response.get("observations", [])
        for row in rows:
            if self.identity is not None:
                require(all(row.get(key) == getattr(self.identity, key) for key in ("session_epoch", "world_epoch")),
                        "numeric row has another episode identity")
            entity = row["entity_id"]
            require(row["observation_seq"] > self.processed_sequences.get(entity, 0), "numeric row repeated/backwards")
            self.processed_sequences[entity] = row["observation_seq"]
            if entity not in self.leases or all(row.get(key) == self.leases[entity][key] for key in LEASE_KEYS):
                self.latest[entity] = row
        return rows

    def pull(self):
        return tuple(self.consume(self.runner.call({"cmd": "body_observations", "max": 256})))


def audit_saved_output(output):
    """Read-only stdlib admission: actual raw clocks, strict tokens and cap one."""
    output = Path(output)
    manifest = json.loads((output/"manifest.json").read_text())
    result = json.loads((output/"probe.json").read_text())
    require(manifest.get("driver_sha256") == sha256(Path(__file__)) and
            manifest.get("scheduling_diagnostics") is True and manifest.get("model") is None and
            result.get("status") == "collected" and result.get("model") is None and
            result.get("profile") == PROFILE, "probe/helper/mode/model binding differs")
    for key in ("binary_sha256", "source_sha256", "engine_library_sha256"):
        require(isinstance(manifest.get(key), str) and len(manifest[key]) == 64, "native source/binary binding missing")
    inventory = manifest.get("helper_inventory", {})
    require(set(inventory) == set(helper_inventory()) and all(
        entry.get("sha256") == sha256(Path(entry["path"])) == helper_inventory()[name]["sha256"]
        for name, entry in inventory.items()), "actual stdlib harness helper inventory differs")
    ipc = lines(output/"episode/ipc.jsonl")
    requests, responses = {}, {}
    received_rows = {}
    for entry in ipc:
        value, stamp = entry["value"], entry["wall_monotonic_ns"]
        table = requests if entry["direction"] == "request" else responses
        require(value["id"] not in table and type(stamp) is int and stamp > 0, "duplicate IPC ID or invalid clock")
        table[value["id"]] = (value, stamp)
        for row in value.get("observations", []):
            key = tuple(row.get(field) for field in (*LEASE_KEYS, "observation_seq"))
            require(key not in received_rows, "duplicate wire numeric row")
            received_rows[key] = (row, stamp)
    require(set(requests) == set(responses) and len(requests) <= 2200, "incomplete or unbounded raw IPC")
    for identifier, (request, sent) in requests.items():
        response, received = responses[identifier]
        require(response.get("clock_domain") == "linux-clock-monotonic" and
                sent <= response["wall_monotonic_ns"] <= received and response.get("ok") is not False,
                "native IPC clock not bounded by actual collector clocks")
        require(all(response.get(key) is False for key in
                    ("graphics_device_created", "window_created", "audio_device_created")),
                "actual native response does not attest absence of graphics/window/audio devices")
        require(request.get("cmd") not in ("pause", "resume", "advance", "arena_step", "body_fixture"),
                "probe used a clock pause/forced physics fixture")
    scheduling = [row for row in lines(output/"episode/observer-timing.jsonl") if row.get("kind") == "runner_scheduling"]
    require(scheduling and [row["seq"] for row in scheduling] == list(range(1, len(scheduling)+1)),
            "native scheduling sequence dropped/duplicated")
    closing = result["close_response"]["scheduling_diagnostics"]
    require(closing.get("enabled") is True and closing.get("oracle_only") is True and
            closing.get("rows_emitted") == len(scheduling) and closing.get("dropped_total") == 0 and
            closing.get("capacity") == 65536, "native scheduling capture failed or dropped rows")
    ticks, active, reads, chains, due_counts = {}, None, {}, [], Counter()
    last_clock = 0
    for row in scheduling:
        stamp = row["wall_monotonic_ns"]
        require(type(stamp) is int and stamp >= last_clock and row.get("clock_domain") == "linux-clock-monotonic" and
                row.get("oracle_only") is True, "native scheduling clocks reversed or have another domain")
        last_clock = stamp
        if row["phase"] == "tick_begin":
            require(active is None and row["tick_id"] not in ticks, "native ticks overlap/duplicate")
            active = row; ticks[row["tick_id"]] = [row, None]
        elif row["phase"] == "tick_end":
            require(active is not None and active["tick_id"] == row["tick_id"] and
                    row["sim_step"] == active["sim_step"]+1 and row["game_ms"] >= active["game_ms"] and
                    row["tick_due_wall_monotonic_ns"] == active["tick_due_wall_monotonic_ns"], "tick end does not match real step")
            ticks[row["tick_id"]][1] = row; active = None
        elif row["phase"] == "input_read":
            require(active is None and row.get("request_id") in requests and row["request_id"] not in reads and
                    row.get("max_ready_commands_per_due_tick") == 1, "input read inside indivisible tick or has invalid cap/ID")
            request, sent = requests[row["request_id"]]; response, received = responses[row["request_id"]]
            require(row["command"] == request["cmd"] and sent <= stamp <= response["wall_monotonic_ns"] <= received,
                    "native input-read marker differs from actual request/receipt")
            reads[row["request_id"]] = row
            if row.get("admitted_before_due_tick") is True:
                due = row["tick_due_wall_monotonic_ns"]
                require(due <= stamp, "claimed ready admission precedes its due deadline")
                due_counts[(row["runner_generation"], due)] += 1
        else:
            raise ArenaInfrastructureError("unknown native scheduling phase")
    require(active is None and all(end is not None for _, end in ticks.values()), "unfinished native tick")
    require(set(reads) == {identifier for identifier, (request, _) in requests.items() if request["cmd"] != "reset"},
            "complete command lacks its native input-read marker")
    query_chains = []
    for identifier, row in reads.items():
        if row.get("admitted_before_due_tick") is not True:
            following = [(begin, end) for begin, end in ticks.values() if begin["seq"] > row["seq"]]
            if (row["command"] in ("body_observations", "status", "body_control_status", "body_events", "arena_status") and
                    row["wall_monotonic_ns"] >= row["tick_due_wall_monotonic_ns"] and following):
                begin, end = min(following, key=lambda pair: pair[0]["seq"])
                if begin["tick_due_wall_monotonic_ns"] == row["tick_due_wall_monotonic_ns"]:
                    response = responses[identifier][0]
                    require(row["wall_monotonic_ns"] <= begin["wall_monotonic_ns"] <= end["wall_monotonic_ns"] <= response["wall_monotonic_ns"] and
                            response["sim_step"] == end["sim_step"] and response["game_ms"] == end["game_ms"],
                            "queued numeric/status query replied before its due physical tick")
                    query_chains.append({"request_id": identifier, "command": row["command"],
                        "input_read_ns": row["wall_monotonic_ns"], "tick_begin_ns": begin["wall_monotonic_ns"],
                        "tick_end_ns": end["wall_monotonic_ns"], "response_ns": response["wall_monotonic_ns"],
                        "response_game_ms": response["game_ms"], "response_sim_step": response["sim_step"]})
            continue
        following = [(begin, end) for begin, end in ticks.values() if begin["seq"] > row["seq"]]
        require(following, "ready admission has no following actual tick")
        begin, end = min(following, key=lambda pair: pair[0]["seq"])
        require(begin["runner_generation"] == row["runner_generation"] and
                begin["tick_due_wall_monotonic_ns"] == row["tick_due_wall_monotonic_ns"] and
                row["wall_monotonic_ns"] <= responses[identifier][0]["wall_monotonic_ns"] <= begin["wall_monotonic_ns"],
                "ready command was not handled before the same actual due tick")
        chains.append({"request_id": identifier, "command": row["command"], "input_read_seq": row["seq"],
                       "tick_id": begin["tick_id"], "input_read_ns": row["wall_monotonic_ns"],
                       "tick_begin_ns": begin["wall_monotonic_ns"], "tick_end_ns": end["wall_monotonic_ns"],
                       "deadline_ns": row["tick_due_wall_monotonic_ns"]})
    require(chains and max(due_counts.values()) == 1, "no real ready admission or more than one command admitted per due tick")
    require(query_chains, "no actual queued query read/tick/fresh-reply chain")
    controls = lines(output/"episode/body-control-events.jsonl")
    attempts, accepted, rejected = [], [], []
    for identifier, (request, sent) in requests.items():
        if request["cmd"] != "body_action":
            continue
        key = tuple(request.get(field) for field in (*LEASE_KEYS, "observation_seq"))
        require(key in received_rows, "action token is not an actually received observation")
        row, received = received_rows[key]
        require(received <= sent and row.get("periodic") is True and row.get("can_decide") is True and
                row.get("action_mask_valid") is True and row["action_mask"][11] is True and
                request["action_id"] == 11 and request["ttl_ms"] == 100 and request["issued_game_ms"] == row["t_game_ms"] and
                request["goal_version"] == row["goal"]["version"] and
                request["target_entity_id"] == row["target"]["entity_id"], "action source/mask/clock changed")
        response = responses[identifier][0]; attempts.append(identifier)
        if response.get("accepted") is True:
            accepted.append(identifier)
            started = [event for event in controls if event.get("phase") == "started" and
                       all(event.get(field) == request[field] for field in LEASE_KEYS) and
                       event.get("command_id") == request["command_id"] and event.get("action_id") == 11 and
                       event.get("execution_id") == response.get("execution_id")]
            require(len(started) == 1, "accepted physical wait lacks its actual unique started hook")
        else:
            rejected.append(identifier)
            require(response.get("reason") in ("stale_observation", "arena_terminal"), "unexpected primitive refusal")
    require(accepted and len(attempts) <= 4, "no actual physical wait or action attempt budget exceeded")
    final = result["final"]
    require(final.get("outcome") == "time_limit" and final.get("infrastructure") is False and
            all(actor.get("hp") == 300 and actor.get("owner") == "scripted" and actor.get("release_pending") is False
                for actor in final["actors"]), "physical actors lost healthy ownership or clock outcome")
    checks = [{"name": "native_binary_source_helper_binding", "passed": True},
              {"name": "strict_native_clock_and_ready_admission_cap_one", "passed": True,
               "ready_chains": len(chains), "fresh_query_chains": len(query_chains)},
              {"name": "actual_periodic_action_mask_and_execution", "passed": True, "accepted": len(accepted), "rejected": len(rejected)},
              {"name": "healthy_real_time_actors_without_model_or_clock_pause", "passed": True}]
    artifacts = ("manifest.json", "probe.json", "episode/ipc.jsonl", "collector-transport.jsonl",
                 "episode/observer-timing.jsonl", "episode/observations.jsonl", "episode/body-control-events.jsonl")
    return {"schema_version": 1, "kind": "body_pending_input_saved_audit", "passed": True, "model": None,
            "checks": checks, "chains": chains, "query_chains": query_chains,
            "raw_sha256": {name: sha256(output/name) for name in artifacts}}


def run(args, config):
    runner = None
    result = {"schema_version": 1, "kind": "body_pending_input_probe", "status": "infrastructure_failure",
              "profile": PROFILE, "model": None, "quality_gate_evaluated": False, "batches": []}
    try:
        manifest = provenance(args)
        args.compact_replies = config.get("runtime", {}).get("compact_replies", False)
        manifest.update(kind=result["kind"], model=None, driver_sha256=sha256(Path(__file__)),
                        scheduling_diagnostics=True, compact_replies=args.compact_replies, profile=PROFILE,
                        helper_inventory=helper_inventory())
        write_json(args.output/"manifest.json", manifest)
        runner = ProbeRunner(args)
        env = ProbeEnv(runner, args.scenario)
        initial = env.reset(output=args.output/"episode", task=PROFILE["task"], seed=PROFILE["seed"], side=PROFILE["side"],
                            distance_cm=PROFILE["distance_cm"], timeout_ms=PROFILE["timeout_ms"], provider="scripted")
        result.update(initial=initial, leases=env.leases.copy())
        env._arena(runner.call({"cmd": "arena_start", "provider": "scripted"}))
        attempts = 0
        for batch in range(PROFILE["maximum_batches"]):
            if env.arena.get("terminated") or env.arena.get("truncated"):
                break
            queries = [{"cmd": "body_control_status"} for _ in range(6)] + [
                {"cmd": "body_events"}, {"cmd": "body_observations", "max": 256}]
            query_receipts = runner.batch(queries)
            result["batches"].append({"index": batch, "phase": "query",
                                      "request_ids": [request["id"] for request, _ in query_receipts]})
            for _, response in query_receipts:
                env.consume(response)
            if env.arena.get("terminated") or env.arena.get("truncated"):
                break
            primary = initial["primary_entity_id"]; row = env.latest.get(primary)
            if (attempts < 4 and row is not None and row.get("periodic") is True and env.eligible(primary, 11)):
                attempts += 1
                action = {"cmd": "body_action", **env.leases[primary], "provider": PROVIDER,
                                 "command_id": attempts, "action_id": 11, "observation_seq": row["observation_seq"],
                                 "issued_game_ms": row["t_game_ms"], "goal_version": row["goal"]["version"],
                                 "target_entity_id": row["target"]["entity_id"], "ttl_ms": 100}
                env.latest.pop(primary)
                action_receipts = runner.batch([action])
                for _, response in action_receipts:
                    env.consume(response)
            if env.arena.get("terminated") or env.arena.get("truncated"):
                break
            receipts = runner.batch(write_flood(env.leases))
            result["batches"].append({"index": batch, "phase": "write_flood",
                                      "request_ids": [request["id"] for request, _ in receipts]})
            for request, response in receipts:
                env.consume(response)
                if request["cmd"] == "body_heartbeat" and response.get("accepted") is not True:
                    require(response.get("reason") == "arena_terminal" and (env.arena.get("terminated") or env.arena.get("truncated")),
                            "actual healthy body lease heartbeat refused")
            time.sleep(PROFILE["poll_ms"]/1000)
        # The fixed flood does not inspect admission coverage or rerun a case.
        # Allow the same physical episode to reach its declared terminal clock
        # with bounded read-only polls; the engine keeps ticking independently.
        for _ in range(PROFILE["maximum_terminal_polls"]):
            if env.arena.get("terminated") or env.arena.get("truncated"):
                break
            time.sleep(.02)
            env.consume(runner.call({"cmd": "arena_status"}))
        require(env.arena.get("terminated") or env.arena.get("truncated"), "probe exceeded bounded actual command batches")
        final_response = runner.call({"cmd": "arena_status"}); result["final"] = env._arena(final_response)
        runner.call({"cmd": "body_observations", "max": 256})
        runner.call({"cmd": "body_events"})
        result["close_response"] = runner.call({"cmd": "close"})
        runner.process.wait(timeout=15)
        require(runner.process.returncode == 0, "native probe did not close cleanly")
        result["status"] = "collected"
        write_json(args.output/"probe.json", result)
        audit = audit_saved_output(args.output)
        write_json(args.output/"audit.json", audit)
        print(json.dumps({"status": result["status"], "checks": len(audit["checks"]), "ready_chains": len(audit["chains"])}), flush=True)
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print("pending input probe failed: "+str(error), file=sys.stderr, flush=True)
        return 2
    finally:
        if runner is not None:
            runner.terminate()
        write_json(args.output/"probe.json", result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-workers",dest="native_serial_workers",action="store_true")
    for name in ("runner", "assets", "save", "reference", "source", "engine-library", "scenario",
                 "melee-scenario", "native-reference", "config", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("real native probes run only on dev-ubuntu")
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    args.output.mkdir(exist_ok=False)
    return run(args, json.loads(args.config.read_text()))


if __name__ == "__main__":
    raise SystemExit(main())
