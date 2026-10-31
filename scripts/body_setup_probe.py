#!/usr/bin/env python3
"""Real headless checks for the reset-only stage-7 arena extensions."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys

from body_arena_env import ArenaRunner, EpisodeEnv, LEASE_KEYS, PROVIDER
from body_arena_suite import native_off
from body_headless_suite import service_snapshot, write_json
from body_observe_suite import provenance


def weapon_contract(runner, args):
    """Check reset profiles on the real runner without advancing a duel."""
    profiles = {}
    fields = ("symbol", "physical_side", "hp", "hp_max", "str", "dex", "onehand", "onehand_talent",
              "edge_protection", "blunt_protection", "weapon_symbol", "weapon", "armor_symbol", "armor",
              "weapon_damage", "weapon_length", "weapon_state", "immortal", "pose")
    legacy = {"hp": 300, "hp_max": 300, "str": 60, "dex": 60, "onehand": 60, "onehand_talent": 2,
              "edge_protection": 30, "blunt_protection": 30, "weapon": "ItMw_1h_Common_01",
              "armor": "ItAr_Leather_L", "weapon_state": 2, "immortal": False}
    for name, extra in (("default", {}), ("1h", {"weapon": "1h"}), ("2h", {"weapon": "2h"}),
                        ("unknown", {"weapon": "unknown"})):
        directory = args.output / ("weapon-" + name)
        directory.mkdir()
        response = runner.request({"cmd": "reset", "scenario": str(args.scenario), "output_dir": str(directory),
                                   "mode": "lockstep", "arena": {"scenario": "native_duel", "seed": 100000,
                                   "side": 0, "distance_cm": 400, "timeout_ms": 1000, **extra}}, allow_error=True)
        if name == "unknown":
            if response.get("ok") is not False:
                raise RuntimeError("Unknown arena weapon unexpectedly reset successfully")
            profiles[name] = {"rejected": True, "error": response.get("error")}
            continue
        if response.get("ok") is False or not response["arena"]["ready"] or response["arena"]["started"]:
            raise RuntimeError("Weapon reset did not yield an unstarted ready arena: " + name)
        actors = response["arena"]["actors"]
        if len(actors) != 2:
            raise RuntimeError("Weapon reset did not create two duel actors")
        for actor in actors:
            expected = legacy if name != "2h" else {
                key: value for key, value in legacy.items() if key not in ("weapon", "weapon_state", "onehand", "onehand_talent")}
            if any(actor[key] != value for key, value in expected.items()):
                raise RuntimeError("Weapon reset changed the canonical arena profile: " + name)
            if name == "2h" and (actor["weapon"].upper() != "ITMW_2H_BAU_AXE" or actor["weapon_state"] != 3 or
                                 actor.get("twohand") != 60 or actor.get("twohand_talent") != 2):
                raise RuntimeError("Two-handed reset did not equip the master axe profile")
        profiles[name] = [{key: actor[key] for key in fields} for actor in actors]
        if name == "2h":
            for profile, actor in zip(profiles[name], actors):
                profile.update(twohand=actor["twohand"], twohand_talent=actor["twohand_talent"])
    if profiles["default"] != profiles["1h"]:
        raise RuntimeError("Explicit one-handed reset differs from the legacy default profile")
    return profiles


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-workers",dest="native_serial_workers",action="store_true")
    for name in ("runner", "assets", "save", "reference", "source", "engine-library", "config", "scenario",
                 "melee-scenario", "native-reference", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Native checks run on dev-ubuntu, never on the development Mac")
    for name, value in vars(args).items():
        if isinstance(value,Path):setattr(args, name, value.resolve())
    args.output.mkdir(exist_ok=False)
    manifest = provenance(args)
    write_json(args.output / "manifest.json", manifest)
    runner = ArenaRunner(args)
    result = {"kind": "body_stage7_setup_probe", "status": "infrastructure_failure", "cases": []}
    try:
        result["weapon_contract"] = weapon_contract(runner, args)
        for name, extra, distance in (("yaw", {"yaw_offset_deg": 35, "center_offset_cm": [120, -120],
                                     "separation_yaw_deg": 25}, 650),
                                     ("wall-150", {"geometry": "near_wall"}, 150),
                                     ("wall-400", {"geometry": "near_wall"}, 400),
                                     ("wall-550", {"geometry": "near_wall"}, 550)):
            directory = args.output / name
            directory.mkdir()
            runner.episode_log(directory)
            response = runner.call({"cmd": "reset", "scenario": str(args.scenario), "output_dir": str(directory),
                                    "mode": "lockstep", "arena": {"scenario": "reset_stress", "seed": 17, "side": 0,
                                    "distance_cm": distance, "timeout_ms": 1000, **extra}})
            arena = response["arena"]
            if not arena["ready"] or arena["started"]:
                raise RuntimeError("Setup did not yield a stable unstarted arena")
            if "geometry" in extra:
                proof = arena["initial_state"]["setup"]["geometry_proof"]
                for flag in ("wall_ray_hit", "capsule_move_blocked", "capsule_block_is_land", "initial_capsules_free",
                             "safe_outward_move", "grounded_placements", "target_visible", "melee_line_clear"):
                    if proof.get(flag) is not True:
                        raise RuntimeError("Wall geometry proof lacks " + flag)
                if abs(proof["safe_height_delta_cm"]) > 50 or abs(proof["placement_height_delta_cm"]) > 50:
                    raise RuntimeError("Unsafe wall placement height difference")
            runner.sample()
            result["cases"].append({"name": name, "initial": arena})
            write_json(args.output / "probe.json", result)
        # Fair micro comparison keeps the opponent scripted for both native
        # and learned actors; prove the native ownership contract on real RPC.
        directory = args.output / "mixed-native"
        directory.mkdir()
        runner.episode_log(directory)
        arena = runner.call({"cmd": "reset", "scenario": str(args.scenario), "output_dir": str(directory),
                             "mode": "lockstep", "arena": {"scenario": "attack", "seed": 29, "side": 0,
                             "distance_cm": 150, "timeout_ms": 1000}})["arena"]
        actor = next(row for row in runner.call({"cmd": "body_control_status"})["actors"]
                     if row["entity_id"] == arena["opponent_entity_id"])
        acquired = runner.call({"cmd": "body_acquire", **{key: actor[key] for key in LEASE_KEYS},
                                "target_entity_id": actor["target_entity_id"], "provider": "stage7-setup-probe",
                                "body_control": True, "mode": "lockstep", "timeout_ms": 5000})
        if not acquired.get("accepted"):
            raise RuntimeError("Could not acquire the diagnostic opponent")
        rejected = {}
        for provider in ("native", "scripted"):
            rejected[provider] = runner.request({"cmd": "arena_start", "provider": provider}, allow_error=True)
            if rejected[provider].get("ok") is not False:
                raise RuntimeError("Invalid mixed ownership unexpectedly started " + provider)
        started = runner.call({"cmd": "arena_start", "provider": "native_with_scripted_opponent"})
        advanced = runner.call({"cmd": "arena_step", "substeps": 2})
        result["mixed_native"] = {"initial": arena, "acquired": acquired, "rejected": rejected,
                                  "started": started, "advanced": advanced}
        result["movement"] = []
        for mode in ("complete", "goal_cancel", "release"):
            env = EpisodeEnv(runner, args.scenario)
            initial = env.reset(output=args.output / ("movement-" + mode), task="reset_stress", seed=17,
                                side=0, distance_cm=650, timeout_ms=10000, provider="scripted")
            env.start()
            primary = initial["primary_entity_id"]
            for _ in range(10):
                if env.eligible(primary, 1):
                    break
                env.step()
            command_id = env.act(primary, 1)
            for _ in range(10):
                env.step()
                if any(row.get("entity_id") == primary and row.get("command_id") == command_id and
                       row.get("phase") == "started" for row in env.events):
                    break
            else:
                raise RuntimeError("Movement never really started")
            if mode == "complete":
                env.wait_command(primary, command_id)
                before = env.status()
                anchor = env.latest[primary]
                if anchor["target"]["in_w_range"] is not True:
                    raise RuntimeError("Approach completed outside physical weapon range")
                for _ in range(10):
                    env.step()
                after = env.status()
                positions = [next(row["pos"] for row in frame["actors"] if row["entity_id"] == primary)
                             for frame in (before, after)]
                drift = math.dist(*positions)
                if drift > 2:
                    raise RuntimeError("Completed approach kept applying run root motion")
                for _ in range(10):
                    if env.eligible(primary, 6):
                        break
                    env.step()
                strike = env.act(primary, 6)
                env.wait_command(primary, strike)
                final = env.status()
                actor = next(row for row in final["actors"] if row["entity_id"] == primary)
                if actor["outgoing_damage"] <= 0:
                    raise RuntimeError("A stopped approach could not execute a real melee hit")
                result["movement"].append({"mode": mode, "initial": initial, "completed": before,
                                           "held": after, "drift_cm": drift, "after_hit": final})
            elif mode == "goal_cancel":
                current = env.body_status(primary)
                response = runner.call({"cmd": "body_goal", **env.leases[primary], "provider": PROVIDER,
                                        "target_entity_id": current["target_entity_id"],
                                        "goal_version": current["goal_version"] + 1})
                if response.get("accepted") is not True:
                    raise RuntimeError("Movement cancellation goal was rejected")
                # Cancellation does not interrupt a protected Run-start
                # transition. Wait for the real safe cleanup boundary first.
                for _ in range(200):
                    env.step()
                    if env.body_status(primary)["can_decide"]:
                        break
                else:
                    raise RuntimeError("Cancelled movement never reached its bounded safe cleanup")
                before = env.status()
                for _ in range(10):
                    env.step()
                after = env.status()
                positions = [next(row["pos"] for row in frame["actors"] if row["entity_id"] == primary)
                             for frame in (before, after)]
                drift = math.dist(*positions)
                if drift > 2 or not env.body_status(primary)["can_decide"]:
                    raise RuntimeError("Cancelled approach left locomotion or a permanent pending cleanup")
                result["movement"].append({"mode": mode, "initial": initial, "response": response,
                                           "before": before, "after": after, "drift_cm": drift})
            else:
                response = runner.call({"cmd": "body_release", **env.leases[primary], "provider": PROVIDER})
                if response.get("accepted") is not True:
                    raise RuntimeError("Movement release was rejected")
                restored = None
                for _ in range(200):
                    runner.call({"cmd": "arena_step", "substeps": 1})
                    events = runner.call({"cmd": "body_events"})["events"]
                    restored = next((row for row in events if row.get("entity_id") == primary and
                                     row.get("reason") == "native_restored"), restored)
                    if restored is not None:
                        break
                if restored is None or any(token in restored["pose_identity"] for token in ("S_1HRUNL", "T_1HRUN_2_1HRUNL")):
                    raise RuntimeError("Native restoration retained unfinished owned locomotion")
                result["movement"].append({"mode": mode, "initial": initial, "response": response, "restored": restored})
            write_json(args.output / "probe.json", result)
        result["native_off"] = native_off(runner, args, manifest)
        runner.call({"cmd": "close"})
        runner.process.wait(timeout=15)
        if runner.process.returncode:
            raise RuntimeError("Runner did not close cleanly")
        subprocess.run([sys.executable, str(Path(__file__).with_name("body_headless_parity.py")),
                        str(args.native_reference), str(args.output / "native-off"), "--profile", "full",
                        "--output-dir", str(args.output / "parity")], check=True)
        result["native_parity"] = json.loads((args.output / "parity/headless-parity.json").read_text())
        result.update(status="complete", resources=runner.samples, services_after=service_snapshot())
        write_json(args.output / "probe.json", result)
        print(json.dumps({"status": result["status"], "cases": len(result["cases"]), "mixed_native": True}))
        return 0
    finally:
        runner.terminate()
        write_json(args.output / "probe.json", result)


if __name__ == "__main__":
    raise SystemExit(main())
