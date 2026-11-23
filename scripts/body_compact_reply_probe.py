#!/usr/bin/env python3
"""Actual compact runner transport proof; no model, inference or fake combat.

Run on dev-ubuntu while the caller holds the simulation lock. Preparation uses
the unchanged incoming-windup proof. Read-only RPCs must preserve every native
state value while omitting only the declared repeated diagnostics after play.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys

from body_arena_env import ArenaRunner, LEASE_KEYS
from body_block_reaction import preparation_proof, ready_anchor
from body_headless_suite import rng_environment, sha256, write_json
from body_observe_suite import provenance,native_worker_arguments

PROVIDER = "stage8-compact-reply-probe"
TRANSPORT_ONLY = {"id", "wall_monotonic_ns", "elapsed_wall_ms"}


class CompactRunner(ArenaRunner):
    def __init__(self, args):
        if sys.platform != "linux":
            raise RuntimeError("compact runner proof runs only on dev-ubuntu")
        self.args = args
        self.stderr = (args.output/"runner-stderr.log").open("wb")
        self.ipc = (args.output/"ipc.jsonl").open("w", encoding="utf-8")
        self.buffer, self.sequence, self.samples = b"", 0, []
        workdir = args.output/"run"
        workdir.mkdir()
        (workdir/"Gothic.ini").write_bytes((args.reference/"run/Gothic.ini").read_bytes())
        self.argv = [str(args.runner), "-g", str(args.assets), "-devmode", "--save", str(args.save),
                     "--scenario", str(args.scenario), "--output", str(args.output/"initial"),
                     "--mode", "lockstep", "--stdio", "--compact-replies"]
        self.argv.extend(native_worker_arguments(args))
        self.process = subprocess.Popen(self.argv, cwd=workdir, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr, env=rng_environment("isolated"))


def read_rows(path):
    with Path(path).open() as stream:
        return [json.loads(line) for line in stream if line.strip()]


def actor_status(response, entity):
    return next(row for row in response["body_control"]["actors"] if row["entity_id"] == entity)


def full_evidence(response):
    arena = response.get("arena", {})
    actors = response.get("body_control", {}).get("actors", [])
    return (isinstance(arena.get("initial_state"), dict) and bool(actors) and
            all(isinstance(row.get("purity_state", {}).get("rng_state"), str) and
                bool(row["purity_state"]["rng_state"]) for row in actors))


def compact_expected(response):
    result = deepcopy(response)
    for status in (result, result.get("body_control", {}), result.get("body_reply", {})):
        for actor in status.get("actors", []):
            if isinstance(actor.get("purity_state"), dict):
                actor["purity_state"].pop("rng_state", None)
    if isinstance(result.get("arena"), dict):
        result["arena"].pop("initial_state", None)
    return result


def state_envelope(response):
    # Native send stamps only this outer transport clock. Game/step/animation
    # clocks and all actor/control/observer/arena fields remain in comparison.
    return {key: deepcopy(value) for key, value in response.items() if key not in TRANSPORT_ONLY}


def same_native_state(full, compact):
    expected = compact_expected(full)
    for key, value in state_envelope(expected).items():
        if value != compact.get(key):
            raise RuntimeError("compact transport changed actual native state: "+key)
    if compact.get("arena", {}).get("initial_state") is not None:
        raise RuntimeError("active compact reply retained duplicate initial arena snapshot")
    if any("rng_state" in row.get("purity_state", {}) for row in compact["body_control"]["actors"]):
        raise RuntimeError("active compact reply retained repeated RNG diagnostics")


def audit_saved_output(output):
    output = Path(output)
    manifest = json.loads((output/"manifest.json").read_text())
    if sha256(Path(__file__)) != manifest["probe_sha256"] or any(
            sha256(Path(__file__).with_name(name)) != digest for name, digest in manifest["helper_sha256"].items()):
        raise RuntimeError("compact proof auditor or unchanged helper bytes differ from frozen provenance")
    if manifest.get("compact_replies") is not True or manifest["runner_argv"].count("--compact-replies") != 1:
        raise RuntimeError("actual native runner launch did not explicitly enable compact replies")
    saved = json.loads((output/"evidence.json").read_text())
    for key in ("initial", "start", "before", "armed", "activation", "terminal"):
        if not full_evidence(saved[key]):
            raise RuntimeError("required initial/held/armed/activation/terminal full evidence absent: "+key)
    primary = saved["initial"]["arena"]["primary_entity_id"]
    controls = read_rows(output/"episode/body-control-events.jsonl")
    mechanics = read_rows(output/"episode/events.jsonl")
    anchor = saved["anchor"]
    prepared_game = anchor["t_game_ms"]
    proof = preparation_proof(saved["initial"]["arena"], saved["armed"]["arena"],
                              actor_status(saved["before"], primary), actor_status(saved["armed"], primary),
                              [row for row in controls if row.get("t_game_ms", 0) <= prepared_game and
                               row.get("reason") not in ("reaction_mode_armed", "release_pending", "native_restored")],
                              [row for row in mechanics if row.get("t_game", 0) <= prepared_game], anchor)
    activation = saved["activation"]
    if (activation["game_ms"] != prepared_game or activation["arena"]["reaction"]["phase"] != "active" or
            activation["arena"]["reaction"]["activation_provider"] != "native" or
            actor_status(activation, primary)["owner"] != "native"):
        raise RuntimeError("actual native reaction activation changed the prepared clock or failed authority handoff")
    for key in ("active_status", "active_body_status"):
        same_native_state(activation, saved[key])
    # body_control_status repeats the common native encoder at its root and in
    # body_reply. Both copies must differ only by the exact diagnostic string.
    expected_body = compact_expected({"body_control": activation["body_control"]})["body_control"]
    if saved["active_body_status"].get("body_reply") != expected_body:
        raise RuntimeError("compact common encoder receipt changed more than the allowed RNG strings")
    if any(saved["active_body_status"].get(key) != value for key, value in expected_body.items()):
        raise RuntimeError("compact top-level native status copy differs from its common encoder")
    terminal = saved["terminal"]
    if terminal["arena"]["outcome"] not in ("task_success", "time_limit", "death_or_down") or not terminal.get("done"):
        raise RuntimeError("actual native episode did not reach a real terminal state")
    if any(terminal[key] is not False for key in ("graphics_device_created", "window_created", "audio_device_created")):
        raise RuntimeError("runner created a rendering/audio device")
    return {"preparation": proof, "checks": {name: True for name in
            ("actual_compact_flag", "held_and_armed_full_evidence", "held_primary_rng_unchanged",
             "no_primary_preparation_choices_or_commands", "real_incoming_attack_anchor",
             "activation_full_and_same_game_clock", "active_reply_only_allowed_pruning",
             "numeric_masks_pose_hp_clocks_preserved", "terminal_full_evidence", "no_rendering_devices")}}


def run(runner, args, result):
    episode = args.output/"episode"
    episode.mkdir()
    runner.episode_log(episode)
    evidence = {}
    evidence["initial"] = runner.call({"cmd": "reset", "scenario": str(args.scenario), "mode": "lockstep",
        "output_dir": str(episode), "arena": {"scenario": "block", "seed": 223, "side": 1,
        "distance_cm": 150, "timeout_ms": 1000, "reaction_hold": True}})
    arena = evidence["initial"]["arena"]
    primary, opponent = arena["primary_entity_id"], arena["opponent_entity_id"]
    leases = {}
    for actor in evidence["initial"]["body_control"]["actors"]:
        reply = runner.call({"cmd": "body_acquire", **{key: actor[key] for key in LEASE_KEYS},
            "target_entity_id": actor["target_entity_id"], "provider": PROVIDER,
            "body_control": True, "mode": "lockstep", "timeout_ms": 5000})
        if reply.get("accepted") is not True:
            raise RuntimeError("real compact probe lease refused")
        leases[actor["entity_id"]] = {key: reply[key] for key in LEASE_KEYS}
    evidence["before"] = runner.call({"cmd": "body_control_status"})
    evidence["start"] = runner.call({"cmd": "arena_start", "provider": "scripted"})
    latest, sent = {}, False
    for _ in range(8):
        if not sent and opponent in latest and latest[opponent].get("can_decide") and latest[opponent]["action_mask"][6]:
            row = latest[opponent]
            request = {"cmd": "body_action", **leases[opponent], "provider": PROVIDER,
                "goal_version": row["goal"]["version"], "target_entity_id": primary, "command_id": 1,
                "action_id": 6, "observation_seq": row["observation_seq"], "issued_game_ms": row["t_game_ms"], "ttl_ms": 1000}
            reply = runner.call(request)
            if reply.get("accepted") is not True:
                raise RuntimeError("real incoming front attack was refused")
            evidence["attacker"] = {"request": request, "response": reply}
            sent = True
        stepped = runner.call({"cmd": "arena_step", "substeps": 1})
        packet = runner.call({"cmd": "body_observations", "max": 256})
        for row in packet["observations"]:
            if row.get("periodic"):
                latest[row["entity_id"]] = row
        if stepped["arena"]["reaction"]["phase"] == "armed":
            evidence["armed"] = runner.call({"cmd": "body_control_status"})
            evidence["anchor"] = latest.get(primary)
            if not sent or not ready_anchor(evidence["anchor"], evidence["armed"]["arena"]):
                raise RuntimeError("engine armed without a real observed incoming windup")
            break
    else:
        raise RuntimeError("no genuine armed reaction within eight native steps")
    anchor = evidence["anchor"]
    evidence["activation"] = runner.call({"cmd": "arena_reaction_activate",
        "session_epoch": arena["session_epoch"], "world_epoch": arena["world_epoch"],
        "primary_entity_id": primary, "opponent_entity_id": opponent,
        "primary_control_generation": leases[primary]["control_generation"],
        "opponent_control_generation": leases[opponent]["control_generation"],
        "observation_seq": anchor["observation_seq"], "issued_game_ms": anchor["t_game_ms"],
        "provider": "native", "mode": "lockstep"})
    evidence["active_status"] = runner.call({"cmd": "status"})
    evidence["active_body_status"] = runner.call({"cmd": "body_control_status"})
    for _ in range(40):
        current = runner.call({"cmd": "arena_step", "substeps": 1})
        runner.call({"cmd": "body_observations", "max": 256})
        if current.get("done"):
            break
    else:
        raise RuntimeError("native compact episode failed its bounded terminal")
    evidence["terminal"] = runner.call({"cmd": "body_control_status"})
    runner.sample()
    write_json(args.output/"evidence.json", evidence)
    runner.call({"cmd": "close"})
    if runner.process.wait(timeout=20) != 0:
        raise RuntimeError("compact native runner failed to close")
    result.update(audit_saved_output(args.output))
    result["journal_sha256"] = {name: sha256(episode/name) for name in
        ("events.jsonl", "observations.jsonl", "body-control-events.jsonl", "runner-summary.json", "ipc.jsonl")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-workers",dest="native_serial_workers",action="store_true")
    for name in ("runner", "assets", "save", "reference", "source", "engine-library", "config", "scenario",
                 "melee-scenario", "native-reference", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("compact proof runs only on dev-ubuntu")
    for name, value in vars(args).items():
        if isinstance(value,Path):setattr(args, name, value.resolve())
    args.output.mkdir(exist_ok=False)
    result = {"schema_version": 1, "kind": "body_compact_reply_probe", "status": "failed",
              "model": None, "quality_credit": False, "historical_owned_equivalence": False}
    runner = None
    try:
        manifest = {**provenance(args), "kind": result["kind"], "compact_replies": True,
            "probe_sha256": sha256(Path(__file__)), "helper_sha256": {name: sha256(Path(__file__).with_name(name))
            for name in ("body_block_reaction.py", "body_arena_env.py", "body_observe_suite.py", "body_headless_suite.py")}}
        runner = CompactRunner(args)
        manifest["runner_argv"] = runner.argv
        write_json(args.output/"manifest.json", manifest)
        result["runner_pid"] = runner.process.pid
        run(runner, args, result)
        result["status"] = "passed"
    except Exception as error:
        result["error"] = str(error)
    finally:
        if runner is not None:
            runner.terminate()
        result["imported_model_modules"] = [name for name in sys.modules if name.split(".")[0] in
            ("torch", "tensorflow", "jax", "onnxruntime")]
        if result["imported_model_modules"]:
            result.update(status="failed", error="unexpected model library import")
        write_json(args.output/"probe.json", result)
    print(json.dumps({"status": result["status"], "error": result.get("error"), "model": None}))
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
